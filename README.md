# Monitoreo ambiental de datacenter — SIY6122

Un NodeMCU (ESP8266) mide temperatura, humedad, gas/humo y llama dentro de un gabinete de servidores. Envía las lecturas por WiFi a una API en Docker, que las valida y las guarda en InfluxDB. Grafana las muestra en un dashboard y **genera las alertas**. El dispositivo solo mide y envía: no tiene LEDs ni botones.

```
NodeMCU + sensores ──POST JSON──▶ API FastAPI (:8000) ──▶ InfluxDB (lecturas)
  portal cautivo                       │                       ▲
                                       ▼                       │
                                  PostgreSQL              Grafana (:3000)
                          (clientes, sensores, alertas)
```

| Sensor | Qué vigila | Tipo de lectura | Alerta |
|---|---|---|---|
| DHT11 | Sobrecalentamiento del gabinete | `temperatura` (°C) | sobre 30 °C |
| DHT11 | Riesgo de condensación | `humedad` (% HR) | sobre 70 % |
| MQ-2 | Combustión incipiente (gas/humo) | `gas` (0–1023) | sobre 400 |
| Sensor de llama | Incendio declarado | `llama` (0 o 1) | 1 |

## Estructura

```
proyecto-iot/
├── docker-compose.yml
├── .env.example
├── simular.sh                  # simula el dispositivo con curl
├── api/                        # FastAPI: recibe y valida
├── db/init.sql                 # clientes, sensores y reglas de alerta
├── grafana/
│   ├── provisioning/           # datasource InfluxDB, dashboard y reglas de alerta, se cargan solos
│   └── dashboards/datacenter.json
├── firmware/sensor_datacenter/sensor_datacenter.ino
└── docs/diagrama-sensores.png  # diagrama de conexiones
```

---

## 1. Levantar el servidor (en tres comandos)

En su PC con Docker Desktop, o en la VM/EC2 del lab:

```bash
cp .env.example .env        # y cambien las claves
docker compose up -d --build
docker compose ps           # postgres e influxdb deben quedar (healthy)
```

Probar:

```bash
curl http://localhost:8000/salud     # influx y postgres deben decir true
./simular.sh                         # 30 envíos, con un "incendio" en la lectura 20
```

Abrir Grafana en `http://<ip>:3000` (usuario `admin`, clave `GF_SECURITY_ADMIN_PASSWORD` del `.env`). El dashboard **Datacenter › Monitoreo Datacenter** ya viene conectado a InfluxDB: no hay que configurar nada a mano. Arriba elijan el cliente y el sensor.

En EC2 abran en el Security Group solo los puertos 22 (su IP), 8000 y 3000, como en el lab.

## 2. Contrato de la API

`POST /api/v1/lecturas` — el mismo de la Clase 10, con dos tipos nuevos (`gas` y `llama`):

```json
{
  "sensor": "dc-rack-01",
  "cliente": "cliente-007",
  "ubicacion": "Sala servidores - Rack 1",
  "tipo": "temperatura",
  "valor": 24.5,
  "unidad": "C"
}
```

| `tipo` | `unidad` | Rango válido |
|---|---|---|
| `temperatura` | `C` | −40 a 80 |
| `humedad` | `%` | 0 a 100 |
| `gas` | `raw` | 0 a 1023 |
| `llama` | `bool` | solo 0 o 1 |

| Código | Cuándo |
|---|---|
| 201 | Guardado |
| 403 | El sensor no está registrado en PostgreSQL para ese cliente |
| 422 | Falta un campo, tipo desconocido, unidad incorrecta o valor fuera de rango |

El dispositivo hace 4 POST por ciclo (uno por tipo). El timestamp lo pone la API.

**Registrar otro dispositivo** (si no, la API responde 403):

```bash
docker compose exec postgres psql -U iot -d iot -c \
  "INSERT INTO sensores VALUES ('dc-rack-02','cliente-007','Sala servidores - Rack 2','ambiental',10);"
```

---

## 3. Armar el dispositivo

Diagramas: `docs/diagrama-sensores.png` (conexiones) y `docs/detalle-resistencias.png` (divisor del MQ-2).

**Materiales:** NodeMCU, DHT11, MQ-2, sensor de llama, 2 resistencias **iguales** de 2,2 kΩ (rojo-rojo-rojo), protoboard y cables. El Arduino Uno no se usa porque no tiene WiFi.

> **Importante:** el NodeMCU trabaja a **3,3 V**. El DHT11 y el sensor de llama se alimentan desde **3V**; solo el MQ-2 va a 5 V (**VU**).

- El NodeMCU va **fuera de la protoboard** (es muy ancho y la taparía entera), conectado con cables hembra-macho.
- Las líneas + y − de la protoboard están cortadas cerca del 30: dos **puentes** (rojo + con +, negro − con −) unen ambas mitades para usar toda la protoboard.
- Los sensores van **separados**: el MQ-2 se calienta y, pegado al DHT11, altera la temperatura. El MQ-2 puede ir fuera de la protoboard con cables hembra-macho.

| Componente | Pin del componente | Conexión |
|---|---|---|
| **NodeMCU** | 3V / G | línea **roja (+)** / línea **azul (−)** |
| **Puentes** | + col. 30 → + col. 32 · − col. 30 → − col. 34 | unen las dos mitades |
| **DHT11** (C5–C7) | + / out / − | línea roja / **D5** / línea azul |
| **Sensor de llama** (C25–C28) | AO / DO / GND / VCC | nada / **D6** / línea azul / línea roja |
| **MQ-2** (C50–C53 o fuera) | AO / DO / GND / VCC | **divisor de voltaje → A0** / nada / línea azul / **VU** del NodeMCU |

**Divisor de voltaje del MQ-2** (la salida AO llega a casi 5 V y A0 acepta hasta 3,2 V):

```
AO del MQ-2 ── resistencia 2,2 kΩ ──┬── A0 del NodeMCU
                                    │
                             resistencia 2,2 kΩ
                                    │
                                   GND
```

Con dos resistencias iguales, a A0 llega la mitad del voltaje (máximo 2,5 V).

Notas:

- Los pines de cada sensor van en la **misma letra** y en **números seguidos** (cada pin en su propio número). Los cables salen de la fila A del mismo número.
- El sensor de llama y el MQ-2 tienen el mismo orden impreso: **AO · DO · GND · VCC**. El DHT11: **+ · out · −**. Si su módulo es distinto, guíense por lo impreso.
- Las patas de las resistencias no deben tocarse entre sí ni tocar otras patas.

## 4. Cargar el firmware

1. Arduino IDE → **Archivo → Preferencias → Gestor de URLs adicionales de tarjetas**, peguen:
   `https://arduino.esp8266.com/stable/package_esp8266com_index.json`
2. **Herramientas → Placa → Gestor de tarjetas**, busquen **esp8266** e instálenlo.
3. **Herramientas → Administrar bibliotecas**, instalen:
   - **WiFiManager** (tzapu)
   - **ArduinoJson** (Benoit Blanchon), versión 7
   - **DHT sensor library** (Adafruit), y acepten **Adafruit Unified Sensor**
4. **Herramientas → Placa → NodeMCU 1.0 (ESP-12E Module)** y elijan el puerto. Si no aparece, instalen el driver **CH340** (o **CP2102**, según el chip USB de su placa).
5. Abran `firmware/sensor_datacenter/sensor_datacenter.ino` y presionen **Subir**.
6. **Monitor Serie a 115200 baudios.**

Probado: compila sin errores para NodeMCU 1.0 con el core esp8266 3.1.2.

## 5. Configurar con el portal cautivo

1. Al encender sin configuración aparece la red WiFi **SENSOR-XXXX**.
2. Conéctense con el celular: se abre el portal (o entren a `192.168.4.1`).
3. **Configure WiFi** → elijan la red y su clave, y completen:

| Campo | Ejemplo |
|---|---|
| Servidor | `http://54.12.34.56:8000` (IP elástica) o `http://192.168.1.50:8000` (su notebook) |
| Ciclo de envío | `10` |
| Nombre del sensor | `dc-rack-01` (debe existir en la tabla `sensores`) |
| ID cliente | `cliente-007` |
| Ubicación | `Sala servidores - Rack 1` |

4. **Save**: guarda todo en la flash y se reinicia conectado.

Para volver a configurarlo: **mantengan presionado 5 segundos el botón FLASH** que trae la placa. Se borra la configuración y vuelve a aparecer el portal.

**Sobre la red WiFi:**
- El ESP8266 solo se conecta a redes de **2,4 GHz**, no de 5 GHz.
- No entra a redes con usuario y clave personal (WPA2-Enterprise, como la de muchas sedes) ni con página de inicio de sesión. Usen el **punto de acceso del celular**.
- Si el servidor corre en su notebook, el NodeMCU y el notebook deben estar en la misma red, y el firewall de Windows debe permitir el puerto 8000.

## 6. Funcionamiento

- Cada ciclo envía 4 lecturas y escribe en el Monitor Serie:
  ```
  Temp: 24.0°C / 75.2°F | Humedad: 55%
  Gas: 210 | Llama: No
    POST temperatura -> 201 guardado
    POST humedad -> 201 guardado
    POST gas -> 201 guardado
    POST llama -> 201 guardado
  ```
- Gas y llama se revisan cada 200 ms. Si aparece llama o el gas cruza 400, se envía **de inmediato**, sin esperar el ciclo, para que Grafana alerte antes.
- Si la API responde un error, se imprime su explicación (por ejemplo `403 {"detail":"sensor no registrado"}`) y se reintenta en el ciclo siguiente.

## 7. Alertas en Grafana

Las reglas vienen listas en `grafana/provisioning/alerting/alertas.yml` y se ven en **Alerting → Alert rules** (carpeta Datacenter):

| Regla | Condición | Espera antes de avisar |
|---|---|---|
| Temperatura alta | sobre 30 °C | 1 min |
| Humedad alta | sobre 70 % | 2 min |
| Gas o humo | MQ-2 sobre 400 | inmediato |
| FUEGO detectado | llama = 1 | inmediato |
| Dispositivo sin datos | nada en 2 min | inmediato |

**Recibir las notificaciones (Telegram, lo más simple):**

1. En Telegram, hablen con **@BotFather** → `/newbot` → copien el **token**.
2. Escríbanle cualquier mensaje a su bot. Luego abran `https://api.telegram.org/bot<TOKEN>/getUpdates` y copien el número de `"chat":{"id":...}`.
3. En Grafana: **Alerting → Contact points → Add contact point**, tipo **Telegram**, peguen el token y el chat ID → **Test** → **Save**.
4. **Alerting → Notification policies → Default policy → Edit** y elijan ese contact point.

También sirve **Email** (requiere configurar SMTP en Grafana) o **Webhook**.

**Probar:** acerquen un encendedor prendido al sensor de llama. En menos de un minuto la regla "FUEGO detectado" pasa a *Firing* y llega el mensaje. También pueden probar sin el dispositivo con `./simular.sh`, que simula un incendio en la lectura 20.

## 8. Calibración

- **MQ-2:** con el divisor, en aire limpio debería marcar entre 30 y 300 aprox. Déjenlo encendido unos minutos. Anoten el valor de `Gas` en aire limpio y con gas de encendedor **sin prender**. Pongan ese valor intermedio en `GAS_AVISO` del firmware, en la regla "Gas o humo" de `alertas.yml` y en el umbral del dashboard.
- **Sensor de llama:** giren el potenciómetro azul hasta que el LED del módulo se apague sin llama y se prenda con un encendedor a 20–30 cm. Si el Monitor Serie dice `Llama: SI` sin fuego, cambien `FLAMA_DETECTADA` a `HIGH`.

Prueben con fuego y gas solo en pequeñas cantidades, lejos de papeles y con supervisión.

## 9. Problemas frecuentes

| Síntoma | Qué revisar |
|---|---|
| `error de conexión: connection refused` | IP o puerto del servidor; Security Group / firewall; `docker compose ps` |
| `403 sensor no registrado` | El nombre del sensor o cliente del portal no está en la tabla `sensores` |
| `DHT11: error de lectura` | Cable S en D5 y alimentación en 3V3 |
| Gas siempre en 0 | La resistencia de arriba del divisor no toca el cable AO, o A0 quedó unido a GND |
| Gas cerca de 1023 | La resistencia de abajo del divisor no llega a GND: no acercar gas hasta corregirlo |
| El WiFi se corta | Fuente USB débil: el MQ-2 consume ~150 mA; usen otro cable o puerto |
| Grafana sin datos | Elegir cliente y sensor arriba; rango "Last 1 hour"; `docker compose logs api` |
| No aparece la red SENSOR-XXXX | Mantengan el botón FLASH 5 s para borrar la configuración |
| La alerta no llega a Telegram | Contact point probado con **Test** y asignado en la Default policy |

## Pendientes antes de producción

- Autenticación del dispositivo (API key por sensor).
- HTTPS entre el dispositivo y la API.
