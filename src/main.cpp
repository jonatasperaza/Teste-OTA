#include <Arduino.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Update.h>
#include "mbedtls/md.h"

#include "generated_secrets.h"

// ─────────────────────────────────────────────
// Constantes
// ─────────────────────────────────────────────
static const int LED_PIN = 2;

static const char *TOPIC_TELEMETRY = "v1/devices/me/telemetry";
static const char *TOPIC_ATTRIBUTES = "v1/devices/me/attributes";
static const char *TOPIC_ATTR_RESPONSE = "v1/devices/me/attributes/response/+";
static const char *TOPIC_FW_RESPONSE = "v2/fw/response/+/chunk/+";

static const size_t OTA_CHUNK_SIZE = 4096;
static const unsigned long MQTT_RECONNECT_MS = 3000;
static const unsigned long TELEMETRY_INTERVAL_MS = 10000;
static const unsigned long OTA_CHUNK_TIMEOUT_MS = 30000;

// ─────────────────────────────────────────────
// Globais
// ─────────────────────────────────────────────
WiFiClient wifiClient;
PubSubClient mqtt(wifiClient);

unsigned long lastMqttTry = 0;
unsigned long lastTelemetry = 0;
unsigned long lastBlink = 0;
bool ledState = false;

int attrRequestId = 1;
int fwRequestId = 1;

bool otaInProgress = false;
bool sha256Active = false;

String otaTitle;
String otaVersion;
String otaChecksum;
String otaChecksumAlgorithm;

size_t otaSize = 0;
size_t otaWritten = 0;
int otaChunkIndex = 0;
unsigned long lastChunkAt = 0;

mbedtls_md_context_t sha256Ctx;

// ─────────────────────────────────────────────
// Utilidades
// ─────────────────────────────────────────────
String chipId()
{
  uint64_t mac = ESP.getEfuseMac();
  char id[32];
  snprintf(id, sizeof(id), "%04X%08X", (uint16_t)(mac >> 32), (uint32_t)mac);
  return String(id);
}

void publishTelemetry(const String &json)
{
  Serial.print("[TB] Telemetria: ");
  Serial.println(json);

  if (mqtt.connected())
  {
    mqtt.publish(TOPIC_TELEMETRY, json.c_str());
  }
}

void publishOTAState(const String &state, const String &error = "")
{
  JsonDocument doc;

  doc["fw_state"] = state;

  if (state == "UPDATED")
  {
    doc["current_fw_title"] = FW_TITLE;
    doc["current_fw_version"] = FW_VERSION;
  }

  if (otaTitle.length())
    doc["target_fw_title"] = otaTitle;

  if (otaVersion.length())
    doc["target_fw_version"] = otaVersion;

  if (otaSize > 0)
  {
    doc["fw_size"] = otaSize;
    doc["fw_written"] = otaWritten;
    doc["fw_progress"] = (int)((otaWritten * 100) / otaSize);
  }

  if (error.length())
    doc["fw_error"] = error;

  String json;
  serializeJson(doc, json);

  publishTelemetry(json);
}

void publishBootInfo()
{
  JsonDocument doc;

  doc["current_fw_title"] = FW_TITLE;
  doc["current_fw_version"] = FW_VERSION;
  doc["fw_state"] = "UPDATED";
  doc["ip"] = WiFi.localIP().toString();
  doc["rssi"] = WiFi.RSSI();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["sketch_size"] = ESP.getSketchSize();
  doc["free_ota_space"] = ESP.getFreeSketchSpace();

  String json;
  serializeJson(doc, json);

  publishTelemetry(json);
}

void publishPeriodicTelemetry()
{
  JsonDocument doc;

  doc["temperature"] = 25;
  doc["uptime_ms"] = millis();
  doc["free_heap"] = ESP.getFreeHeap();
  doc["rssi"] = WiFi.RSSI();
  doc["fw_title"] = FW_TITLE;
  doc["fw_version"] = FW_VERSION;

  String json;
  serializeJson(doc, json);

  publishTelemetry(json);
}

// ─────────────────────────────────────────────
// SHA-256
// ─────────────────────────────────────────────
bool sha256Start()
{
  const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);

  if (!info)
    return false;

  mbedtls_md_init(&sha256Ctx);

  if (mbedtls_md_setup(&sha256Ctx, info, 0) != 0)
    return false;

  if (mbedtls_md_starts(&sha256Ctx) != 0)
    return false;

  sha256Active = true;
  return true;
}

void sha256Update(const uint8_t *data, size_t len)
{
  if (!sha256Active)
    return;

  mbedtls_md_update(&sha256Ctx, data, len);
}

bool sha256FinishAndVerify()
{
  if (!sha256Active)
    return true;

  unsigned char hash[32];

  if (mbedtls_md_finish(&sha256Ctx, hash) != 0)
  {
    mbedtls_md_free(&sha256Ctx);
    sha256Active = false;
    return false;
  }

  mbedtls_md_free(&sha256Ctx);
  sha256Active = false;

  String actual;

  for (int i = 0; i < 32; i++)
  {
    if (hash[i] < 16)
      actual += "0";

    actual += String(hash[i], HEX);
  }

  actual.toLowerCase();

  String expected = otaChecksum;
  expected.toLowerCase();
  expected.replace(" ", "");

  Serial.print("[OTA] SHA256 esperado: ");
  Serial.println(expected);

  Serial.print("[OTA] SHA256 calculado: ");
  Serial.println(actual);

  return actual == expected;
}

void sha256Abort()
{
  if (sha256Active)
  {
    mbedtls_md_free(&sha256Ctx);
    sha256Active = false;
  }
}

// ─────────────────────────────────────────────
// OTA ThingsBoard
// ─────────────────────────────────────────────
void failOTA(const String &reason)
{
  Serial.print("[OTA] FALHOU: ");
  Serial.println(reason);

  Update.abort();
  sha256Abort();

  publishOTAState("FAILED", reason);

  otaInProgress = false;
  otaWritten = 0;
  otaSize = 0;
  otaChunkIndex = 0;
}

void requestNextChunk()
{
  if (!otaInProgress || !mqtt.connected())
    return;

  String topic = "v2/fw/request/" + String(fwRequestId) + "/chunk/" + String(otaChunkIndex);
  String payload = String(OTA_CHUNK_SIZE);

  Serial.print("[OTA] Pedindo chunk ");
  Serial.print(otaChunkIndex);
  Serial.print(" em ");
  Serial.println(topic);

  mqtt.publish(topic.c_str(), payload.c_str());

  lastChunkAt = millis();
}

void finishOTA()
{
  Serial.println("[OTA] Download concluido");

  if (otaSize > 0 && otaWritten != otaSize)
  {
    failOTA("Tamanho recebido diferente do fw_size. Recebido=" + String(otaWritten) + " esperado=" + String(otaSize));
    return;
  }

  publishOTAState("DOWNLOADED");

  String alg = otaChecksumAlgorithm;
  alg.toUpperCase();

  if ((alg == "SHA256" || alg == "SHA-256") && otaChecksum.length())
  {
    Serial.println("[OTA] Verificando SHA-256...");

    if (!sha256FinishAndVerify())
    {
      failOTA("Checksum SHA-256 invalido");
      return;
    }
  }

  publishOTAState("VERIFIED");

  Serial.println("[OTA] Finalizando Update.end()...");

  if (!Update.end(true))
  {
    String err = "Update.end falhou. Erro=" + String(Update.getError());
    failOTA(err);
    return;
  }

  if (!Update.isFinished())
  {
    failOTA("Update nao finalizou completamente");
    return;
  }

  publishOTAState("UPDATING");

  Serial.println("[OTA] Firmware gravado com sucesso. Reiniciando...");
  delay(1000);

  ESP.restart();
}

void handleFirmwareChunk(const uint8_t *payload, unsigned int length)
{
  if (!otaInProgress)
  {
    Serial.println("[OTA] Chunk recebido, mas nao ha OTA em andamento");
    return;
  }

  lastChunkAt = millis();

  // ThingsBoard pode enviar chunk vazio para indicar fim.
  if (length == 0)
  {
    finishOTA();
    return;
  }

  size_t written = Update.write((uint8_t *)payload, length);

  if (written != length)
  {
    failOTA("Update.write escreveu " + String(written) + " de " + String(length) + " bytes");
    return;
  }

  sha256Update(payload, length);

  otaWritten += written;

  int progress = otaSize > 0 ? (int)((otaWritten * 100) / otaSize) : 0;

  Serial.print("[OTA] Chunk ");
  Serial.print(otaChunkIndex);
  Serial.print(" OK | ");
  Serial.print(otaWritten);
  Serial.print("/");
  Serial.print(otaSize);
  Serial.print(" bytes | ");
  Serial.print(progress);
  Serial.println("%");

  if (otaChunkIndex % 5 == 0)
  {
    publishOTAState("DOWNLOADING");
  }

  // Se ja recebeu o tamanho total informado pelo ThingsBoard, finaliza.
  if (otaSize > 0 && otaWritten >= otaSize)
  {
    finishOTA();
    return;
  }

  otaChunkIndex++;
  requestNextChunk();
}

void startOTA()
{
  if (otaInProgress)
  {
    Serial.println("[OTA] Ja existe OTA em andamento");
    return;
  }

  Serial.println();
  Serial.println("══════════════════════════════════════");
  Serial.println("[OTA] Iniciando OTA via ThingsBoard");
  Serial.print("[OTA] Title: ");
  Serial.println(otaTitle);
  Serial.print("[OTA] Version: ");
  Serial.println(otaVersion);
  Serial.print("[OTA] Size: ");
  Serial.println(otaSize);
  Serial.print("[OTA] Checksum algorithm: ");
  Serial.println(otaChecksumAlgorithm);
  Serial.print("[OTA] Checksum: ");
  Serial.println(otaChecksum);
  Serial.print("[OTA] Sketch atual: ");
  Serial.println(ESP.getSketchSize());
  Serial.print("[OTA] Espaco livre OTA: ");
  Serial.println(ESP.getFreeSketchSpace());
  Serial.print("[OTA] Heap livre: ");
  Serial.println(ESP.getFreeHeap());
  Serial.println("══════════════════════════════════════");

  if (otaSize == 0)
  {
    failOTA("fw_size veio vazio ou zero");
    return;
  }

  if (otaSize > ESP.getFreeSketchSpace())
  {
    failOTA("Firmware maior que o espaco OTA livre");
    return;
  }

  String alg = otaChecksumAlgorithm;
  alg.toUpperCase();

  if (alg == "SHA256" || alg == "SHA-256")
  {
    if (!sha256Start())
    {
      failOTA("Nao conseguiu iniciar SHA-256");
      return;
    }
  }
  else if (otaChecksum.length())
  {
    failOTA("Algoritmo de checksum nao suportado neste exemplo: " + otaChecksumAlgorithm);
    return;
  }

  if (!Update.begin(otaSize))
  {
    failOTA("Update.begin falhou. Erro=" + String(Update.getError()));
    return;
  }

  otaInProgress = true;
  otaWritten = 0;
  otaChunkIndex = 0;
  fwRequestId++;

  publishOTAState("DOWNLOADING");

  requestNextChunk();
}

void parseOTAAttributes(JsonObject obj)
{
  if (!obj["fw_title"].is<const char *>() && !obj["fw_version"].is<const char *>())
    return;

  String newTitle = obj["fw_title"] | "";
  String newVersion = obj["fw_version"] | "";

  size_t newSize = obj["fw_size"] | 0;
  String newChecksum = obj["fw_checksum"] | "";
  String newChecksumAlgorithm = obj["fw_checksum_algorithm"] | "";

  if (!newTitle.length() || !newVersion.length())
  {
    Serial.println("[OTA] Atributos OTA incompletos");
    return;
  }

  Serial.println();
  Serial.println("[OTA] Atributos recebidos:");
  Serial.print("  fw_title: ");
  Serial.println(newTitle);
  Serial.print("  fw_version: ");
  Serial.println(newVersion);
  Serial.print("  fw_size: ");
  Serial.println(newSize);
  Serial.print("  fw_checksum_algorithm: ");
  Serial.println(newChecksumAlgorithm);
  Serial.print("  fw_checksum: ");
  Serial.println(newChecksum);

  if (newTitle != FW_TITLE)
  {
    Serial.println("[OTA] Ignorado: fw_title diferente do firmware atual");
    return;
  }

  if (newVersion == FW_VERSION)
  {
    Serial.println("[OTA] Ignorado: versao ja instalada");
    publishOTAState("UPDATED");
    return;
  }

  otaTitle = newTitle;
  otaVersion = newVersion;
  otaSize = newSize;
  otaChecksum = newChecksum;
  otaChecksumAlgorithm = newChecksumAlgorithm;

  startOTA();
}

void requestSharedAttributes()
{
  if (!mqtt.connected())
    return;

  String topic = "v1/devices/me/attributes/request/" + String(attrRequestId++);

  String payload = "{\"sharedKeys\":\"fw_title,fw_version,fw_size,fw_checksum,fw_checksum_algorithm\"}";

  Serial.print("[TB] Solicitando atributos compartilhados em ");
  Serial.println(topic);

  mqtt.publish(topic.c_str(), payload.c_str());
}

// ─────────────────────────────────────────────
// MQTT callback
// ─────────────────────────────────────────────
void mqttCallback(char *topic, uint8_t *payload, unsigned int length)
{
  String topicStr = String(topic);

  if (topicStr.startsWith("v2/fw/response/"))
  {
    handleFirmwareChunk(payload, length);
    return;
  }

  String msg;
  msg.reserve(length + 1);

  for (unsigned int i = 0; i < length; i++)
  {
    msg += (char)payload[i];
  }

  Serial.print("[MQTT] Topic: ");
  Serial.println(topicStr);

  Serial.print("[MQTT] Payload: ");
  Serial.println(msg);

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, msg);

  if (err)
  {
    Serial.print("[JSON] Erro: ");
    Serial.println(err.c_str());
    return;
  }

  // Resposta de request vem assim:
  // {"client":{},"shared":{"fw_title":"..."}}
  if (doc["shared"].is<JsonObject>())
  {
    JsonObject shared = doc["shared"].as<JsonObject>();
    parseOTAAttributes(shared);
    return;
  }

  // Update direto de shared attribute vem assim:
  // {"fw_title":"...","fw_version":"..."}
  if (doc.is<JsonObject>())
  {
    JsonObject root = doc.as<JsonObject>();
    parseOTAAttributes(root);
    return;
  }
}

// ─────────────────────────────────────────────
// Wi-Fi e MQTT
// ─────────────────────────────────────────────
void connectWiFi()
{
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);

  Serial.print("[WiFi] Conectando em ");
  Serial.println(WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASS);

  while (WiFi.status() != WL_CONNECTED)
  {
    delay(300);
    Serial.print(".");
  }

  Serial.println();
  Serial.print("[WiFi] OK. IP: ");
  Serial.println(WiFi.localIP());
}

void connectMQTT()
{
  if (mqtt.connected())
    return;

  if (millis() - lastMqttTry < MQTT_RECONNECT_MS)
    return;

  lastMqttTry = millis();

  String clientId = "esp32-tb-ota-" + chipId();

  Serial.print("[MQTT] Conectando em ");
  Serial.print(TB_HOST);
  Serial.print(":");
  Serial.println(TB_PORT);

  Serial.print("[MQTT] Client ID: ");
  Serial.println(clientId);

  bool ok = mqtt.connect(clientId.c_str(), TB_TOKEN, "");

  if (!ok)
  {
    Serial.print("[MQTT] Falhou. State: ");
    Serial.println(mqtt.state());
    return;
  }

  Serial.println("[MQTT] Conectado");

  mqtt.subscribe(TOPIC_ATTRIBUTES);
  mqtt.subscribe(TOPIC_ATTR_RESPONSE);
  mqtt.subscribe(TOPIC_FW_RESPONSE);

  Serial.println("[MQTT] Subscrito em atributos e OTA");

  publishBootInfo();
  requestSharedAttributes();
}

// ─────────────────────────────────────────────
// Arduino setup/loop
// ─────────────────────────────────────────────
void setup()
{
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, LOW);
  Serial.println("slk o pae é bom dms");

  Serial.begin(115200);
  delay(500);

  Serial.println();
  Serial.println("══════════════════════════════════════");
  Serial.println("[SYS] ESP32 ThingsBoard OTA Test");
  Serial.print("[FW] Title: ");
  Serial.println(FW_TITLE);
  Serial.print("[FW] Version: ");
  Serial.println(FW_VERSION);
  Serial.print("[SYS] Chip ID: ");
  Serial.println(chipId());
  Serial.println("══════════════════════════════════════");

  connectWiFi();

  mqtt.setServer(TB_HOST, TB_PORT);
  mqtt.setCallback(mqttCallback);
  mqtt.setBufferSize(8192);
  mqtt.setKeepAlive(60);
  mqtt.setSocketTimeout(30);
}

void loop()
{
  if (WiFi.status() != WL_CONNECTED)
  {
    Serial.println("[WiFi] Desconectado. Reconectando...");
    WiFi.reconnect();
    delay(1000);
    return;
  }

  connectMQTT();
  mqtt.loop();

  if (otaInProgress && millis() - lastChunkAt > OTA_CHUNK_TIMEOUT_MS)
  {
    failOTA("Timeout esperando chunk OTA");
  }

  if (!otaInProgress && millis() - lastTelemetry > TELEMETRY_INTERVAL_MS)
  {
    lastTelemetry = millis();
    publishPeriodicTelemetry();
  }

  if (millis() - lastBlink > 500)
  {
    lastBlink = millis();
    ledState = !ledState;
    digitalWrite(LED_PIN, ledState ? HIGH : LOW);
  }
}
