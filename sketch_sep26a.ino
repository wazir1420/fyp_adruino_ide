#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ModbusMaster.h>
#include <time.h>

// ---------- WiFi CONFIG ----------
const char* WIFI_SSID     = "Wazir";
const char* WIFI_PASSWORD = "wazir@1420";

// ---------- FIREBASE CONFIG ----------
const char* FIREBASE_DB_URL = "https://finalyearproject-2034b-default-rtdb.asia-southeast1.firebasedatabase.app";

// ---------- METER IDENTITY ----------
const char* METER_ID   = "meter1";
const char* METER_NAME = "ABB B24";

// ---------- Meter CONFIG ----------
#define METER_SLAVE_ID     10
#define MODBUS_BAUDRATE     19200

// ---------- Pin definitions ----------
#define RS485_DE_RE_PIN     4
#define RXD2                16
#define TXD2                17

ModbusMaster node;

// Ek hi dafa banate hain — baar baar naya client banane se memory
// fragment hoti hai aur ESP32 crash/reboot ho sakta hai
WiFiClientSecure secureClient;

unsigned long lastReadTime = 0;
const unsigned long READ_INTERVAL_MS = 2000; // har 2 second 'latest' update

unsigned long lastHistoryWriteTime = 0;
// History ko 2-second resolution ki zaroorat nahi (sirf daily total chahiye),
// isliye isay kam baar likhte hain — har cycle mein 1 extra HTTPS request
// hatane se 'latest' zyada consistently, bina lambe gap ke, update hoti hai
// (jo "Meter Offline" flicker ka masla fix karta hai).
const unsigned long HISTORY_INTERVAL_MS = 30000; // har 30 second

void preTransmission() {
  digitalWrite(RS485_DE_RE_PIN, HIGH);
}

void postTransmission() {
  digitalWrite(RS485_DE_RE_PIN, LOW);
}

void connectWiFi() {
  Serial.print("WiFi se connect ho raha hai");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println();
  Serial.println("WiFi Connected!");
  Serial.print("IP address: ");
  Serial.println(WiFi.localIP());

  // NTP se real calendar time set karte hain (Pakistan = UTC+5)
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

// Aaj ki date "YYYY-MM-DD" format mein deta hai, Firebase history key ke liye
String getTodayDateString() {
  time_t now = time(nullptr);
  struct tm timeinfo;
  localtime_r(&now, &timeinfo);
  char buf[11];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
           timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday);
  return String(buf);
}

// Voltage, Current, Power jaisi 32-bit (2-register) values ke liye
uint32_t read32(uint16_t startReg) {
  uint8_t result = node.readHoldingRegisters(startReg, 2);
  if (result == node.ku8MBSuccess) {
    uint32_t high = node.getResponseBuffer(0);
    uint32_t low  = node.getResponseBuffer(1);
    return (high << 16) | low;
  }
  Serial.print("Modbus read failed at reg 0x");
  Serial.print(startReg, HEX);
  Serial.print(" - error: ");
  Serial.println(result, HEX);
  return 0xFFFFFFFF;
}

// Frequency aur Power Factor jaisi 16-bit (1-register) values ke liye
int32_t read16(uint16_t reg, bool &ok) {
  uint8_t result = node.readHoldingRegisters(reg, 1);
  if (result == node.ku8MBSuccess) {
    ok = true;
    return (int16_t)node.getResponseBuffer(0);
  }
  Serial.print("Modbus read failed at reg 0x");
  Serial.print(reg, HEX);
  Serial.print(" - error: ");
  Serial.println(result, HEX);
  ok = false;
  return 0;
}

// Resettable Energy counter jaisi 64-bit (4-register) values ke liye
uint64_t read64(uint16_t startReg, bool &ok) {
  uint8_t result = node.readHoldingRegisters(startReg, 4);
  if (result == node.ku8MBSuccess) {
    ok = true;
    uint64_t r0 = node.getResponseBuffer(0);
    uint64_t r1 = node.getResponseBuffer(1);
    uint64_t r2 = node.getResponseBuffer(2);
    uint64_t r3 = node.getResponseBuffer(3);
    return (r0 << 48) | (r1 << 32) | (r2 << 16) | r3;
  }
  Serial.print("Modbus read failed at reg 0x");
  Serial.print(startReg, HEX);
  Serial.print(" - error: ");
  Serial.println(result, HEX);
  ok = false;
  return 0;
}

// Meter ka naam Firebase mein ek hi baar bhej deta hai (setup() se call hota hai)
void sendMeterName() {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(FIREBASE_DB_URL) + "/meters/" + METER_ID + "/name.json";
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");

  String json = "\"" + String(METER_NAME) + "\"";
  int httpCode = http.PUT(json);

  if (httpCode > 0) {
    Serial.print("Meter name set, response code: ");
    Serial.println(httpCode);
  } else {
    Serial.print("Meter name send failed: ");
    Serial.println(http.errorToString(httpCode));
  }
  http.end();
}

// Firebase ko sirf 'latest' reading bhejta hai (har cycle, tez rehne ke liye)
void sendLatestToFirebase(float voltage, float current, float powerW, float frequency, float powerFactor, float energyKwh) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  String url = String(FIREBASE_DB_URL) + "/meters/" + METER_ID + "/latest.json";
  http.begin(secureClient, url);
  http.addHeader("Content-Type", "application/json");

  String json = "{";
  json += "\"voltage\":" + String(voltage, 1) + ",";
  json += "\"current\":" + String(current, 2) + ",";
  json += "\"power\":"   + String(powerW, 2) + ",";
  json += "\"frequency\":" + String(frequency, 2) + ",";
  json += "\"powerFactor\":" + String(powerFactor, 3) + ",";
  json += "\"energy\":" + String(energyKwh, 2) + ",";
  json += "\"timestamp\":{\".sv\":\"timestamp\"}";
  json += "}";

  int httpCode = http.PUT(json);
  if (httpCode > 0) {
    Serial.print("Firebase (latest) updated, response code: ");
    Serial.println(httpCode);
  } else {
    Serial.print("Firebase (latest) send failed: ");
    Serial.println(http.errorToString(httpCode));
  }
  http.end();
}

// History ko alag se, kam baar (har 30 sec) likhta hai
void sendHistoryToFirebase(float energyKwh) {
  if (WiFi.status() != WL_CONNECTED) return;

  String dateKey = getTodayDateString();
  String historyUrl = String(FIREBASE_DB_URL) + "/meters/" + METER_ID + "/history/" + dateKey + ".json";
  HTTPClient http2;
  http2.begin(secureClient, historyUrl);
  http2.addHeader("Content-Type", "application/json");
  int code = http2.PUT(String(energyKwh, 2));
  Serial.print("Firebase (history) updated, response code: ");
  Serial.println(code);
  http2.end();
}

void updateAndSendReadings() {
  const uint16_t REG_VOLTAGE_L1     = 0x5B00;
  const uint16_t REG_CURRENT_L1     = 0x5B0C;
  const uint16_t REG_POWER_TOTAL    = 0x5B14;
  const uint16_t REG_FREQUENCY      = 0x5B2C; // 16-bit, unsigned, x0.01
  const uint16_t REG_POWER_FACTOR   = 0x5B3A; // 16-bit, signed,   x0.001
  const uint16_t REG_RESETTABLE_ENERGY = 0x552C; // 64-bit, unsigned, x0.01 kWh

  uint32_t rawVoltage = read32(REG_VOLTAGE_L1);

  // Agar meter se voltage hi na mile, to matlab meter off/disconnected hai —
  // Firebase ko bilkul update na karein, taake purana timestamp wahi rahe
  // aur app khud "Offline" detect kar le.
  if (rawVoltage == 0xFFFFFFFF) {
    Serial.println("Meter se koi jawab nahi mila — Firebase update skip kar rahe hain");
    return;
  }

  delay(150);
  uint32_t rawCurrent = read32(REG_CURRENT_L1);
  delay(150);
  uint32_t rawPower   = read32(REG_POWER_TOTAL);
  delay(150);

  bool freqOk = false, pfOk = false, energyOk = false;
  int32_t rawFrequency   = read16(REG_FREQUENCY, freqOk);
  delay(150);
  int32_t rawPowerFactor = read16(REG_POWER_FACTOR, pfOk);
  delay(150);
  uint64_t rawEnergy = read64(REG_RESETTABLE_ENERGY, energyOk);

  float voltage      = rawVoltage * 0.1;
  float current       = (rawCurrent != 0xFFFFFFFF) ? rawCurrent * 0.01 : 0;
  float powerW        = (rawPower   != 0xFFFFFFFF) ? ((int32_t)rawPower) * 0.01 : 0;
  float frequency      = freqOk ? rawFrequency * 0.01f  : 0;
  float powerFactor   = pfOk   ? rawPowerFactor * 0.001f : 0;
  float energyKwh     = energyOk ? (double)rawEnergy * 0.01 : 0;

  Serial.printf("[%s] V=%.1f  I=%.2f  P=%.2fW  Hz=%.2f  PF=%.3f  E=%.2fkWh\n",
                METER_ID, voltage, current, powerW, frequency, powerFactor, energyKwh);

  // 'latest' har cycle update hoti hai (tez, sirf 1 request)
  sendLatestToFirebase(voltage, current, powerW, frequency, powerFactor, energyKwh);

  // 'history' sirf har HISTORY_INTERVAL_MS baad likhte hain
  if (millis() - lastHistoryWriteTime >= HISTORY_INTERVAL_MS) {
    lastHistoryWriteTime = millis();
    sendHistoryToFirebase(energyKwh);
  }
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  connectWiFi();

  secureClient.setInsecure(); // FYP/prototype ke liye theek hai, production mein certificate use karein
  sendMeterName();

  Serial2.begin(MODBUS_BAUDRATE, SERIAL_8E1, RXD2, TXD2);
  pinMode(RS485_DE_RE_PIN, OUTPUT);
  digitalWrite(RS485_DE_RE_PIN, LOW);

  node.begin(METER_SLAVE_ID, Serial2);
  node.preTransmission(preTransmission);
  node.postTransmission(postTransmission);

  Serial.println("Setup complete. Meter data Firebase ko bhejna shuru...");
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi disconnect ho gaya, dobara connect kar rahe hain...");
    connectWiFi();
  }

  if (millis() - lastReadTime >= READ_INTERVAL_MS) {
    lastReadTime = millis();
    updateAndSendReadings();
  }
}
