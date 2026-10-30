/*
  ESP32 SMART AIR QUALITY IoT MONITOR + AQI + IAQ
  Target: Original ESP32 / ESP-WROOM-32
  Fixed: Memory crash on web dashboard (PROGMEM + send_P)
*/

#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <math.h>
#include <Adafruit_GFX.h>
#include <SPI.h>
#include <Adafruit_ILI9341.h>
#include <Adafruit_Sensor.h>
#include <bsec2.h>
#include <ThingSpeak.h>
#include <WiFiClientSecure.h>
#include <UniversalTelegramBot.h>
#include <ArduinoJson.h>
#include <BluetoothSerial.h>

// =============== USER CONFIGURATION ===============

const char* WIFI_SSID = "SSID";
const char* WIFI_PASSWORD = "PASSWORD";

const char* AP_NAME = "AirMonitor-ESP32";
const char* AP_PASSWORD = "AirMonitor123";

const char* TELEGRAM_TOKEN = "TOKEN";
const char* TELEGRAM_CHAT_ID = "CHAT ID";

unsigned long TS_CHANNEL_ID = 1234567;
const char* TS_WRITE_KEY = "YOUR_THINGSPEAK_WRITE_API_KEY";

const char* BT_DEVICE_NAME = "AirMonitor-ESP32";

// =============== HARDWARE PINS ===============

#define PMS_RX_PIN 16
#define PMS_TX_PIN 17
#define BUZZER_PIN 25
#define GREEN_LED_PIN 32
#define YELLOW_LED_PIN 33
#define RED_LED_PIN 27
#define TFT_CS_PIN   5
#define TFT_DC_PIN   2
#define TFT_RST_PIN  4
#define TFT_SCK_PIN  18
#define TFT_MISO_PIN 19
#define TFT_MOSI_PIN 23

// =============== OBJECTS ===============

HardwareSerial pmsSerial(2);
Bsec2 airSensor;
Adafruit_ILI9341 tft(TFT_CS_PIN, TFT_DC_PIN, TFT_RST_PIN);
WebServer server(80);
BluetoothSerial SerialBT;

WiFiClient tsClient;
WiFiClientSecure telegramClient;
UniversalTelegramBot* telegramBot = nullptr;

// =============== SENSOR VALUES ===============

float pm1 = 0;
float pm25 = 0;
float pm10 = 0;
float tempC = 0;
float humidity = 0;
float pressureHPa = 0;
float gasKOhm = 0;
float estimatedCO2ppm = -1.0f;
float estimatedVOCppm = -1.0f;
float bsecIAQ = -1.0f;
uint8_t bsecIAQAccuracy = 0;

bool pmsReady = false;
bool bmeReady = false;
bool tftReady = false;

String airStatus = "Waiting";
String lastAlertStatus = "";

bool buzzerManualRequest = false;
bool buzzerOutput = false;

// AQI category alarm: 30 seconds when AQI enters Unhealthy or worse.
const unsigned long AQI_ALARM_DURATION_MS = 30000UL;
const unsigned long SLOW_BLINK_INTERVAL_MS = 2000UL;
const unsigned long FAST_BLINK_INTERVAL_MS = 1000UL;
unsigned long aqiAlarmUntil = 0;
int lastAQICategory = -1;
unsigned long lastBuzzerToggle = 0;
bool buzzerBlinkState = false;
bool aqiAlertActive = false;

// =============== CUSTOM DATA TYPES ===============

struct AQIInfo {
  int value;
  const char* status;
};

struct IAQInfo {
  int value;
  const char* state;
};

// =============== AQI DATA ===============

AQIInfo aqi = {-1, "Waiting"};

AQIInfo calculateAQI(float concentration) {
  if (!pmsReady || !isfinite(concentration) || concentration < 0) {
    return {-1, "Waiting"};
  }

  float c = floorf(concentration);
  const float cLow[] = {0, 31, 61, 91, 121, 251};
  const float cHigh[] = {30, 60, 90, 120, 250, 500};
  const int iLow[] = {0, 51, 101, 201, 301, 401};
  const int iHigh[] = {50, 100, 200, 300, 400, 500};
  const char* labels[] = {"Good", "Satisfactory", "Moderate", "Poor", "Very Poor", "Severe"};

  if (c > 500) c = 500;
  for (int i = 0; i < 6; i++) {
    if (c >= cLow[i] && c <= cHigh[i]) {
      float value = ((float)(iHigh[i] - iLow[i]) / (cHigh[i] - cLow[i])) *
                    (c - cLow[i]) + iLow[i];
      return {(int)roundf(value), labels[i]};
    }
  }
  return {-1, "Waiting"};
}

void updateAQI() {
  aqi = calculateAQI(pm25);
}

int getAQICategoryIndex() {
  if (aqi.value < 0) return -1;
  if (aqi.value <= 50) return 0;
  if (aqi.value <= 100) return 1;
  if (aqi.value <= 200) return 2;
  if (aqi.value <= 300) return 3;
  if (aqi.value <= 400) return 4;
  return 5;
}

// =============== PROJECT IAQ SCORE ===============

IAQInfo iaq = {-1, "Waiting"};

IAQInfo calculateIAQ(float pm) {
  if (!pmsReady || !isfinite(pm) || pm < 0) {
    return {-1, "Waiting"};
  }

  int score = (int)roundf((pm / (pm + 25.0f)) * 100.0f);
  score = constrain(score, 0, 100);

  if (pm <= 15.0f) return {score, "Good"};
  if (pm <= 35.0f) return {score, "Moderate"};
  if (pm <= 55.0f) return {score, "Poor"};
  return {score, "Unhealthy"};
}

void updateIAQ() {
  iaq = calculateIAQ(pm25);
}

// =============== TIMERS ===============

unsigned long lastBmeRead = 0;
unsigned long lastDisplayUpdate = 0;
unsigned long lastTsUpload = 0;
unsigned long lastTelegramPoll = 0;
unsigned long lastWifiRetry = 0;
unsigned long lastPmsValidFrame = 0;

const unsigned long TS_INTERVAL = 60000;
const unsigned long BME_INTERVAL = 2000;
const unsigned long DISPLAY_INTERVAL = 2000;
const unsigned long TELEGRAM_INTERVAL = 1500;
const unsigned long WIFI_RETRY_INTERVAL = 10000;

// =============== BLUETOOTH CLASSIC SPP ===============

bool btEnabled = false;
bool btConnected = false;
bool btWasConnected = false;

void startBluetooth() {
  if (btEnabled) return;
  SerialBT.begin(BT_DEVICE_NAME);
  btEnabled = true;
  btConnected = false;
  btWasConnected = false;
}

void stopBluetooth() {
  if (!btEnabled) return;
  SerialBT.end();
  btEnabled = false;
  btConnected = false;
  btWasConnected = false;
}

void btReply(const String& message) {
  if (btEnabled) SerialBT.println(message);
}

// Forward declaration to prevent prototype issues
String processCommand(String command);

void checkBluetoothCommands() {
  if (!btEnabled) return;

  bool nowConnected = SerialBT.hasClient();
  if (nowConnected != btConnected) {
    btConnected = nowConnected;
    if (btConnected) {
      SerialBT.println("Connected to AirMonitor-ESP32. Type HELP for commands.");
    }
  }

  while (SerialBT.available()) {
    String command = SerialBT.readStringUntil('\n');
    command.trim();
    if (command.length() == 0) continue;
    btReply(processCommand(command));
  }
}

// =============== PMS FRAME PARSER ===============

uint8_t pmsFrame[32];
uint8_t pmsIndex = 0;

void readPMS() {
  while (pmsSerial.available()) {
    uint8_t c = pmsSerial.read();

    if (pmsIndex == 0) {
      if (c == 0x42) pmsFrame[pmsIndex++] = c;
      continue;
    }

    if (pmsIndex == 1) {
      if (c == 0x4D) {
        pmsFrame[pmsIndex++] = c;
      } else {
        pmsIndex = (c == 0x42) ? 1 : 0;
        if (pmsIndex == 1) pmsFrame[0] = 0x42;
      }
      continue;
    }

    pmsFrame[pmsIndex++] = c;

    if (pmsIndex == 32) {
      uint16_t checksum = 0;
      for (int i = 0; i < 30; i++) checksum += pmsFrame[i];
      uint16_t receivedChecksum = ((uint16_t)pmsFrame[30] << 8) | pmsFrame[31];
      uint16_t frameLength = ((uint16_t)pmsFrame[2] << 8) | pmsFrame[3];

      if (checksum == receivedChecksum && frameLength >= 28) {
        pm1 = ((uint16_t)pmsFrame[10] << 8) | pmsFrame[11];
        pm25 = ((uint16_t)pmsFrame[12] << 8) | pmsFrame[13];
        pm10 = ((uint16_t)pmsFrame[14] << 8) | pmsFrame[15];

        pmsReady = true;
        lastPmsValidFrame = millis();
        updateAQI();
        updateIAQ();
      }
      pmsIndex = 0;
    }
  }

  if (pmsReady && millis() - lastPmsValidFrame > 10000) {
    pmsReady = false;
    updateAQI();
    updateIAQ();
  }
}

// =============== BME680 + BOSCH BSEC2 ===============

void bsecNewData(bme68x_data data, bsecOutputs outputs, Bsec2 bsec) {
  (void)data;
  (void)bsec;

  for (uint8_t i = 0; i < outputs.nOutputs; i++) {
    const bsecData output = outputs.output[i];
    switch (output.sensor_id) {
      case BSEC_OUTPUT_IAQ:
        bsecIAQ = output.signal;
        bsecIAQAccuracy = output.accuracy;
        break;
      case BSEC_OUTPUT_CO2_EQUIVALENT:
        estimatedCO2ppm = output.signal;
        break;
      case BSEC_OUTPUT_BREATH_VOC_EQUIVALENT:
        estimatedVOCppm = output.signal;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE:
        tempC = output.signal;
        break;
      case BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY:
        humidity = output.signal;
        break;
      case BSEC_OUTPUT_RAW_PRESSURE:
        pressureHPa = output.signal / 100.0f;
        break;
      case BSEC_OUTPUT_RAW_GAS:
        gasKOhm = output.signal / 1000.0f;
        break;
    }
  }
}

void readBME() {
  if (!bmeReady) return;
  if (!airSensor.run()) {
    if (airSensor.status < BSEC_OK) Serial.println(airSensor.status);
    if (airSensor.sensor.status < BME68X_OK) Serial.println(airSensor.sensor.status);
  }
}

// =============== AIR STATUS AND OUTPUTS ===============

String calculateAirStatus() {
  if (!pmsReady) return "Waiting";
  updateAQI();
  return String(aqi.status);
}

void updateOutputs() {
  airStatus = calculateAirStatus();
  updateAQI();
  updateIAQ();

  int category = getAQICategoryIndex();

  digitalWrite(GREEN_LED_PIN, (category == 0 || category == 1) ? HIGH : LOW);
  digitalWrite(YELLOW_LED_PIN, category == 2 ? HIGH : LOW);
  digitalWrite(RED_LED_PIN, category >= 3 ? HIGH : LOW);

  if (category < 3) {
    aqiAlarmUntil = 0;
    aqiAlertActive = false;
    buzzerBlinkState = false;
    buzzerOutput = false;
    digitalWrite(BUZZER_PIN, LOW);
  }

  bool alarmWindowActive = (category >= 3 && aqiAlarmUntil != 0 && (long)(aqiAlarmUntil - millis()) > 0);

  if (!alarmWindowActive) {
    aqiAlarmUntil = 0;
    aqiAlertActive = false;
    buzzerBlinkState = false;
  }

  bool alarmBuzzer = false;
  if (alarmWindowActive) {
    if (category == 5) {
      alarmBuzzer = true;
    } else if (category == 4 || category == 3) {
      unsigned long interval = (category == 4) ? FAST_BLINK_INTERVAL_MS : SLOW_BLINK_INTERVAL_MS;
      if (millis() - lastBuzzerToggle >= interval) {
        lastBuzzerToggle = millis();
        buzzerBlinkState = !buzzerBlinkState;
      }
      alarmBuzzer = buzzerBlinkState;
    }
  }

  bool manualAlarm = buzzerManualRequest && category >= 3;
  buzzerOutput = alarmBuzzer || manualAlarm;
  digitalWrite(BUZZER_PIN, buzzerOutput ? HIGH : LOW);
}

// =============== STATUS TEXT ===============

String makeStatusText() {
  updateAQI();
  updateIAQ();

  String s = "ESP32 Air Monitor\n";
  s += "IAQ Value: " + ((iaq.value < 0) ? String("Waiting") : String(iaq.value) + "/100") + "\n";
  s += "IAQ State: " + String(iaq.state) + "\n";
  s += "Estimated AQI: " + ((aqi.value < 0) ? String("Waiting") : String(aqi.value)) + "\n";
  s += "AQI category: " + String(aqi.status) + "\n";
  s += "PM2.5: " + String(pm25, 1) + " ug/m3\n";
  s += "Temp: " + (bmeReady ? String(tempC, 1) + " C\n" : "Unavailable\n");
  s += "Humidity: " + (bmeReady ? String(humidity, 1) + " %\n" : "Unavailable\n");
  return s;
}

String makeAQIText() {
  updateAQI();
  String s = "Estimated PM2.5 AQI report\n";
  s += "AQI: " + ((aqi.value < 0) ? String("Waiting") : String(aqi.value)) + "\n";
  s += "Category: " + String(aqi.status) + "\n";
  s += "PM2.5: " + String(pm25, 1) + " ug/m3\n";
  return s;
}

String makeIAQText() {
  updateIAQ();
  String s = "Indoor Air Quality Report\n";
  s += "IAQ Value: " + ((iaq.value < 0) ? String("Waiting") : String(iaq.value) + "/100") + "\n";
  s += "IAQ State: " + String(iaq.state) + "\n";
  return s;
}

String processCommand(String command) {
  command.trim();
  command.toUpperCase();

  if (command == "STATUS") return makeStatusText();
  if (command == "AQI") return makeAQIText();
  if (command == "IAQ") return makeIAQText();

  if (command == "BUZZER ON") {
    buzzerManualRequest = true;
    updateOutputs();
    return buzzerOutput ? "Buzzer enabled." : "Buzzer remains OFF: status is Good.";
  }
  if (command == "BUZZER OFF") {
    buzzerManualRequest = false;
    updateOutputs();
    return "Buzzer disabled.";
  }
  if (command == "HELP") return "Commands: STATUS, AQI, IAQ, BUZZER ON, BUZZER OFF, HELP";
  if (command == "BLUETOOTH ON" || command == "BT ON") { startBluetooth(); return "Bluetooth is ON."; }
  
  return "Unknown command. Send HELP.";
}

// =============== TELEGRAM ===============

void sendTelegram(String message) {
  if (telegramBot == nullptr || WiFi.status() != WL_CONNECTED) return;
  telegramBot->sendMessage(TELEGRAM_CHAT_ID, message, "");
}

String makeAQIAlertText() {
  updateAQI();
  String s = "AIR QUALITY AQI ALERT\n";
  s += "AQI: " + String(aqi.value) + "\n";
  s += "Category: " + String(aqi.status) + "\n";
  s += "PM2.5: " + String(pm25, 1) + " ug/m3\n";
  return s;
}

void checkAQICategoryAlert() {
  int category = getAQICategoryIndex();
  if (category < 0) return;

  if (lastAQICategory < 0) {
    lastAQICategory = category;
    return;
  }

  if (category > lastAQICategory && category >= 3) {
    aqiAlarmUntil = millis() + AQI_ALARM_DURATION_MS;
    lastBuzzerToggle = millis();
    buzzerBlinkState = (category == 5);
    aqiAlertActive = true;
    sendTelegram(makeAQIAlertText());
  }
  lastAQICategory = category;
}

void checkTelegram() {
  if (telegramBot == nullptr || WiFi.status() != WL_CONNECTED) return;
  int count = telegramBot->getUpdates(telegramBot->last_message_received + 1);
  
  while (count > 0) {
    for (int i = 0; i < count; i++) {
      String chatId = telegramBot->messages[i].chat_id;
      String command = telegramBot->messages[i].text;
      if (chatId != TELEGRAM_CHAT_ID) continue;
      command.trim();
      command.toLowerCase();

      if (command == "/start" || command == "/help") {
        sendTelegram("Commands:\n/status\n/aqi\n/iaq\n/bluetooth_on\n/buzzer_on\n/buzzer_off");
      } else if (command == "/status") {
        sendTelegram(makeStatusText());
      } else if (command == "/aqi") {
        sendTelegram(makeAQIText());
      } else if (command == "/iaq") {
        sendTelegram(makeIAQText());
      } else if (command == "/bluetooth_on") {
        startBluetooth(); sendTelegram("Bluetooth enabled.");
      } else if (command == "/buzzer_on") {
        buzzerManualRequest = true; updateOutputs(); sendTelegram("Buzzer enabled.");
      } else if (command == "/buzzer_off") {
        buzzerManualRequest = false; updateOutputs(); sendTelegram("Buzzer disabled.");
      }
    }
    count = telegramBot->getUpdates(telegramBot->last_message_received + 1);
  }
}

// =============== WEB DASHBOARD (STORED IN FLASH) ===============

// The 'PROGMEM' keyword stores this giant string in Flash memory instead of RAM.
// This prevents the ESP32 from crashing and fixes the flashing white screen.
const char DASHBOARD_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Air Quality Monitor</title>
<style>
  *{box-sizing:border-box;margin:0;padding:0}
  :root{
    --bg:#f8fafc; --card:#ffffff; --border:#e2e8f0; --border-soft:#eef2f7;
    --text:#0f172a; --text-2:#475569; --muted:#94a3b8;
    --accent:#0284c7; --accent-soft:#e0f2fe;
    --good:#16a34a; --sat:#65a30d; --mod:#ca8a04;
    --poor:#ea580c; --vpoor:#dc2626; --sev:#991b1b;
    --shadow:0 1px 2px rgba(15,23,42,.04),0 1px 3px rgba(15,23,42,.06);
    --shadow-lg:0 20px 40px -12px rgba(15,23,42,.18),0 8px 16px -8px rgba(15,23,42,.1);
  }
  body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,'Helvetica Neue',Arial,sans-serif;
    background:var(--bg); color:var(--text); min-height:100vh; padding:32px 24px; -webkit-font-smoothing:antialiased}
  .wrap{max-width:1160px;margin:0 auto}
  header{display:flex;justify-content:space-between;align-items:center;margin-bottom:28px;flex-wrap:wrap;gap:16px}
  .brand{display:flex;align-items:center;gap:12px}
  .brand-mark{width:40px;height:40px;border-radius:10px;background:linear-gradient(135deg,#0284c7,#0ea5e9);
    display:flex;align-items:center;justify-content:center}
  .brand-mark svg{width:22px;height:22px;stroke:#fff;stroke-width:2.2;fill:none;stroke-linecap:round;stroke-linejoin:round}
  h1{font-size:18px;font-weight:600;letter-spacing:-.01em}
  h1 span{color:var(--muted);font-weight:400}
  .header-actions{display:flex;gap:10px;align-items:center}
  .status-pill{display:inline-flex;align-items:center;gap:8px;padding:8px 14px;border-radius:100px;
    background:var(--card);border:1px solid var(--border);font-size:13px;font-weight:500;
    color:var(--text-2);box-shadow:var(--shadow);transition:all .2s}
  .status-pill.offline{border-color:#fecaca;background:#fef2f2;color:#991b1b}
  .status-pill.reconnecting{border-color:#fde68a;background:#fffbeb;color:#92400e}
  .status-dot{width:7px;height:7px;border-radius:50%;background:var(--good);
    box-shadow:0 0 0 3px rgba(22,163,74,.15);animation:pulse 2s infinite}
  .status-pill.offline .status-dot{background:#dc2626;box-shadow:0 0 0 3px rgba(220,38,38,.15)}
  .status-pill.reconnecting .status-dot{background:#ca8a04;box-shadow:0 0 0 3px rgba(202,138,4,.15)}
  @keyframes pulse{0%,100%{opacity:1}50%{opacity:.5}}
  .icon-btn{width:36px;height:36px;border-radius:10px;border:1px solid var(--border);background:var(--card);
    cursor:pointer;display:flex;align-items:center;justify-content:center;color:var(--text-2);
    transition:all .15s;box-shadow:var(--shadow)}
  .icon-btn:hover{background:#f1f5f9;border-color:#cbd5e1;color:var(--accent)}
  .icon-btn svg{width:16px;height:16px;stroke-width:2;fill:none;stroke-linecap:round;stroke-linejoin:round}
  .grid{display:grid;grid-template-columns:repeat(12,1fr);gap:16px}
  .card{background:var(--card);border:1px solid var(--border);border-radius:14px;padding:22px;
    box-shadow:var(--shadow);transition:box-shadow .2s,border-color .2s}
  .card:hover{box-shadow:0 4px 12px rgba(15,23,42,.06),0 1px 3px rgba(15,23,42,.05);border-color:#cbd5e1}
  .card-label{font-size:12px;font-weight:600;color:var(--muted);text-transform:uppercase;
    letter-spacing:.06em;margin-bottom:16px;display:flex;justify-content:space-between;align-items:center;gap:8px}
  .card-label .label-text{display:inline-flex;align-items:center;gap:8px}
  .card-label .label-text svg{width:14px;height:14px;stroke:currentColor;stroke-width:2;fill:none;
    stroke-linecap:round;stroke-linejoin:round}
  .card-label .badge{text-transform:none;letter-spacing:0;font-size:11px;color:var(--accent);
    background:var(--accent-soft);padding:3px 8px;border-radius:6px;font-weight:600}
  .hero{grid-column:span 7;display:flex;align-items:center;gap:28px}
  .gauge{position:relative;width:200px;height:200px;flex-shrink:0}
  .gauge svg{transform:rotate(-90deg)}
  .gauge-center{position:absolute;inset:0;display:flex;flex-direction:column;align-items:center;justify-content:center}
  .gauge-num{font-size:52px;font-weight:700;line-height:1;color:var(--text);letter-spacing:-.03em;
    font-variant-numeric:tabular-nums}
  .gauge-cat{font-size:13px;font-weight:600;color:var(--text-2);margin-top:6px}
  .hero-info{flex:1;min-width:0}
  .hero-info .title{font-size:15px;font-weight:600;color:var(--text);margin-bottom:10px;
    display:flex;align-items:center;gap:8px}
  .hero-info .title svg{width:16px;height:16px;stroke:var(--accent);stroke-width:2.2;fill:none;
    stroke-linecap:round;stroke-linejoin:round}
  .hero-chip{display:inline-flex;align-items:center;gap:6px;padding:6px 12px;border-radius:100px;
    font-size:12px;font-weight:600}
  .hero-chip.good{background:#dcfce7;color:var(--good)}
  .hero-chip.sat{background:#ecfccb;color:var(--sat)}
  .hero-chip.mod{background:#fef9c3;color:var(--mod)}
  .hero-chip.poor{background:#ffedd5;color:var(--poor)}
  .hero-chip.vpoor{background:#fee2e2;color:var(--vpoor)}
  .hero-chip.sev{background:#fecaca;color:var(--sev)}
  .hero-meta{margin-top:18px;padding-top:16px;border-top:1px solid var(--border-soft);display:flex;gap:24px}
  .hero-meta div{display:flex;flex-direction:column;gap:3px}
  .hero-meta .k{font-size:11px;color:var(--muted);font-weight:500;text-transform:uppercase;
    letter-spacing:.04em;display:flex;align-items:center;gap:5px}
  .hero-meta .k svg{width:11px;height:11px;stroke:currentColor;stroke-width:2.2;fill:none;
    stroke-linecap:round;stroke-linejoin:round;opacity:.7}
  .hero-meta .v{font-size:15px;font-weight:600;color:var(--text);font-variant-numeric:tabular-nums}
  .iaq{grid-column:span 5}
  .iaq-num{font-size:44px;font-weight:700;line-height:1;color:var(--text);letter-spacing:-.03em;
    font-variant-numeric:tabular-nums}
  .iaq-num small{font-size:18px;font-weight:500;color:var(--muted);margin-left:4px}
  .iaq-state{font-size:13px;color:var(--text-2);margin-top:8px;font-weight:500}
  .iaq-track{height:8px;border-radius:100px;background:var(--border-soft);overflow:hidden;margin-top:20px}
  .iaq-fill{height:100%;border-radius:100px;background:linear-gradient(90deg,#dc2626,#ca8a04,#16a34a);
    background-size:300% 100%;background-position:right;
    transition:width .8s cubic-bezier(.4,0,.2,1),background-position .8s}
  .iaq-scale{display:flex;justify-content:space-between;margin-top:8px;font-size:11px;
    color:var(--muted);font-weight:500}
  .metric{grid-column:span 3;padding:20px;position:relative;overflow:hidden}
  .metric-head{display:flex;align-items:center;gap:12px;margin-bottom:14px}
  .metric-icon{width:40px;height:40px;border-radius:10px;display:flex;align-items:center;
    justify-content:center;flex-shrink:0;transition:transform .25s cubic-bezier(.34,1.4,.64,1),box-shadow .2s}
  .metric-icon svg{width:22px;height:22px;stroke-width:2;fill:none;stroke-linecap:round;stroke-linejoin:round}
  .metric:hover .metric-icon{transform:scale(1.08)}
  .metric-icon.blue  {background:#e0f2fe;color:#0284c7;box-shadow:inset 0 0 0 1px #bae6fd}
  .metric-icon.green {background:#dcfce7;color:#16a34a;box-shadow:inset 0 0 0 1px #bbf7d0}
  .metric-icon.amber {background:#fef3c7;color:#ca8a04;box-shadow:inset 0 0 0 1px #fde68a}
  .metric-icon.purple{background:#ede9fe;color:#7c3aed;box-shadow:inset 0 0 0 1px #ddd6fe}
  .metric-icon.rose  {background:#ffe4e6;color:#e11d48;box-shadow:inset 0 0 0 1px #fecdd3}
  .metric-icon.teal  {background:#ccfbf1;color:#0d9488;box-shadow:inset 0 0 0 1px #99f6e4}
  .metric-icon.orange{background:#ffedd5;color:#ea580c;box-shadow:inset 0 0 0 1px #fed7aa}
  .metric-icon.slate {background:#f1f5f9;color:#475569;box-shadow:inset 0 0 0 1px #e2e8f0}
  .metric-label{font-size:11px;font-weight:600;color:var(--text-2);text-transform:uppercase;letter-spacing:.06em}
  .metric-value{font-size:26px;font-weight:700;color:var(--text);letter-spacing:-.02em;
    font-variant-numeric:tabular-nums;line-height:1.1}
  .metric-value .unit{font-size:12px;font-weight:500;color:var(--muted);margin-left:3px;letter-spacing:0}
  .chart-card{grid-column:span 12;padding:22px}
  .chart-head{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px;
    flex-wrap:wrap;gap:12px}
  .chart-head h3{font-size:14px;font-weight:600;color:var(--text);display:flex;align-items:center;gap:8px}
  .chart-head h3 svg{width:16px;height:16px;stroke:var(--accent);stroke-width:2.2;fill:none;
    stroke-linecap:round;stroke-linejoin:round}
  .legend{display:flex;gap:16px;font-size:12px;color:var(--text-2);font-weight:500}
  .legend span{display:inline-flex;align-items:center;gap:6px}
  .legend i{width:10px;height:10px;border-radius:3px;display:inline-block}
  canvas{width:100%;height:180px;display:block}
  .controls{grid-column:span 12;display:flex;flex-wrap:wrap;gap:10px;padding:18px 22px;align-items:center}
  .controls-label{font-size:12px;font-weight:600;color:var(--muted);text-transform:uppercase;
    letter-spacing:.06em;width:100%;margin-bottom:4px}
  .controls-divider{width:100%;height:1px;background:var(--border-soft);margin:8px 0 4px}
  .btn{display:inline-flex;align-items:center;gap:8px;padding:9px 16px;border:1px solid var(--border);
    border-radius:9px;background:var(--card);color:var(--text);font-size:13px;font-weight:500;
    font-family:inherit;cursor:pointer;transition:all .15s}
  .btn svg{width:14px;height:14px;stroke-width:2;fill:none;stroke-linecap:round;stroke-linejoin:round}
  .btn:hover{background:#f8fafc;border-color:#cbd5e1}
  .btn.primary{background:var(--accent);color:#fff;border-color:var(--accent)}
  .btn.primary:hover{background:#0369a1;border-color:#0369a1}
  .btn.danger{color:#dc2626;border-color:#fecaca}
  .btn.danger:hover{background:#fef2f2;border-color:#fca5a5}
  .btn.ghost{margin-left:auto}
  .btn.ghost:hover{background:#f1f5f9}
  footer{margin-top:32px;padding-top:20px;border-top:1px solid var(--border);
    display:flex;justify-content:space-between;align-items:center;font-size:12px;
    color:var(--muted);flex-wrap:wrap;gap:8px}
  .toast-container{position:fixed;bottom:24px;right:24px;display:flex;flex-direction:column;
    gap:10px;z-index:1000;pointer-events:none}
  .toast{display:flex;align-items:flex-start;gap:12px;min-width:320px;max-width:420px;
    padding:14px 16px;background:#fff;border:1px solid var(--border);border-radius:12px;
    box-shadow:var(--shadow-lg);pointer-events:auto;animation:toastIn .3s cubic-bezier(.4,0,.2,1);
    position:relative;overflow:hidden}
  .toast.leaving{animation:toastOut .25s ease forwards}
  @keyframes toastIn{from{transform:translateY(20px);opacity:0}to{transform:translateY(0);opacity:1}}
  @keyframes toastOut{to{transform:translateX(120%);opacity:0}}
  .toast-icon{width:32px;height:32px;border-radius:8px;flex-shrink:0;display:flex;
    align-items:center;justify-content:center}
  .toast-icon svg{width:16px;height:16px;stroke-width:2.2;fill:none;stroke-linecap:round;stroke-linejoin:round}
  .toast.success .toast-icon{background:#dcfce7;color:var(--good)}
  .toast.info .toast-icon{background:var(--accent-soft);color:var(--accent)}
  .toast.warning .toast-icon{background:#fef3c7;color:var(--mod)}
  .toast.error .toast-icon{background:#fee2e2;color:var(--vpoor)}
  .toast.offline .toast-icon{background:#fee2e2;color:var(--vpoor)}
  .toast.reconnecting .toast-icon{background:#fef3c7;color:var(--mod)}
  @keyframes spin{to{transform:rotate(360deg)}}
  .toast-body{flex:1;min-width:0}
  .toast-title{font-size:13px;font-weight:600;color:var(--text);margin-bottom:2px}
  .toast-msg{font-size:12px;color:var(--text-2);line-height:1.5}
  .toast-close{background:none;border:0;cursor:pointer;color:var(--muted);padding:2px;
    border-radius:4px;display:flex;align-items:center}
  .toast-close:hover{background:var(--border-soft);color:var(--text)}
  .toast-close svg{width:14px;height:14px;stroke-width:2;fill:none;stroke-linecap:round;stroke-linejoin:round}
  .toast-progress{position:absolute;bottom:0;left:0;height:2px;background:var(--accent);
    animation:progress linear forwards}
  .toast.success .toast-progress{background:var(--good)}
  .toast.warning .toast-progress{background:var(--mod)}
  .toast.error .toast-progress{background:var(--vpoor)}
  @keyframes progress{from{width:100%}to{width:0}}
  .toast.sticky{min-width:340px}
  .toast.sticky .toast-close{display:none}
  .toast.sticky .toast-action{padding:4px 10px;border-radius:6px;border:1px solid var(--border);
    background:#fff;font-size:11px;font-weight:600;color:var(--text-2);cursor:pointer;
    margin-top:8px;font-family:inherit}
  .modal-backdrop{position:fixed;inset:0;background:rgba(15,23,42,.4);backdrop-filter:blur(2px);
    display:flex;align-items:center;justify-content:center;z-index:900;padding:20px;
    opacity:0;pointer-events:none;transition:opacity .2s}
  .modal-backdrop.open{opacity:1;pointer-events:auto}
  .modal{background:#fff;border-radius:16px;max-width:480px;width:100%;box-shadow:var(--shadow-lg);
    overflow:hidden;transform:scale(.95);transition:transform .25s cubic-bezier(.34,1.4,.64,1)}
  .modal-backdrop.open .modal{transform:scale(1)}
  .modal-header{padding:20px 22px 16px;border-bottom:1px solid var(--border-soft);
    display:flex;align-items:flex-start;gap:14px}
  .modal-icon{width:40px;height:40px;border-radius:10px;flex-shrink:0;display:flex;
    align-items:center;justify-content:center}
  .modal-icon svg{width:20px;height:20px;stroke-width:2.2;fill:none;stroke-linecap:round;stroke-linejoin:round}
  .modal-icon.info{background:var(--accent-soft);color:var(--accent)}
  .modal-icon.danger{background:#fee2e2;color:var(--vpoor)}
  .modal-title{font-size:16px;font-weight:600;color:var(--text);margin-bottom:4px;letter-spacing:-.01em}
  .modal-subtitle{font-size:13px;color:var(--text-2);line-height:1.5}
  .modal-close{background:none;border:0;cursor:pointer;color:var(--muted);padding:6px;
    border-radius:6px;margin:-4px -4px 0 auto;display:flex;align-items:center}
  .modal-close:hover{background:var(--border-soft);color:var(--text)}
  .modal-close svg{width:16px;height:16px;stroke-width:2;fill:none;stroke-linecap:round;stroke-linejoin:round}
  .modal-body{padding:20px 22px;max-height:60vh;overflow-y:auto;font-size:13px;
    color:var(--text-2);line-height:1.65}
  .modal-footer{padding:16px 22px 20px;display:flex;justify-content:flex-end;gap:10px;
    background:#fafbfc;align-items:center}
  .alert-countdown{padding:14px 22px;background:#fef2f2;border-top:1px solid #fecaca;
    display:flex;align-items:center;gap:12px;font-size:13px;color:#991b1b;font-weight:500}
  .alert-countdown .countdown-bar{flex:1;height:4px;background:#fecaca;border-radius:2px;overflow:hidden}
  .alert-countdown .countdown-fill{height:100%;background:#dc2626;width:100%;transition:width 1s linear}
  .alert-countdown b{font-variant-numeric:tabular-nums;min-width:14px;text-align:center;font-size:14px}
  .drawer-backdrop{position:fixed;inset:0;background:rgba(15,23,42,.4);backdrop-filter:blur(2px);
    z-index:950;opacity:0;pointer-events:none;transition:opacity .25s}
  .drawer-backdrop.open{opacity:1;pointer-events:auto}
  .drawer{position:fixed;top:0;right:0;bottom:0;width:440px;max-width:100vw;background:#fff;
    box-shadow:-24px 0 48px -12px rgba(15,23,42,.18);display:flex;flex-direction:column;
    z-index:951;transform:translateX(100%);transition:transform .3s cubic-bezier(.4,0,.2,1)}
  .drawer.open{transform:translateX(0)}
  .drawer-header{padding:22px 24px 18px;border-bottom:1px solid var(--border-soft);
    display:flex;align-items:flex-start;gap:14px;flex-shrink:0}
  .drawer-body{flex:1;overflow-y:auto;padding:22px 24px;font-size:13px;color:var(--text-2);line-height:1.65}
  .drawer-footer{padding:16px 24px 20px;border-top:1px solid var(--border-soft);display:flex;
    justify-content:flex-end;gap:10px;background:#fafbfc;flex-shrink:0}
  .aqi-list{display:flex;flex-direction:column;gap:6px;margin-top:12px}
  .aqi-row{display:flex;align-items:center;gap:12px;padding:8px 12px;border-radius:8px;font-size:12px}
  .aqi-row .range{font-weight:600;font-variant-numeric:tabular-nums;min-width:80px}
  .aqi-row .name{flex:1;font-weight:500}
  .aqi-row .swatch{width:12px;height:12px;border-radius:3px;flex-shrink:0}
  @media(max-width:900px){ .hero,.iaq{grid-column:span 12} .metric{grid-column:span 6} .hero{flex-direction:column;text-align:center} }
  @media(max-width:520px){ .metric{grid-column:span 12} .toast-container{left:16px;right:16px;bottom:16px} .toast{min-width:0;max-width:none} .modal-footer{flex-direction:column-reverse} .modal-footer .btn{width:100%;justify-content:center} .drawer{width:100vw} }
</style>
</head>
<body>
<div class="wrap">
<header>
  <div class="brand">
    <div class="brand-mark"><svg viewBox="0 0 24 24"><path d="M3 12h3l2-7 4 14 2-7h4"/></svg></div>
    <h1>Air Quality <span>Monitor</span></h1>
  </div>
  <div class="header-actions">
    <div class="status-pill" id="statusPill">
      <span class="status-dot"></span><span id="statusText">Connecting…</span>
    </div>
    <button class="icon-btn" onclick="openDrawer()"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><path d="M12 16v-4M12 8h.01"/></svg></button>
  </div>
</header>

<div class="grid">
  <div class="card hero">
    <div class="gauge">
      <svg width="200" height="200" viewBox="0 0 200 200">
        <circle cx="100" cy="100" r="84" stroke="#eef2f7" stroke-width="14" fill="none"/>
        <circle id="arc" cx="100" cy="100" r="84" stroke="#94a3b8" stroke-width="14" fill="none"
          stroke-linecap="round" stroke-dasharray="528" stroke-dashoffset="528"
          style="transition:stroke-dashoffset .9s cubic-bezier(.4,0,.2,1),stroke .5s"/>
      </svg>
      <div class="gauge-center">
        <div class="gauge-num" id="aqiNum">--</div>
        <div class="gauge-cat" id="aqiCat">Waiting</div>
      </div>
    </div>
    <div class="hero-info">
      <div class="title"><svg viewBox="0 0 24 24"><path d="M3 12h3l2-7 4 14 2-7h4"/></svg>Air Quality Index</div>
      <span class="hero-chip" id="aqiChip">Waiting</span>
      <div class="hero-meta">
        <div><span class="k"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3" fill="currentColor" stroke="none"/></svg>PM2.5</span><span class="v"><span id="pm25">--</span> µg/m³</span></div>
        <div><span class="k"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="5" fill="currentColor" stroke="none"/></svg>PM10</span><span class="v"><span id="pm10">--</span> µg/m³</span></div>
      </div>
    </div>
  </div>

  <div class="card iaq">
    <div class="card-label"><span class="label-text"><svg viewBox="0 0 24 24"><path d="M11 20A7 7 0 0 1 9.8 6.1C15.5 5 17 4.48 19 2c1 2 2 4.18 2 8 0 5.5-4.78 10-10 10z"/></svg>Indoor Air Quality</span></div>
    <div class="iaq-num"><span id="iaqVal">--</span><small>/ 100</small></div>
    <div class="iaq-state">State: <span id="iaqState" style="color:var(--muted);font-weight:600">Waiting</span></div>
    <div class="iaq-track"><div class="iaq-fill" id="iaqFill" style="width:0%"></div></div>
  </div>

  <div class="card metric">
    <div class="metric-head"><div class="metric-icon rose"><svg viewBox="0 0 24 24"><path d="M14 14.76V3.5a2.5 2.5 0 0 0-5 0v11.26a4.5 4.5 0 1 0 5 0z"/></svg></div><div class="metric-label">Temp</div></div>
    <div class="metric-value"><span id="tempVal">--</span><span class="unit">°C</span></div>
  </div>
  <div class="card metric">
    <div class="metric-head"><div class="metric-icon blue"><svg viewBox="0 0 24 24"><path d="M12 2.69l5.66 5.66a8 8 0 1 1-11.31 0z"/></svg></div><div class="metric-label">Humidity</div></div>
    <div class="metric-value"><span id="humVal">--</span><span class="unit">%</span></div>
  </div>
  <div class="card metric">
    <div class="metric-head"><div class="metric-icon slate"><svg viewBox="0 0 24 24"><path d="M12 14l3.5-3.5"/><path d="M20.2 17.5a9 9 0 1 0-16.4 0"/><circle cx="12" cy="14" r="1.5" fill="currentColor" stroke="none"/></svg></div><div class="metric-label">Pressure</div></div>
    <div class="metric-value"><span id="pressVal">--</span><span class="unit">hPa</span></div>
  </div>
  <div class="card metric">
    <div class="metric-head"><div class="metric-icon purple"><svg viewBox="0 0 24 24"><path d="M17.5 19a4.5 4.5 0 0 0 .4-9A6 6 0 0 0 6.1 11 4 4 0 0 0 6.5 19z"/><path d="M9 14h.01M13 14h.01M11 17h.01"/></svg></div><div class="metric-label">eCO₂</div></div>
    <div class="metric-value"><span id="co2Val">--</span><span class="unit">ppm</span></div>
  </div>

  <div class="card metric">
    <div class="metric-head"><div class="metric-icon green"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="1.8" fill="currentColor" stroke="none"/></svg></div><div class="metric-label">PM1.0</div></div>
    <div class="metric-value"><span id="pm1Val">--</span><span class="unit">µg/m³</span></div>
  </div>
  <div class="card metric">
    <div class="metric-head"><div class="metric-icon amber"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="3.2" fill="currentColor" stroke="none"/></svg></div><div class="metric-label">PM2.5</div></div>
    <div class="metric-value"><span id="pm25Val">--</span><span class="unit">µg/m³</span></div>
  </div>
  <div class="card metric">
    <div class="metric-head"><div class="metric-icon orange"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="9"/><circle cx="12" cy="12" r="5" fill="currentColor" stroke="none"/></svg></div><div class="metric-label">PM10</div></div>
    <div class="metric-value"><span id="pm10Val">--</span><span class="unit">µg/m³</span></div>
  </div>
  <div class="card metric">
    <div class="metric-head"><div class="metric-icon teal"><svg viewBox="0 0 24 24"><path d="M2 6c.6.5 1.2 1 2.5 1 2.5 0 2.5-2 5-2 2.6 0 2.4 2 5 2 2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1"/><path d="M2 12c.6.5 1.2 1 2.5 1 2.5 0 2.5-2 5-2 2.6 0 2.4 2 5 2 2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1"/><path d="M2 18c.6.5 1.2 1 2.5 1 2.5 0 2.5-2 5-2 2.6 0 2.4 2 5 2 2.5 0 2.5-2 5-2 1.3 0 1.9.5 2.5 1"/></svg></div><div class="metric-label">VOC</div></div>
    <div class="metric-value"><span id="vocVal">--</span><span class="unit">ppm</span></div>
  </div>

  <div class="card chart-card">
    <div class="chart-head">
      <h3><svg viewBox="0 0 24 24"><path d="M3 3v18h18M7 15l4-4 4 3 5-6"/></svg>PM2.5 trend · last 60 readings</h3>
    </div>
    <canvas id="chart"></canvas>
  </div>

  <div class="card controls">
    <div class="controls-label">Device Controls</div>
    <button class="btn primary" onclick="fetch('/ble/on').then(()=>poll())">Bluetooth ON</button>
    <button class="btn" onclick="fetch('/ble/off').then(()=>poll())">Bluetooth OFF</button>
    <button class="btn danger" onclick="fetch('/buzzer/off').then(()=>poll())">Mute Buzzer</button>
    <button class="btn ghost" onclick="poll()">Refresh</button>
  </div>
</div>
<footer><span>ESP32 Air Quality Monitor</span><span>Live polling <code>/data</code> every 3s</span></footer>
</div>

<div class="toast-container" id="toastContainer"></div>

<div class="drawer-backdrop" id="drawerBackdrop" onclick="closeDrawer()"></div>
<aside class="drawer" id="drawer" role="dialog" aria-modal="true" aria-hidden="true">
  <div class="drawer-header">
    <div class="modal-icon info"><svg viewBox="0 0 24 24"><circle cx="12" cy="12" r="10"/><path d="M12 16v-4M12 8h.01"/></svg></div>
    <div style="flex:1"><div class="modal-title">AQI Categories & Sensors</div><div class="modal-subtitle">How readings are calculated</div></div>
    <button class="modal-close" onclick="closeDrawer()"><svg viewBox="0 0 24 24"><path d="M18 6L6 18M6 6l12 12"/></svg></button>
  </div>
  <div class="drawer-body">
    <p style="margin-bottom:8px"><b style="color:var(--text)">CPCB-style AQI bands (PM2.5)</b></p>
    <div class="aqi-list">
      <div class="aqi-row" style="background:#dcfce7"><span class="swatch" style="background:#16a34a"></span><span class="range">0–50</span><span class="name">Good</span></div>
      <div class="aqi-row" style="background:#ecfccb"><span class="swatch" style="background:#65a30d"></span><span class="range">51–100</span><span class="name">Satisfactory</span></div>
      <div class="aqi-row" style="background:#fef9c3"><span class="swatch" style="background:#ca8a04"></span><span class="range">101–200</span><span class="name">Moderate</span></div>
      <div class="aqi-row" style="background:#ffedd5"><span class="swatch" style="background:#ea580c"></span><span class="range">201–300</span><span class="name">Poor</span></div>
      <div class="aqi-row" style="background:#fee2e2"><span class="swatch" style="background:#dc2626"></span><span class="range">301–400</span><span class="name">Very Poor</span></div>
      <div class="aqi-row" style="background:#fecaca"><span class="swatch" style="background:#991b1b"></span><span class="range">401–500</span><span class="name">Severe</span></div>
    </div>
  </div>
  <div class="drawer-footer"><button class="btn primary" onclick="closeDrawer()">Got it</button></div>
</aside>

<script>
let chartData = [];
function openDrawer(){ document.getElementById('drawer').classList.add('open'); document.getElementById('drawerBackdrop').classList.add('open'); }
function closeDrawer(){ document.getElementById('drawer').classList.remove('open'); document.getElementById('drawerBackdrop').classList.remove('open'); }
function setHeaderStatus(state, label){ document.getElementById('statusPill').className = 'status-pill ' + state; document.getElementById('statusText').textContent = label; }
function setGauge(aqi){
  const arc = document.getElementById('arc');
  arc.style.strokeDashoffset = 528 - (528 * (Math.min(aqi, 500) / 500));
  let color='#16a34a', chip='good', label='Good';
  if (aqi>50) { color='#65a30d'; chip='sat'; label='Satisfactory'; }
  if (aqi>100){ color='#ca8a04'; chip='mod'; label='Moderate'; }
  if (aqi>200){ color='#ea580c'; chip='poor'; label='Poor'; }
  if (aqi>300){ color='#dc2626'; chip='vpoor'; label='Very Poor'; }
  if (aqi>400){ color='#991b1b'; chip='sev'; label='Severe'; }
  arc.style.stroke = color;
  document.getElementById('aqiNum').textContent = aqi;
  document.getElementById('aqiCat').textContent = label;
  document.getElementById('aqiCat').style.color = color;
  const c = document.getElementById('aqiChip'); c.className='hero-chip '+chip; c.textContent=label;
}
function clearGauge(){
  document.getElementById('arc').style.strokeDashoffset = 528;
  document.getElementById('arc').style.stroke = '#94a3b8';
  document.getElementById('aqiNum').textContent = '--';
  document.getElementById('aqiCat').textContent = 'Waiting';
  document.getElementById('aqiCat').style.color = 'var(--text-2)';
  const c = document.getElementById('aqiChip'); c.className='hero-chip'; c.textContent='Waiting';
}
const canvas = document.getElementById('chart');
const ctx = canvas.getContext('2d');
function drawChart(){
  const dpr = window.devicePixelRatio || 1;
  const W = canvas.offsetWidth, H = 180;
  canvas.width = W * dpr; canvas.height = H * dpr;
  ctx.setTransform(dpr,0,0,dpr,0,0);
  ctx.clearRect(0,0,W,H);
  ctx.strokeStyle = '#eef2f7'; ctx.lineWidth = 1;
  for (let i=0;i<=4;i++){ const y=10+((H-20)/4)*i; ctx.beginPath(); ctx.moveTo(0,y); ctx.lineTo(W,y); ctx.stroke(); }
  if (chartData.length < 2) {
    ctx.fillStyle = '#94a3b8'; ctx.font = '13px sans-serif'; ctx.textAlign = 'center';
    ctx.fillText('Waiting for PM2.5 data…', W/2, H/2); return;
  }
  const max=200,min=0,padTop=10,padBot=10;
  const plotH=H-padTop-padBot;
  const yOf=v=>padTop+plotH-((Math.min(v,max)-min)/(max-min))*plotH;
  const xStep=W/(chartData.length-1);
  ctx.beginPath(); ctx.moveTo(0,yOf(chartData[0]));
  chartData.forEach((v,i)=>ctx.lineTo(i*xStep,yOf(v)));
  ctx.strokeStyle='#0284c7'; ctx.lineWidth=2.5; ctx.stroke();
}
function updateDashboard(d){
  if (d.network === 'Wi-Fi connected') setHeaderStatus('', 'Live · Wi-Fi');
  else setHeaderStatus('reconnecting', 'Hotspot mode');

  if (d.aqi >= 0) setGauge(d.aqi); else clearGauge();
  document.getElementById('pm25').textContent = d.aqi >= 0 ? d.pm25.toFixed(1) : '--';
  document.getElementById('pm10').textContent = d.aqi >= 0 ? d.pm10.toFixed(1) : '--';

  if (d.iaqValue >= 0) {
    document.getElementById('iaqVal').textContent = d.iaqValue;
    document.getElementById('iaqFill').style.width = d.iaqValue + '%';
    document.getElementById('iaqState').textContent = d.iaqState;
    document.getElementById('iaqState').style.color = d.iaqValue<=25 ? 'var(--good)' : d.iaqValue<=50 ? 'var(--mod)' : d.iaqValue<=75 ? 'var(--poor)' : 'var(--vpoor)';
  } else {
    document.getElementById('iaqVal').textContent = '--';
    document.getElementById('iaqFill').style.width = '0%';
    document.getElementById('iaqState').textContent = 'Waiting';
  }

  document.getElementById('tempVal').textContent = d.temperature.toFixed(1);
  document.getElementById('humVal').textContent = d.humidity.toFixed(1);
  document.getElementById('pressVal').textContent = d.pressure.toFixed(1);
  document.getElementById('co2Val').textContent = d.estCO2 >= 0 ? Math.round(d.estCO2) : '--';
  document.getElementById('pm1Val').textContent = d.pm1.toFixed(1);
  document.getElementById('pm25Val').textContent = d.pm25.toFixed(1);
  document.getElementById('pm10Val').textContent = d.pm10.toFixed(1);
  document.getElementById('vocVal').textContent = d.estVOC >= 0 ? d.estVOC.toFixed(2) : '--';

  if (d.aqi >= 0) { chartData.push(d.pm25); if (chartData.length > 60) chartData.shift(); drawChart(); }
}
async function poll(){
  try{
    const res = await fetch('/data', {cache:'no-store'});
    if (!res.ok) throw new Error('HTTP');
    updateDashboard(await res.json());
  }catch(e){
    setHeaderStatus('offline', 'Connection lost');
  }
}
setTimeout(poll, 200);
setInterval(poll, 3000);
</script>
</body>
</html>
)rawliteral";

void handleRoot() {
  // send_P streams the HTML directly from Flash memory. 
  // This prevents the 45KB string from crashing the ESP32 RAM.
  server.sendHeader("Cache-Control", "no-cache, no-store, must-revalidate");
  server.send_P(200, "text/html", DASHBOARD_HTML);
}

void handleBleOn() {
  startBluetooth();
  server.send(200, "text/plain", "Bluetooth enabled.");
}

void handleBleOff() {
  stopBluetooth();
  server.send(200, "text/plain", "Bluetooth disabled.");
}

void handleBuzzerOff() {
  buzzerManualRequest = false;
  updateOutputs();
  server.send(200, "text/plain", "Buzzer disabled.");
}

void handleData() {
  updateAQI();
  updateIAQ();

  String json = "{";
  json += "\"pm1\":" + String(pm1, 1) + ",";
  json += "\"pm25\":" + String(pm25, 1) + ",";
  json += "\"pm10\":" + String(pm10, 1) + ",";
  json += "\"temperature\":" + String(tempC, 1) + ",";
  json += "\"humidity\":" + String(humidity, 1) + ",";
  json += "\"pressure\":" + String(pressureHPa, 1) + ",";
  json += "\"gas\":" + String(gasKOhm, 1) + ",";
  json += "\"estCO2\":" + String(estimatedCO2ppm, 1) + ",";
  json += "\"estVOC\":" + String(estimatedVOCppm, 3) + ",";
  json += "\"bsecIAQ\":" + String(bsecIAQ, 1) + ",";
  json += "\"bsecAccuracy\":" + String(bsecIAQAccuracy) + ",";
  json += "\"aqi\":" + String(aqi.value) + ",";
  json += "\"aqiStatus\":\"" + String(aqi.status) + "\",";
  json += "\"iaqValue\":" + String(iaq.value) + ",";
  json += "\"iaqState\":\"" + String(iaq.state) + "\",";
  json += "\"status\":\"" + airStatus + "\",";
  json += "\"bt\":" + String(btEnabled ? "true" : "false") + ",";
  json += "\"btConnected\":" + String(btConnected ? "true" : "false") + ",";
  json += "\"buzzer\":" + String(buzzerOutput ? "true" : "false") + ",";
  json += "\"network\":\"";
  json += (WiFi.status() == WL_CONNECTED) ? "Wi-Fi connected" : "Hotspot mode";
  json += "\"}";

  server.send(200, "application/json", json);
}

// =============== WIFI AND HOTSPOT ===============

bool hotspotStarted = false;

void startHotspot() {
  if (hotspotStarted) return;
  WiFi.mode(WIFI_AP_STA);
  bool ok = WiFi.softAP(AP_NAME, AP_PASSWORD);
  if (ok) hotspotStarted = true;
}

void connectWiFiOrStartHotspot() {
  WiFi.mode(WIFI_AP_STA);
  startHotspot();
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  unsigned long started = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - started < 15000) {
    delay(300);
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("Home Wi-Fi dashboard: http://");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("Home Wi-Fi unavailable; use ESP32 hotspot.");
  }
}

// =============== THINGSPEAK ===============

void uploadThingSpeak() {
  if (WiFi.status() != WL_CONNECTED) return;
  updateAQI();
  updateIAQ();

  ThingSpeak.setField(1, pmsReady ? pm1 : 0);
  ThingSpeak.setField(2, pmsReady ? pm25 : 0);
  ThingSpeak.setField(3, pmsReady ? pm10 : 0);
  ThingSpeak.setField(4, bmeReady ? tempC : 0);
  ThingSpeak.setField(5, bmeReady ? humidity : 0);
  ThingSpeak.setField(6, bmeReady ? pressureHPa : 0);
  ThingSpeak.setField(7, bmeReady ? gasKOhm : 0);

  int statusCode = 0;
  if (airStatus == "Good") statusCode = 1;
  else if (airStatus == "Moderate") statusCode = 2;
  else if (airStatus == "Poor") statusCode = 3;
  else if (airStatus == "Unhealthy") statusCode = 4;

  ThingSpeak.setField(8, statusCode);
  ThingSpeak.setField(9, aqi.value);
  ThingSpeak.setField(10, iaq.value);

  ThingSpeak.writeFields(TS_CHANNEL_ID, TS_WRITE_KEY);
}

// =============== SETUP ===============

void setup() {
  Serial.begin(115200);
  pmsSerial.begin(9600, SERIAL_8N1, PMS_RX_PIN, PMS_TX_PIN);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(GREEN_LED_PIN, OUTPUT);
  pinMode(YELLOW_LED_PIN, OUTPUT);
  pinMode(RED_LED_PIN, OUTPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(GREEN_LED_PIN, LOW);
  digitalWrite(YELLOW_LED_PIN, LOW);
  digitalWrite(RED_LED_PIN, LOW);

  Wire.begin(21, 22);

  SPI.begin(TFT_SCK_PIN, TFT_MISO_PIN, TFT_MOSI_PIN, TFT_CS_PIN);
  tft.begin();
  tft.setRotation(1);
  tft.fillScreen(ILI9341_BLACK);
  tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
  tft.setTextSize(2);
  tft.setCursor(10, 10);
  tft.println("Starting air monitor...");
  tftReady = true;

  bmeReady = airSensor.begin(BME68X_I2C_ADDR_LOW, Wire);
  if (!bmeReady) bmeReady = airSensor.begin(BME68X_I2C_ADDR_HIGH, Wire);

  if (bmeReady) {
    bsecSensor sensorList[] = {
      BSEC_OUTPUT_IAQ, BSEC_OUTPUT_CO2_EQUIVALENT, BSEC_OUTPUT_BREATH_VOC_EQUIVALENT,
      BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_TEMPERATURE, BSEC_OUTPUT_SENSOR_HEAT_COMPENSATED_HUMIDITY,
      BSEC_OUTPUT_RAW_PRESSURE, BSEC_OUTPUT_RAW_GAS
    };
    if (airSensor.updateSubscription(sensorList, sizeof(sensorList) / sizeof(sensorList[0]), BSEC_SAMPLE_RATE_LP)) {
      airSensor.attachCallback(bsecNewData);
    } else bmeReady = false;
  }

  connectWiFiOrStartHotspot();

  ThingSpeak.begin(tsClient);
  telegramClient.setInsecure();
  telegramBot = new UniversalTelegramBot(TELEGRAM_TOKEN, telegramClient);

  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/ble/on", handleBleOn);
  server.on("/ble/off", handleBleOff);
  server.on("/buzzer/off", handleBuzzerOff);
  server.begin();

  startBluetooth();
  Serial.println("Monitor started.");
}

// =============== MAIN LOOP ===============

void loop() {
  readPMS();

  if (millis() - lastBmeRead >= BME_INTERVAL) {
    lastBmeRead = millis();
    readBME();
  }

  updateOutputs();
  checkAQICategoryAlert();
  checkBluetoothCommands();

  if (millis() - lastDisplayUpdate >= DISPLAY_INTERVAL) {
    lastDisplayUpdate = millis();
    if (tftReady) {
      tft.fillScreen(ILI9341_BLACK);
      tft.setTextColor(ILI9341_CYAN, ILI9341_BLACK);
      tft.setTextSize(2);
      tft.setCursor(8, 6);
      tft.println("ESP32 AIR QUALITY MONITOR");
      tft.drawFastHLine(8, 30, 304, ILI9341_DARKGREY);

      tft.setTextColor(ILI9341_YELLOW, ILI9341_BLACK);
      tft.setCursor(8, 38);
      tft.print("AQI: ");
      if (aqi.value >= 0) tft.print(aqi.value); else tft.print("--");
      tft.print("  ");
      tft.println(aqi.status);

      tft.setTextColor(ILI9341_WHITE, ILI9341_BLACK);
      tft.setCursor(8, 68);
      tft.print("PM1.0 : ");
      if (pmsReady) tft.print(pm1, 1); else tft.print("--");
      tft.println(" ug/m3");
      tft.setCursor(8, 91);
      tft.print("PM2.5 : ");
      if (pmsReady) tft.print(pm25, 1); else tft.print("--");
      tft.println(" ug/m3");
      tft.setCursor(8, 114);
      tft.print("PM10  : ");
      if (pmsReady) tft.print(pm10, 1); else tft.print("--");
      tft.println(" ug/m3");

      tft.drawFastHLine(8, 140, 304, ILI9341_DARKGREY);
      tft.setCursor(8, 148);
      tft.print("Temp: ");
      if (bmeReady) tft.print(tempC, 1); else tft.print("--");
      tft.print(" C   Hum: ");
      if (bmeReady) tft.print(humidity, 0); else tft.print("--");
      tft.println("%");

      tft.setCursor(8, 174);
      tft.print("Pressure: ");
      if (bmeReady) tft.print(pressureHPa, 1); else tft.print("--");
      tft.println(" hPa");

      tft.setCursor(8, 198);
      tft.print("Gas: ");
      if (bmeReady) tft.print(gasKOhm, 1); else tft.print("--");
      tft.print(" kOhm  IAQ: ");
      if (iaq.value >= 0) tft.print(iaq.value); else tft.print("--");
    }
  }

  if (millis() - lastTsUpload >= TS_INTERVAL) {
    lastTsUpload = millis();
    uploadThingSpeak();
  }

  if (millis() - lastTelegramPoll >= TELEGRAM_INTERVAL) {
    lastTelegramPoll = millis();
    checkTelegram();
  }

  if (WiFi.status() != WL_CONNECTED && millis() - lastWifiRetry >= WIFI_RETRY_INTERVAL) {
    lastWifiRetry = millis();
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  }

  server.handleClient();
  delay(5);
}