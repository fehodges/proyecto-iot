/*
  Monitoreo ambiental de datacenter — SIY6122
  Placa: NodeMCU 1.0 (ESP-12E), ESP8266

  El dispositivo solo MIDE y ENVÍA. Las alertas las genera Grafana.

  Sensores:
    DHT11            -> temperatura y humedad   (pin D5)
    Sensor de llama  -> fuego visible           (pin D6, salida DO)
    MQ-2             -> gas / humo              (pin A0, salida AO con resistencia 100 kΩ)

  Funcionamiento:
    1. Primer encendido (o sin configuración): crea la red WiFi SENSOR-XXXX.
       Te conectas con el celular, se abre el portal cautivo (192.168.4.1)
       y completas: WiFi, servidor, ciclo, nombre del sensor, cliente y ubicación.
    2. Guarda la configuración en la memoria flash y se conecta a tu WiFi.
    3. Cada <ciclo> segundos lee los sensores y envía 4 POST JSON a
       <servidor>/api/v1/lecturas (temperatura, humedad, gas y llama).
    4. Si aparece llama o el gas sube de golpe, envía de inmediato,
       sin esperar el ciclo, para que Grafana alerte antes.

  Para volver a configurar: mantener presionado 5 s el botón FLASH de la placa.

  Librerías (Arduino IDE -> Herramientas -> Administrar bibliotecas):
    - WiFiManager            (tzapu)
    - ArduinoJson            (Benoit Blanchon), versión 7
    - DHT sensor library     (Adafruit)  + Adafruit Unified Sensor
*/

#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiManager.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <DHT.h>

// ---------------- Pines ----------------
#define PIN_DHT    D5      // DHT11, pin out / S
#define PIN_FLAMA  D6      // sensor de llama, pin DO
#define PIN_GAS    A0      // MQ-2, pin AO (a través de la resistencia de 100 kΩ)
#define PIN_FLASH  D3      // botón FLASH que ya trae la placa (no se conecta nada)

#define TIPO_DHT DHT11

const int FLAMA_DETECTADA = LOW;   // cambiar a HIGH si tu módulo funciona al revés
const int GAS_AVISO = 400;         // sobre este valor se envía de inmediato

// ---------------- Configuración (se edita en el portal cautivo) ----------------
struct Config {
  char servidor[80]  = "http://192.168.1.100:8000";
  char ciclo[6]      = "10";
  char sensor[41]    = "dc-rack-01";
  char cliente[41]   = "cliente-007";
  char ubicacion[61] = "Sala servidores - Rack 1";
} cfg;

const char *ARCHIVO_CONFIG = "/config.json";
bool debeGuardar = false;

DHT dht(PIN_DHT, TIPO_DHT);

unsigned long cicloMs = 10000;
unsigned long ultimoEnvio = 0;
unsigned long ultimaRevision = 0;
bool flamaAnterior = false;
bool gasAltoAnterior = false;

// =====================================================================
//  Configuración en flash (LittleFS)
// =====================================================================
void cargarConfig() {
  if (!LittleFS.begin()) {
    Serial.println("No se pudo montar LittleFS");
    return;
  }
  if (!LittleFS.exists(ARCHIVO_CONFIG)) {
    Serial.println("Sin configuración guardada: se usarán los valores por defecto");
    return;
  }
  File f = LittleFS.open(ARCHIVO_CONFIG, "r");
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, f);
  f.close();
  if (err) {
    Serial.println("config.json dañado: se usarán los valores por defecto");
    return;
  }
  strlcpy(cfg.servidor,  doc["servidor"]  | cfg.servidor,  sizeof(cfg.servidor));
  strlcpy(cfg.ciclo,     doc["ciclo"]     | cfg.ciclo,     sizeof(cfg.ciclo));
  strlcpy(cfg.sensor,    doc["sensor"]    | cfg.sensor,    sizeof(cfg.sensor));
  strlcpy(cfg.cliente,   doc["cliente"]   | cfg.cliente,   sizeof(cfg.cliente));
  strlcpy(cfg.ubicacion, doc["ubicacion"] | cfg.ubicacion, sizeof(cfg.ubicacion));
  Serial.println("Configuración cargada desde la flash");
}

void guardarConfig() {
  JsonDocument doc;
  doc["servidor"]  = cfg.servidor;
  doc["ciclo"]     = cfg.ciclo;
  doc["sensor"]    = cfg.sensor;
  doc["cliente"]   = cfg.cliente;
  doc["ubicacion"] = cfg.ubicacion;
  File f = LittleFS.open(ARCHIVO_CONFIG, "w");
  serializeJson(doc, f);
  f.close();
  Serial.println("Configuración guardada en la flash");
}

void alGuardarPortal() { debeGuardar = true; }

// Quita la "/" final del servidor para no armar URLs con "//"
void limpiarServidor() {
  size_t n = strlen(cfg.servidor);
  while (n > 0 && cfg.servidor[n - 1] == '/') cfg.servidor[--n] = '\0';
}

// =====================================================================
//  Portal cautivo (WiFiManager)
// =====================================================================
void iniciarWiFi() {
  WiFiManager wm;

  WiFiManagerParameter pServidor("servidor", "Servidor (http://IP:8000)", cfg.servidor, sizeof(cfg.servidor) - 1);
  WiFiManagerParameter pCiclo("ciclo", "Ciclo de envío (segundos)", cfg.ciclo, sizeof(cfg.ciclo) - 1);
  WiFiManagerParameter pSensor("sensor", "Nombre del sensor", cfg.sensor, sizeof(cfg.sensor) - 1);
  WiFiManagerParameter pCliente("cliente", "ID cliente", cfg.cliente, sizeof(cfg.cliente) - 1);
  WiFiManagerParameter pUbicacion("ubicacion", "Ubicación", cfg.ubicacion, sizeof(cfg.ubicacion) - 1);

  wm.addParameter(&pServidor);
  wm.addParameter(&pCiclo);
  wm.addParameter(&pSensor);
  wm.addParameter(&pCliente);
  wm.addParameter(&pUbicacion);

  wm.setSaveConfigCallback(alGuardarPortal);
  wm.setConfigPortalTimeout(300);   // 5 min sin configurar -> reinicia
  wm.setTitle("Sensor Datacenter");

  String nombreAP = "SENSOR-" + String(ESP.getChipId(), HEX);
  nombreAP.toUpperCase();
  Serial.print("Si no hay WiFi guardado, conéctate a la red ");
  Serial.println(nombreAP);

  if (!wm.autoConnect(nombreAP.c_str())) {
    Serial.println("No se configuró a tiempo. Reiniciando...");
    delay(1000);
    ESP.restart();
  }

  if (debeGuardar) {
    strlcpy(cfg.servidor,  pServidor.getValue(),  sizeof(cfg.servidor));
    strlcpy(cfg.ciclo,     pCiclo.getValue(),     sizeof(cfg.ciclo));
    strlcpy(cfg.sensor,    pSensor.getValue(),    sizeof(cfg.sensor));
    strlcpy(cfg.cliente,   pCliente.getValue(),   sizeof(cfg.cliente));
    strlcpy(cfg.ubicacion, pUbicacion.getValue(), sizeof(cfg.ubicacion));
    guardarConfig();
  }

  Serial.print("Conectado a ");
  Serial.print(WiFi.SSID());
  Serial.print("  IP: ");
  Serial.println(WiFi.localIP());
}

// Mantener FLASH 5 s -> borra configuración y vuelve al portal cautivo
void revisarBotonFlash() {
  static unsigned long inicio = 0;
  if (digitalRead(PIN_FLASH) == LOW) {
    if (inicio == 0) inicio = millis();
    if (millis() - inicio > 5000) {
      Serial.println("Borrando configuración y WiFi guardados...");
      LittleFS.remove(ARCHIVO_CONFIG);
      WiFiManager wm;
      wm.resetSettings();
      delay(500);
      ESP.restart();
    }
  } else {
    inicio = 0;
  }
}

// =====================================================================
//  Envío a la API
// =====================================================================
// tipo: temperatura | humedad | gas | llama   ·   unidad: C | % | raw | bool
bool enviarLectura(const char *tipo, float valor, const char *unidad) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("  Sin WiFi: no se envió la lectura");
    return false;
  }

  JsonDocument doc;
  doc["sensor"]    = cfg.sensor;
  doc["cliente"]   = cfg.cliente;
  doc["ubicacion"] = cfg.ubicacion;
  doc["tipo"]      = tipo;
  doc["valor"]     = valor;
  doc["unidad"]    = unidad;
  String json;
  serializeJson(doc, json);

  WiFiClient cliente;
  HTTPClient http;
  http.begin(cliente, String(cfg.servidor) + "/api/v1/lecturas");
  http.addHeader("Content-Type", "application/json");
  http.setTimeout(5000);
  int codigo = http.POST(json);

  Serial.print("  POST ");
  Serial.print(tipo);
  Serial.print(" -> ");
  if (codigo == 201) {
    Serial.println("201 guardado");
  } else if (codigo > 0) {
    Serial.print(codigo);
    Serial.print(" ");
    Serial.println(http.getString());   // la API explica el error
  } else {
    Serial.print("error de conexión: ");
    Serial.println(http.errorToString(codigo));
  }
  http.end();
  return codigo == 201;
}

// =====================================================================
//  Lecturas
// =====================================================================
bool leerFlama() { return digitalRead(PIN_FLAMA) == FLAMA_DETECTADA; }

void enviarCiclo() {
  float humedad = dht.readHumidity();
  float tempC = dht.readTemperature();
  int gas = analogRead(PIN_GAS);
  bool flama = leerFlama();

  Serial.println("----------------------------------------");
  if (isnan(tempC) || isnan(humedad)) {
    Serial.println("DHT11: error de lectura (revisa el cable out en D5)");
  } else {
    Serial.printf("Temp: %.1f°C / %.1f°F | Humedad: %.0f%%\n", tempC, tempC * 9.0 / 5.0 + 32.0, humedad);
  }
  Serial.printf("Gas: %d | Llama: %s\n", gas, flama ? "SI" : "No");

  if (!isnan(tempC))   enviarLectura("temperatura", tempC, "C");
  if (!isnan(humedad)) enviarLectura("humedad", humedad, "%");
  enviarLectura("gas", gas, "raw");
  enviarLectura("llama", flama ? 1 : 0, "bool");
}

// Cada 200 ms: si aparece/desaparece la llama o el gas cruza el umbral, se envía al tiro
void revisarCambios() {
  // En el ESP8266, leer A0 muy seguido corta el WiFi: por eso solo cada 200 ms
  int gas = analogRead(PIN_GAS);
  bool flama = leerFlama();
  bool gasAlto = gas > GAS_AVISO;

  if (flama != flamaAnterior) {
    Serial.println(flama ? "!!! Llama detectada: envío inmediato" : "Llama apagada");
    enviarLectura("llama", flama ? 1 : 0, "bool");
    flamaAnterior = flama;
  }
  if (gasAlto != gasAltoAnterior) {
    Serial.println(gasAlto ? "!!! Gas alto: envío inmediato" : "Gas normal");
    enviarLectura("gas", gas, "raw");
    gasAltoAnterior = gasAlto;
  }
}

// =====================================================================
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n\nSensor Datacenter SIY6122");

  pinMode(PIN_FLAMA, INPUT);
  pinMode(PIN_FLASH, INPUT_PULLUP);
  dht.begin();

  cargarConfig();
  iniciarWiFi();
  limpiarServidor();

  cicloMs = (unsigned long)atoi(cfg.ciclo) * 1000UL;
  if (cicloMs < 5000) cicloMs = 5000;   // mínimo 5 s (el DHT11 necesita tiempo)

  Serial.printf("Servidor: %s | Sensor: %s | Cliente: %s | Ciclo: %lu s\n",
                cfg.servidor, cfg.sensor, cfg.cliente, cicloMs / 1000);

  Serial.println("Calentando sensor MQ-2 (20 segundos)...");
  delay(20000);
  Serial.println("Sistema listo");

  flamaAnterior = leerFlama();
  gasAltoAnterior = analogRead(PIN_GAS) > GAS_AVISO;
  ultimoEnvio = millis() - cicloMs;   // primer envío de inmediato
}

void loop() {
  revisarBotonFlash();

  if (millis() - ultimaRevision >= 200) {
    ultimaRevision = millis();
    revisarCambios();
  }

  if (millis() - ultimoEnvio >= cicloMs) {
    ultimoEnvio = millis();
    enviarCiclo();
  }
}
