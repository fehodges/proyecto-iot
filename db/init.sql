-- PostgreSQL lo ejecuta solo la primera vez, cuando el volumen pg-data está vacío.
-- Si lo modifican: docker compose down -v && docker compose up -d --build

CREATE TABLE clientes (
  id      TEXT PRIMARY KEY,
  nombre  TEXT NOT NULL,
  email   TEXT
);

-- Un dispositivo (NodeMCU) = un sensor registrado.
-- Envía varios tipos de lectura: temperatura, humedad, gas y llama.
CREATE TABLE sensores (
  id          TEXT PRIMARY KEY,
  cliente_id  TEXT NOT NULL REFERENCES clientes(id),
  ubicacion   TEXT,
  tipo        TEXT,
  ciclo       INT DEFAULT 30
);

-- Reglas de alerta: condicion = 'mayor' o 'menor'
CREATE TABLE alertas (
  id            SERIAL PRIMARY KEY,
  sensor_id     TEXT REFERENCES sensores(id),
  tipo          TEXT NOT NULL,
  condicion     TEXT NOT NULL CHECK (condicion IN ('mayor', 'menor')),
  umbral        REAL NOT NULL,
  destinatario  TEXT
);

INSERT INTO clientes VALUES ('cliente-007', 'Duoc Sede AV', 'lab@duoc.cl');

INSERT INTO sensores VALUES
  ('dc-rack-01', 'cliente-007', 'Sala servidores - Rack 1', 'ambiental', 30);

-- Los mismos umbrales de las tarjetas del proyecto
INSERT INTO alertas (sensor_id, tipo, condicion, umbral, destinatario) VALUES
  ('dc-rack-01', 'temperatura', 'mayor', 30,  'lab@duoc.cl'),
  ('dc-rack-01', 'humedad',     'mayor', 70,  'lab@duoc.cl'),
  ('dc-rack-01', 'gas',         'mayor', 400, 'lab@duoc.cl'),
  ('dc-rack-01', 'llama',       'mayor', 0.5, 'lab@duoc.cl');
