"""API de monitoreo ambiental para datacenter — SIY6122.

Recibe lecturas del dispositivo (NodeMCU) en POST /api/v1/lecturas,
las valida y las guarda en InfluxDB. PostgreSQL guarda quién es quién
(clientes, sensores y reglas de alerta).

Tipos de lectura que acepta:
  temperatura  -> DHT11, en °C
  humedad      -> DHT11, en % HR
  gas          -> MQ-2, valor crudo del ADC (0 a 1023)
  llama        -> sensor de llama, 0 = sin llama, 1 = llama detectada

Si no existen INFLUX_URL ni DATABASE_URL, la API funciona sola y guarda
las últimas 20 lecturas en memoria (igual que en la Parte 3 del lab).
"""

import logging
import os
from datetime import datetime, timezone
from typing import Literal

from fastapi import FastAPI, HTTPException
from pydantic import BaseModel, Field

logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(message)s")
log = logging.getLogger("api")

app = FastAPI(title="API Monitoreo Datacenter SIY6122")

INFLUX_URL = os.getenv("INFLUX_URL")
INFLUX_TOKEN = os.getenv("DOCKER_INFLUXDB_INIT_ADMIN_TOKEN")
INFLUX_ORG = os.getenv("DOCKER_INFLUXDB_INIT_ORG", "duoc")
INFLUX_BUCKET = os.getenv("DOCKER_INFLUXDB_INIT_BUCKET", "sensores")
DATABASE_URL = os.getenv("DATABASE_URL")

# Rango físico aceptado para cada tipo de lectura
RANGOS = {
    "temperatura": (-40, 80),
    "humedad": (0, 100),
    "gas": (0, 1023),
    "llama": (0, 1),
}

# Unidad que debe acompañar a cada tipo
UNIDADES = {
    "temperatura": "C",
    "humedad": "%",
    "gas": "raw",
    "llama": "bool",
}

ultimas = []  # últimas 20 lecturas en memoria


class Lectura(BaseModel):
    sensor: str = Field(min_length=3, max_length=40)
    cliente: str = Field(min_length=1, max_length=40)
    ubicacion: str = Field(min_length=1, max_length=60)
    tipo: Literal["temperatura", "humedad", "gas", "llama"]
    valor: float
    unidad: str = Field(max_length=10)


def sensor_registrado(sensor: str, cliente: str) -> bool:
    if not DATABASE_URL:
        return True  # sin PostgreSQL se aceptan todos los sensores
    import psycopg

    with psycopg.connect(DATABASE_URL) as conn:
        fila = conn.execute(
            "SELECT 1 FROM sensores WHERE id = %s AND cliente_id = %s",
            (sensor, cliente),
        ).fetchone()
    return fila is not None


def evaluar_alertas(l: Lectura) -> list[dict]:
    """Revisa las reglas de la tabla alertas para este sensor y tipo.

    Devuelve las reglas que se cumplen. Por ahora solo se registran en el
    log y se devuelven en la respuesta; el envío de correo o Telegram es
    el paso siguiente (o se hace con Grafana Alerting).
    """
    if not DATABASE_URL:
        return []
    import psycopg

    with psycopg.connect(DATABASE_URL) as conn:
        reglas = conn.execute(
            "SELECT condicion, umbral, destinatario FROM alertas "
            "WHERE sensor_id = %s AND tipo = %s",
            (l.sensor, l.tipo),
        ).fetchall()

    disparadas = []
    for condicion, umbral, destinatario in reglas:
        if (condicion == "mayor" and l.valor > umbral) or (
            condicion == "menor" and l.valor < umbral
        ):
            disparadas.append(
                {
                    "tipo": l.tipo,
                    "condicion": condicion,
                    "umbral": umbral,
                    "valor": l.valor,
                    "destinatario": destinatario,
                }
            )
            log.warning(
                "ALERTA %s: %s=%s (%s que %s) -> avisar a %s",
                l.sensor, l.tipo, l.valor, condicion, umbral, destinatario,
            )
    return disparadas


def guardar_en_influx(l: Lectura, ts: datetime) -> None:
    if not INFLUX_URL:
        return
    from influxdb_client import InfluxDBClient, Point
    from influxdb_client.client.write_api import SYNCHRONOUS

    punto = (
        Point(l.tipo)
        .tag("sensor", l.sensor)
        .tag("cliente", l.cliente)
        .tag("ubicacion", l.ubicacion)
        .field("valor", float(l.valor))
        .time(ts)
    )
    with InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG) as cliente:
        cliente.write_api(write_options=SYNCHRONOUS).write(bucket=INFLUX_BUCKET, record=punto)


@app.get("/salud")
def salud():
    return {"estado": "ok", "influx": bool(INFLUX_URL), "postgres": bool(DATABASE_URL)}


@app.post("/api/v1/lecturas", status_code=201)
def recibir(l: Lectura):
    if not sensor_registrado(l.sensor, l.cliente):
        raise HTTPException(403, "sensor no registrado")

    esperada = UNIDADES[l.tipo]
    if l.unidad != esperada:
        raise HTTPException(422, f"unidad incorrecta para {l.tipo}: se esperaba '{esperada}'")

    minimo, maximo = RANGOS[l.tipo]
    if not minimo <= l.valor <= maximo:
        raise HTTPException(422, f"valor fuera de rango ({minimo} a {maximo})")

    if l.tipo == "llama" and l.valor not in (0, 1):
        raise HTTPException(422, "llama solo acepta 0 o 1")

    ts = datetime.now(timezone.utc)
    guardar_en_influx(l, ts)
    alertas = evaluar_alertas(l)

    ultimas.append({**l.model_dump(), "timestamp": ts.isoformat()})
    del ultimas[:-20]
    return {"estado": "guardado", "timestamp": ts.isoformat(), "alertas": alertas}


@app.get("/api/v1/lecturas")
def listar():
    return ultimas
