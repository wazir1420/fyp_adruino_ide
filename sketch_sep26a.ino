#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ModbusMaster.h>
#include <time.h>

// ---------- WiFi CONFIG ----------
// NOTE: apna asal naam aur password yahan daalein. Code share karne se pehle
// inhein dobara "xxxx" kar dein.
const char* WIFI_SSID     = "Wazir";
const char* WIFI_PASSWORD = "wazir@1420";

// ---------- FIREBASE CONFIG ----------
const char* FIREBASE_DB_URL = "https://finalyearproject-2034b-default-rtdb.asia-southeast1.firebasedatabase.app";

// ---------- METER IDENTITY ----------
const char* METER_ID   = "meter1";
const char* METER_NAME = "ABB B24";

// ---------- Meter CONFIG ----------
#define METER_SLAVE_ID     10
#define MODBUS_BAUDRATE    19200

// ---------- Pin definitions ----------
#define RS485_DE_RE_PIN    4
#define RXD2               16
#define TXD2               17

// ================= RELAY: device control config =================
// Active-LOW relay module: LOW = ON, HIGH = OFF
#define RELAY_ON   LOW
#define RELAY_OFF  HIGH

struct Device {
  const char* id;            // Firebase key: meters/<METER_ID>/devices/<id>
  uint8_t     pin;           // ESP32 GPIO -> relay IN
  float       ratedWatts;    // ESTIMATED consumption when ON (no sensor)
  unsigned long minOffMs;    // compressor protection (0 = none)
  bool        on;            // current relay state
  unsigned long offSince;    // millis() when last turned OFF (0 = never)
};

Device devices[] = {
  // id       pin  watts  minOff        on     offSince
  {"bulb",    25,  10.0,  0,            false, 0},
  {"fan",     26,  60.0,  0,            false, 0},
  {"fridge",  27,  150.0, 5UL*60*1000,  false, 0},
  {"spare",   32,  0.0,   0,            false, 0},
};
const int NUM_DEVICES = sizeof(devices) / sizeof(devices[0]);

// Control task kitni jaldi command check kare (ms). Kam = tez jawab.
const unsigned long CMD_POLL_INTERVAL_MS = 600;
// Status (applied/power/lastUpdate) bina change ke bhi is dauran likhna
const unsigned long HEARTBEAT_MS = 10000;
// true  = ESP32 chalu hote hi sab commands "off" kar deta hai (bijli aane par
//         koi load achanak nahi chalta).
// false = Firebase mein jo "state" pari hai, wahi dobara lagu ho jati hai.
const bool RESET_COMMANDS_ON_BOOT = true;
// Heap is se kam ho jaye to control ka connection band karke dobara banta hai.
const uint32_t MIN_FREE_HEAP = 30000;
// =====================================================================

ModbusMaster node;
WiFiClientSecure secureClient;     // sirf monitoring ke liye (loop)
WiFiClientSecure ctrlClient;       // sirf control task ke liye
HTTPClient       ctrlHttp;         // connection reuse ke liye global

// ---------- Print queue ----------
QueueHandle_t printQueue = NULL;

unsigned long lastReadTime = 0;
const unsigned long READ_INTERVAL_MS = 2000;

unsigned long lastHistoryWriteTime = 0;
const unsigned long HISTORY_INTERVAL_MS = 60000;

int lastLoggedHour = -1;

// Last-good values
float lastCurrent = 0, lastPower = 0, lastFreq = 50.0, lastPF = 0, lastEnergy = 0;

// ---------- Print task ----------
void printTask(void *pvParameters) {
  char* msg;
  for (;;) {
    if (xQueueReceive(printQueue, &msg, portMAX_DELAY) == pdTRUE) {
      Serial.print(msg);
      Serial.flush();
      free(msg);
    }
  }
}

void safePrintf(const char* fmt, ...) {
  if (printQueue == NULL) return;
  char* buf = (char*)malloc(256);
  if (!buf) return;
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, 256, fmt, args);
  va_end(args);
  if (xQueueSend(printQueue, &buf, 0) != pdTRUE) {
    free(buf);
  }
}

// ---------- RS485 direction control ----------
void preTransmission() {
  digitalWrite(RS485_DE_RE_PIN, HIGH);
  delayMicroseconds(100);
}

void postTransmission() {
  unsigned long t = millis();
  while (Serial2.availableForWrite() < 64 && (millis() - t) < 50) {
  }
  delayMicroseconds(100);
  digitalWrite(RS485_DE_RE_PIN, LOW);
}

// ---------- WiFi + Time ----------
void connectWiFi() {
  safePrintf("WiFi se connect ho raha hai");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi Connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  configTime(5 * 3600, 0, "pool.ntp.org", "time.nist.gov");
  Serial.print("Time sync ho raha hai");
  time_t now = time(nullptr);
  while (now < 100000) {
    delay(500);
    Serial.print(".");
    now = time(nullptr);
  }
  Serial.println();
  Serial.println("Time synced!");
}

String getTodayDateString() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
  return String(buf);
}

// ---------- Modbus reads (3 retries each) ----------
uint32_t read32(uint16_t startReg) {
  for (int i = 1; i <= 3; i++) {
    uint8_t result = node.readHoldingRegisters(startReg, 2);
    if (result == node.ku8MBSuccess) {
      uint32_t high = node.getResponseBuffer(0);
      uint32_t low  = node.getResponseBuffer(1);
      return (high << 16) | low;
    }
    safePrintf("Modbus fail reg 0x%X err 0x%X (try %d)\n", startReg, result, i);
    delay(100);
  }
  return 0xFFFFFFFF;
}

int32_t read16(uint16_t reg, bool &ok) {
  for (int i = 1; i <= 3; i++) {
    uint8_t result = node.readHoldingRegisters(reg, 1);
    if (result == node.ku8MBSuccess) {
      ok = true;
      return (int16_t)node.getResponseBuffer(0);
    }
    safePrintf("Modbus fail reg 0x%X err 0x%X (try %d)\n", reg, result, i);
    delay(100);
  }
  ok = false;
  return 0;
}

uint64_t read64(uint16_t startReg, bool &ok) {
  for (int i = 1; i <= 3; i++) {
    uint8_t result = node.readHoldingRegisters(startReg, 4);
    if (result == node.ku8MBSuccess) {
      ok = true;
      uint64_t r0 = node.getResponseBuffer(0);
      uint64_t r1 = node.getResponseBuffer(1);
      uint64_t r2 = node.getResponseBuffer(2);
      uint64_t r3 = node.getResponseBuffer(3);
      return (r0 << 48) | (r1 << 32) | (r2 << 16) | r3;
    }
    safePrintf("Modbus fail reg 0x%X err 0x%X (try %d)\n", startReg, result, i);
    delay(100);
  }
  ok = false;
  return 0;
}

// ---------- Firebase helper (PUT) — monitoring ke liye, pehle jaisa ----------
bool firebasePut(const String& url, const String& body, const char* tag, int maxAttempts) {
  for (int attempt = 1; attempt <= maxAttempts; attempt++) {
    if (WiFi.status() != WL_CONNECTED) return false;

    secureClient.stop();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(3000);
    http.setTimeout(4000);

    if (!http.begin(secureClient, url)) {
      delay(300);
      continue;
    }
    http.addHeader("Content-Type", "application/json");
    int code = http.PUT(body);
    http.end();

    if (code == 200) {
      safePrintf("Firebase (%s) updated, response code: 200\n", tag);
      return true;
    }
    safePrintf("Firebase (%s) attempt %d failed: %d (%s) heap=%u\n",
               tag, attempt, code, HTTPClient::errorToString(code).c_str(),
               ESP.getFreeHeap());
    delay(300 * attempt);
  }
  return false;
}

// =====================================================================
//                    RELAY CONTROL (alag FreeRTOS task)
// =====================================================================
// Firebase path: /meters/<METER_ID>/devices/<id>
//   state       (app likhti hai)   : true/false = chahiye on/off
//   applied     (ESP32 likhta hai) : relay ki asal halat
//   power       (ESP32 likhta hai) : andaza (W)
//   lastUpdate  (ESP32 likhta hai) : server timestamp

String devicesUrl() {
  return String(FIREBASE_DB_URL) + "/meters/" + METER_ID + "/devices.json";
}

// Control ka apna connection: khula rehta hai (TLS handshake baar baar nahi).
// Fail ho to connection band karke ek dafa dobara koshish karta hai.
bool ctrlRequest(bool isPatch, const String& url, const String& body, String* out) {
  for (int attempt = 1; attempt <= 2; attempt++) {
    if (WiFi.status() != WL_CONNECTED) return false;

    ctrlHttp.setReuse(true);
    ctrlHttp.setConnectTimeout(2500);
    ctrlHttp.setTimeout(2500);
    if (!ctrlHttp.begin(ctrlClient, url)) {
      ctrlClient.stop();
      continue;
    }

    int code;
    if (isPatch) {
      ctrlHttp.addHeader("Content-Type", "application/json");
      code = ctrlHttp.PATCH(body);
    } else {
      code = ctrlHttp.GET();
    }

    if (code == 200) {
      if (out) *out = ctrlHttp.getString();
      ctrlHttp.end();            // reuse=true => connection khula rehta hai
      return true;
    }

    safePrintf("Control %s failed: %d (%s) try %d\n", isPatch ? "PATCH" : "GET",
               code, HTTPClient::errorToString(code).c_str(), attempt);
    ctrlHttp.end();
    ctrlClient.stop();           // purana/band connection chhor kar naya banao
  }
  return false;
}

// payload: {"bulb":{"applied":true,"lastUpdate":123,"power":10,"state":true},"fan":{...}}
// "id" ka object dhoondta hai (agle device ke object tak), us mein "state" parhta hai.
bool parseState(const String& payload, const char* id, bool &state) {
  String key = "\"" + String(id) + "\":{";
  int start = payload.indexOf(key);
  if (start < 0) return false;

  int end = payload.length();
  for (int j = 0; j < NUM_DEVICES; j++) {
    if (strcmp(devices[j].id, id) == 0) continue;
    String other = "\"" + String(devices[j].id) + "\":{";
    int p = payload.indexOf(other, start + key.length());
    if (p >= 0 && p < end) end = p;
  }

  int s = payload.indexOf("\"state\":", start);
  if (s < 0 || s >= end) return false;
  String v = payload.substring(s + 8, s + 13);
  if (v.startsWith("true"))  { state = true;  return true; }
  if (v.startsWith("false")) { state = false; return true; }
  return false;
}

// Sab devices ka status ek hi PATCH mein (multi-path update): ek request, ek dafa.
String buildStatusBody(bool alsoResetState) {
  String b = "{";
  for (int i = 0; i < NUM_DEVICES; i++) {
    Device &d = devices[i];
    if (i) b += ",";
    String id = String(d.id);
    if (alsoResetState) b += "\"" + id + "/state\":false,";
    b += "\"" + id + "/applied\":" + (d.on ? "true" : "false");
    b += ",\"" + id + "/power\":" + String(d.on ? d.ratedWatts : 0.0, 1);
    b += ",\"" + id + "/lastUpdate\":{\".sv\":\"timestamp\"}";
  }
  b += "}";
  return b;
}

unsigned long lastStatusWrite = 0;

bool writeAllStatus(bool alsoResetState) {
  bool ok = ctrlRequest(true, devicesUrl(), buildStatusBody(alsoResetState), nullptr);
  if (ok) lastStatusWrite = millis();
  return ok;
}

void setRelay(Device &d, bool on) {
  digitalWrite(d.pin, on ? RELAY_ON : RELAY_OFF);
  if (d.on && !on) d.offSince = millis();
  d.on = on;
  safePrintf("Relay %s (GPIO%d) -> %s\n", d.id, d.pin, on ? "ON" : "OFF");
}

void pollCommands() {
  String payload;
  if (!ctrlRequest(false, devicesUrl(), "", &payload)) return;  // fail => halat wahi
  if (payload == "null") return;

  bool changed = false;

  for (int i = 0; i < NUM_DEVICES; i++) {
    Device &d = devices[i];
    bool want;
    if (!parseState(payload, d.id, want)) continue;
    if (want == d.on) continue;

    // Compressor protection: OFF ke baad minOff tak ON nahi
    if (want && d.minOffMs > 0 && d.offSince > 0 &&
        (millis() - d.offSince) < d.minOffMs) {
      unsigned long left = (d.minOffMs - (millis() - d.offSince)) / 1000;
      safePrintf("%s: compressor guard, %lus baqi\n", d.id, left);
      continue;     // applied false rehta hai => app mein "waiting"
    }

    setRelay(d, want);   // pehle relay chalao, phir status likho
    changed = true;
  }

  if (changed || millis() - lastStatusWrite >= HEARTBEAT_MS) {
    writeAllStatus(false);
  }
}

void controlTask(void *pvParameters) {
  ctrlClient.setInsecure();

  // Boot par sab relays pehle se OFF hain. Firebase mein purani "state: true"
  // pari ho to usay bhi false kar dein, taake bijli aane par load na chalein.
  if (RESET_COMMANDS_ON_BOOT) {
    while (!writeAllStatus(true)) {
      vTaskDelay(pdMS_TO_TICKS(2000));
    }
    safePrintf("Control: boot par sab commands off kar diye\n");
  }

  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }

    if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
      safePrintf("Control: heap kam (%u), connection reset\n", ESP.getFreeHeap());
      ctrlClient.stop();
    }

    pollCommands();
    vTaskDelay(pdMS_TO_TICKS(CMD_POLL_INTERVAL_MS));
  }
}
// =====================================================================

void sendMeterName() {
  String url = String(FIREBASE_DB_URL) + "/meters/" + METER_ID + "/name.json";
  String json = "\"" + String(METER_NAME) + "\"";
  firebasePut(url, json, "name", 3);
}

void sendLatestToFirebase(float voltage, float current, float powerW,
                          float frequency, float powerFactor, float energyKwh) {
  String url = String(FIREBASE_DB_URL) + "/meters/" + METER_ID + "/latest.json";

  String json = "{";
  json += "\"voltage\":" + String(voltage, 1) + ",";
  json += "\"current\":" + String(current, 2) + ",";
  json += "\"power\":"   + String(powerW, 2) + ",";
  json += "\"frequency\":" + String(frequency, 2) + ",";
  json += "\"powerFactor\":" + String(powerFactor, 3) + ",";
  json += "\"energy\":" + String(energyKwh, 2) + ",";
  json += "\"timestamp\":{\".sv\":\"timestamp\"}";
  json += "}";

  firebasePut(url, json, "latest", 2);
}

void sendHistoryToFirebase(float energyKwh) {
  String dateKey = getTodayDateString();
  String url = String(FIREBASE_DB_URL) + "/meters/" + METER_ID + "/history/" + dateKey + ".json";
  firebasePut(url, String(energyKwh, 2), "history", 3);
}

void sendHourlyToFirebase(float energyKwh) {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);

  if (timeinfo.tm_hour == lastLoggedHour) return;
  lastLoggedHour = timeinfo.tm_hour;

  struct tm prev = timeinfo;
  prev.tm_hour -= 1;
  mktime(&prev);

  char dayBuf[11];
  snprintf(dayBuf, sizeof(dayBuf), "%04d-%02d-%02d",
           prev.tm_year + 1900, prev.tm_mon + 1, prev.tm_mday);

  String url = String(FIREBASE_DB_URL) + "/meters/" + METER_ID +
             "/hourly/" + String(dayBuf) + "/h" + String(prev.tm_hour) + ".json";

  firebasePut(url, String(energyKwh, 2), "hourly", 3);
}

// ---------- Main read + send (original) ----------
void updateAndSendReadings() {
  const uint16_t REG_VOLTAGE_L1        = 0x5B00;
  const uint16_t REG_CURRENT_L1        = 0x5B0C;
  const uint16_t REG_POWER_TOTAL       = 0x5B14;
  const uint16_t REG_FREQUENCY         = 0x5B2C;
  const uint16_t REG_POWER_FACTOR      = 0x5B3A;
  const uint16_t REG_RESETTABLE_ENERGY = 0x552C;

  uint32_t rawVoltage = read32(REG_VOLTAGE_L1);

  if (rawVoltage == 0xFFFFFFFF) {
    safePrintf("Meter se koi jawab nahi mila - Firebase update skip\n");
    return;
  }

  float voltage = rawVoltage * 0.1;
  if (voltage < 150 || voltage > 300) {
    safePrintf("Voltage ghalat lag rahi hai (%.1f), skip\n", voltage);
    return;
  }

  delay(150);
  uint32_t rawCurrent = read32(REG_CURRENT_L1);
  delay(150);
  uint32_t rawPower = read32(REG_POWER_TOTAL);
  delay(150);

  bool freqOk = false, pfOk = false, energyOk = false;
  int32_t rawFrequency   = read16(REG_FREQUENCY, freqOk);
  delay(150);
  int32_t rawPowerFactor = read16(REG_POWER_FACTOR, pfOk);
  delay(150);
  uint64_t rawEnergy     = read64(REG_RESETTABLE_ENERGY, energyOk);

  if (rawCurrent != 0xFFFFFFFF) lastCurrent = rawCurrent * 0.01;
  if (rawPower   != 0xFFFFFFFF) lastPower   = ((int32_t)rawPower) * 0.01;
  if (freqOk) {
    float f = rawFrequency * 0.01f;
    if (f > 45 && f < 55) lastFreq = f;
  }
  if (pfOk && abs(rawPowerFactor) <= 1000) lastPF = rawPowerFactor * 0.001f;
  if (energyOk) lastEnergy = (double)rawEnergy * 0.01;

  safePrintf("[%s] V=%.1f  I=%.2f  P=%.2fW  Hz=%.2f  PF=%.3f  E=%.2fkWh\n",
             METER_ID, voltage, lastCurrent, lastPower, lastFreq, lastPF, lastEnergy);

  sendLatestToFirebase(voltage, lastCurrent, lastPower, lastFreq, lastPF, lastEnergy);

  if (millis() - lastHistoryWriteTime >= HISTORY_INTERVAL_MS) {
    lastHistoryWriteTime = millis();
    sendHistoryToFirebase(lastEnergy);
  }

  sendHourlyToFirebase(lastEnergy);
}

// ---------- Setup ----------
void setup() {
  // Sabse pehle relays OFF karo (active-LOW => HIGH), boot par click na ho
  for (int i = 0; i < NUM_DEVICES; i++) {
    digitalWrite(devices[i].pin, RELAY_OFF);
    pinMode(devices[i].pin, OUTPUT);
    digitalWrite(devices[i].pin, RELAY_OFF);
  }

  Serial.begin(115200);
  delay(1000);

  printQueue = xQueueCreate(30, sizeof(char*));
  xTaskCreatePinnedToCore(printTask, "PrintTask", 4096, NULL, 1, NULL, 0);

  Serial.printf("Reset reason: %d\n", (int)esp_reset_reason());
  Serial.flush();

  connectWiFi();

  secureClient.setInsecure();
  sendMeterName();

  Serial2.begin(MODBUS_BAUDRATE, SERIAL_8E1, RXD2, TXD2);
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW);

  node.begin(METER_SLAVE_ID, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  // Relay control alag task mein (Core 0): monitoring ke delay isay nahi rokte.
  xTaskCreatePinnedToCore(controlTask, "ControlTask", 10240, NULL, 2, NULL, 0);

  Serial.println("Setup complete. Meter data + relay control shuru...");
  Serial.flush();
}

// ---------- Loop (sirf monitoring) ----------
void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    safePrintf("WiFi disconnect ho gaya, dobara connect kar rahe hain...\n");
    connectWiFi();
  }

  if (millis() - lastReadTime >= READ_INTERVAL_MS) {
    lastReadTime = millis();
    updateAndSendReadings();
  }
}
