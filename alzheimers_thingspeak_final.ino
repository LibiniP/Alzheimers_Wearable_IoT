// ================================================================
//   ALZHEIMER'S IoT MONITORING SYSTEM — WiFi + ThingSpeak
//   Platform: ESP32
//
//   ThingSpeak Fields:
//   Field 1 → Temperature
//   Field 2 → Fall Event (1 = fall, 0 = normal)
//   Field 3 → GPS Latitude
//   Field 4 → GPS Longitude
//   Field 5 → SOS Alert (1 = pressed, 0 = normal)
//   Field 6 → Geofence Alert (1 = outside, 0 = inside)
//
//   HOW TO USE:
//   1. Upload code to ESP32
//   2. Turn on mobile hotspot: Thrishalya
//   3. Open Serial Monitor at 115200 baud
//   4. Wait for "WiFi Connected" message
//   5. Type stage number + Enter:
//        1 = EARLY   2 = MIDDLE   3 = LATE
//   6. Open ThingSpeak channel to see live data graphs
// ================================================================


// ================================================================
//   LIBRARIES
// ================================================================
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <TimeLib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <esp_task_wdt.h>


// ================================================================
//   WiFi CREDENTIALS
// ================================================================
const char* WIFI_SSID     = "Thrishalya";
const char* WIFI_PASSWORD = "111222330";


// ================================================================
//   THINGSPEAK CREDENTIALS
// ================================================================
const char* TS_API_KEY   = "B6J8UW5K8LO0GYVB";
const long  TS_CHANNEL   = 3377378;
const char* TS_SERVER    = "http://api.thingspeak.com/update";


// ================================================================
//   PIN DEFINITIONS
// ================================================================
#define BUZZER_PIN  23
#define LED_PIN     19
#define SOS_BTN     18
#define TEMP_PIN     4
#define GPS_RX      16
#define GPS_TX      17
#define BATT_PIN    34
#define MPU_ADDR  0x68


// ================================================================
//   CAREGIVER SETTINGS
// ================================================================
float homeLat    = 12.8948;
float homeLon    = 77.6762;
float safeRadius = 200.0;

int medHour[]   = {16, 14, 20};
int medMinute[] = {30,  0,  0};
int medCount    = 3;

float FALL_THRESHOLD = 2.5;
float REST_THRESHOLD = 0.3;
float BATT_WARN_VOLTAGE = 3.75;


// ================================================================
//   GLOBAL OBJECTS
// ================================================================
Adafruit_SSD1306  display(128, 64, &Wire, -1);
TinyGPSPlus       gps;
HardwareSerial    gpsSerial(1);
OneWire           oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);


// ================================================================
//   GLOBAL STATE
// ================================================================
int           currentStage    = 0;
bool          reminderSent    = false;
bool          fallDetected    = false;
bool          geofenceAlerted = false;
bool          sosArmed        = true;
bool          battWarnSent    = false;
bool          mpuOK           = false;
bool          wifiConnected   = false;
unsigned long fallTime        = 0;
unsigned long lastBattCheck   = 0;
unsigned long lastTSUpdate    = 0;
float         lastTemperature = 0.0;
float         lastLat         = 0.0;
float         lastLon         = 0.0;


// ================================================================
//   MPU6050 DIRECT I2C
// ================================================================
void mpuWrite(byte reg, byte val) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.write(val);
  Wire.endTransmission(true);
}

int16_t mpuRead16(byte reg) {
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 2, true);
  return (Wire.read() << 8) | Wire.read();
}

void mpuGetAccel(float &gX, float &gY, float &gZ) {
  int16_t ax = mpuRead16(0x3B);
  int16_t ay = mpuRead16(0x3D);
  int16_t az = mpuRead16(0x3F);
  gX = ax / 16384.0;
  gY = ay / 16384.0;
  gZ = az / 16384.0;
}

void mpuInit() {
  Wire.setClock(100000);
  delay(500);
  mpuWrite(0x6B, 0x00);
  delay(200);
  mpuWrite(0x1C, 0x00);
  delay(100);

  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x75);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 1, true);
  byte whoami = Wire.read();

  if (whoami == 0x68 || whoami == 0x70) {
    mpuOK = true;
    Serial.println("[OK] MPU6050 ready");
  } else {
    mpuOK = false;
    Serial.println("[WARNING] MPU6050 not found");
  }
}


// ================================================================
//   WDT SAFE DELAY
// ================================================================
void safeDelay(int ms) {
  int steps = ms / 100;
  for (int i = 0; i < steps; i++) {
    esp_task_wdt_reset();
    delay(100);
  }
}


// ================================================================
//   WIFI CONNECT
// ================================================================
void connectWiFi() {
  Serial.print("[WiFi] Connecting to ");
  Serial.println(WIFI_SSID);
  showOLED("CONNECTING", "WiFi...", WIFI_SSID);

  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 30) {
    esp_task_wdt_reset();
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    wifiConnected = true;
    Serial.println("\n[WiFi] Connected!");
    Serial.print("[WiFi] IP: ");
    Serial.println(WiFi.localIP());
    showOLED("WiFi OK!", WiFi.localIP().toString(), "ThingSpeak ON");
    safeDelay(2000);
  } else {
    wifiConnected = false;
    Serial.println("\n[WiFi] FAILED — check hotspot is ON");
    showOLED("WiFi FAILED", "Turn hotspot ON", "Retrying...");
    safeDelay(3000);
  }
}


// ================================================================
//   SEND DATA TO THINGSPEAK
//   Field 1: Temperature
//   Field 2: Fall Event
//   Field 3: GPS Latitude
//   Field 4: GPS Longitude
//   Field 5: SOS Alert
//   Field 6: Geofence Alert
// ================================================================
void sendToThingSpeak(float temp, int fallEvent, float lat, float lon,
                      int sosEvent, int geofenceEvent) {

  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[TS] WiFi not connected — skipping upload");
    connectWiFi();
    return;
  }

  // ThingSpeak requires minimum 15 seconds between updates
  if (millis() - lastTSUpdate < 15000) {
    Serial.println("[TS] Too soon — waiting 15 sec between updates");
    return;
  }

  HTTPClient http;
  String url = String(TS_SERVER)
             + "?api_key=" + TS_API_KEY
             + "&field1="  + String(temp,    2)
             + "&field2="  + String(fallEvent)
             + "&field3="  + String(lat,     6)
             + "&field4="  + String(lon,     6)
             + "&field5="  + String(sosEvent)
             + "&field6="  + String(geofenceEvent);

  Serial.println("[TS] Sending: " + url);
  http.begin(url);
  int httpCode = http.GET();

  if (httpCode > 0) {
    String response = http.getString();
    Serial.println("[TS] Response: " + response);
    if (response.toInt() > 0) {
      Serial.println("[TS] Upload SUCCESS — entry: " + response);
      showOLED("ThingSpeak", "Data sent!", "Entry: " + response);
      safeDelay(1000);
    } else {
      Serial.println("[TS] Upload FAILED — check API key");
    }
  } else {
    Serial.println("[TS] HTTP Error: " + String(httpCode));
  }

  http.end();
  lastTSUpdate = millis();
}


// ================================================================
//   SETUP
// ================================================================
void setup() {
  Serial.begin(115200);

  esp_task_wdt_config_t wdt_config = {
    .timeout_ms      = 30000,
    .idle_core_mask  = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic   = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  Wire.begin(21, 22);
  gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(SOS_BTN,    INPUT_PULLUP);
  pinMode(BATT_PIN,   INPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN,    LOW);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] OLED failed");
    while (1) { esp_task_wdt_reset(); }
  }
  Serial.println("[OK] OLED initialized");

  // MPU6050
  mpuInit();

  // DS18B20
  tempSensor.begin();
  if (tempSensor.getDeviceCount() > 0)
    Serial.println("[OK] DS18B20 found");
  else
    Serial.println("[WARNING] DS18B20 not found");

  // WiFi
  connectWiFi();

  // Time
  setTime(16, 29, 0, 9, 5, 2026);

  showOLED("ALZHEIMER'S", "MONITOR", "Waiting...");

  Serial.println("========================================");
  Serial.println("  ALZHEIMER'S MONITORING SYSTEM READY  ");
  Serial.println("========================================");
  Serial.println("ThingSpeak Channel: " + String(TS_CHANNEL));
  Serial.println("----------------------------------------");
  Serial.println("Type stage number + Enter:");
  Serial.println("  1 = EARLY  (Mild — reminders only)");
  Serial.println("  2 = MIDDLE (Moderate — GPS + SOS)");
  Serial.println("  3 = LATE   (Severe — fall detection)");
  Serial.println("----------------------------------------");
}


// ================================================================
//   MAIN LOOP
// ================================================================
void loop() {
  esp_task_wdt_reset();

  if (Serial.available()) {
    char input = Serial.read();
    if      (input == '1') setStage(1);
    else if (input == '2') setStage(2);
    else if (input == '3') setStage(3);
  }

  if (currentStage == 0) {
    safeDelay(500);
    return;
  }

  // Battery check every 60 seconds
  if (millis() - lastBattCheck > 60000) {
    checkBatteryVoltage();
    lastBattCheck = millis();
  }

  // WiFi reconnect if dropped
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("[WiFi] Disconnected — reconnecting...");
    connectWiFi();
  }

  // ── EARLY STAGE ─────────────────────────────────────────────
  if (currentStage == 1) {
    readTemperature();
    checkMedication();
    // Send temp to ThingSpeak every 15 seconds
    sendToThingSpeak(lastTemperature, 0, 0, 0, 0, 0);
    safeDelay(10000);
  }

  // ── MIDDLE STAGE ────────────────────────────────────────────
  else if (currentStage == 2) {
    readTemperature();
    readGPS();
    checkGeofence();
    checkSOSButton();
    checkMedication();
    safeDelay(5000);
  }

  // ── LATE STAGE ──────────────────────────────────────────────
  else if (currentStage == 3) {
    readGPS();
    detectFall();
    logStatus();
    esp_task_wdt_reset();
    delay(100);
  }
}


// ================================================================
//   STAGE SELECTION
// ================================================================
void setStage(int stage) {
  currentStage    = stage;
  reminderSent    = false;
  fallDetected    = false;
  geofenceAlerted = false;
  sosArmed        = true;

  String labels[]  = {"", "EARLY STAGE",  "MIDDLE STAGE", "LATE STAGE"};
  String details[] = {"", "Mild Mode",    "Moderate Mode","Severe Mode"};
  String extras[]  = {"", "Reminders ON", "GPS+SOS ON",   "Fall Detect ON"};

  showOLED(labels[stage], details[stage], extras[stage]);

  Serial.println("========================================");
  Serial.print("STAGE SET: "); Serial.println(labels[stage]);
  Serial.println("========================================");

  // Notify ThingSpeak of stage change
  sendToThingSpeak(lastTemperature, 0, lastLat, lastLon, 0, 0);

  // Two short beeps
  digitalWrite(BUZZER_PIN, HIGH); delay(200);
  digitalWrite(BUZZER_PIN, LOW);  delay(100);
  digitalWrite(BUZZER_PIN, HIGH); delay(200);
  digitalWrite(BUZZER_PIN, LOW);
}


// ================================================================
//   BATTERY VOLTAGE
// ================================================================
void checkBatteryVoltage() {
  int raw = analogRead(BATT_PIN);
  if (raw < 100) return;

  float adcV    = raw * (3.3 / 4095.0);
  float actualV = adcV * ((10000.0 + 3300.0) / 3300.0);

  Serial.print("[BATT] Voltage: ");
  Serial.print(actualV, 2);
  Serial.println("V");

  if (actualV < BATT_WARN_VOLTAGE && !battWarnSent) {
    Serial.println("[WARNING] Battery low!");
    showOLED("LOW BATTERY", "Charge now!", String(actualV, 1) + "V");
    triggerAlert(2);
    battWarnSent = true;
  }
  if (actualV >= BATT_WARN_VOLTAGE + 0.1) battWarnSent = false;
}


// ================================================================
//   TEMPERATURE
// ================================================================
void readTemperature() {
  tempSensor.requestTemperatures();
  float temp = tempSensor.getTempCByIndex(0);
  if (temp != DEVICE_DISCONNECTED_C) {
    lastTemperature = temp;
    Serial.print("[TEMP] "); Serial.print(temp); Serial.println(" C");
  }
}


// ================================================================
//   MEDICATION REMINDER
// ================================================================
void checkMedication() {
  int h = hour();
  int m = minute();
  bool anyMatch = false;

  for (int i = 0; i < medCount; i++) {
    if (h == medHour[i] && m == medMinute[i]) {
      anyMatch = true;
      if (!reminderSent) {
        triggerAlert(5);
        showOLED("TAKE YOUR", "MEDICINE!", "Temp: " + String(lastTemperature, 1) + "C");
        Serial.println("[MED] Reminder at " + String(h) + ":" + (m < 10 ? "0" : "") + String(m));

        // Send medicine event to ThingSpeak (field2 = 2 means medicine)
        sendToThingSpeak(lastTemperature, 2, lastLat, lastLon, 0, 0);
        reminderSent = true;
      }
    }
  }
  if (!anyMatch) reminderSent = false;
}


// ================================================================
//   GPS
// ================================================================
void readGPS() {
  while (gpsSerial.available() > 0)
    gps.encode(gpsSerial.read());

  if (gps.location.isValid()) {
    lastLat = gps.location.lat();
    lastLon = gps.location.lng();
  }
}


// ================================================================
//   GEOFENCE
// ================================================================
void checkGeofence() {
  if (!gps.location.isValid()) {
    Serial.println("[GPS] Waiting for fix...");
    showOLED("GPS", "No fix yet...", "Near window?");
    return;
  }

  float dist = gps.distanceBetween(lastLat, lastLon, homeLat, homeLon);
  Serial.print("[GEOFENCE] Distance: "); Serial.print(dist); Serial.println("m");

  if (dist > safeRadius) {
    if (!geofenceAlerted) {
      Serial.println("[ALERT] Patient outside safe zone!");
      showOLED("GEOFENCE!", "OUTSIDE ZONE", "Alert sent!");
      triggerAlert(3);
      // Send geofence alert to ThingSpeak — field6 = 1
      sendToThingSpeak(lastTemperature, 0, lastLat, lastLon, 0, 1);
      geofenceAlerted = true;
    }
  } else {
    if (geofenceAlerted) {
      Serial.println("[GEOFENCE] Patient back inside zone");
      showOLED("GEOFENCE", "Back inside", "Zone OK");
      sendToThingSpeak(lastTemperature, 0, lastLat, lastLon, 0, 0);
    }
    geofenceAlerted = false;
  }
}


// ================================================================
//   SOS BUTTON
// ================================================================
void checkSOSButton() {
  if (digitalRead(SOS_BTN) == LOW && sosArmed) {
    delay(50);
    if (digitalRead(SOS_BTN) == LOW) {
      sosArmed = false;
      Serial.println("[ALERT] SOS! Patient needs help!");
      showOLED("SOS SENT!", "Help is coming", "Stay calm");
      triggerAlert(5);
      // Send SOS to ThingSpeak — field5 = 1
      sendToThingSpeak(lastTemperature, 0, lastLat, lastLon, 1, 0);
    }
  }
  if (digitalRead(SOS_BTN) == HIGH) sosArmed = true;
}


// ================================================================
//   FALL DETECTION
// ================================================================
void detectFall() {
  if (!mpuOK) return;

  float gX, gY, gZ;
  mpuGetAccel(gX, gY, gZ);
  float magnitude = sqrt(gX*gX + gY*gY + gZ*gZ);

  if (magnitude > FALL_THRESHOLD && !fallDetected) {
    fallDetected = true;
    fallTime     = millis();
    showOLED("FALL?", "Checking...", "10 sec wait");
    Serial.print("[FALL] Impact: "); Serial.print(magnitude); Serial.println("G");
  }

  if (fallDetected && (millis() - fallTime > 10000)) {
    mpuGetAccel(gX, gY, gZ);
    magnitude = sqrt(gX*gX + gY*gY + gZ*gZ);
    if (magnitude < REST_THRESHOLD) {
      triggerEmergency();
    } else {
      showOLED("OK", "Person up", "Monitoring...");
      Serial.println("[FALL] False alarm — recovered");
      sendToThingSpeak(lastTemperature, 0, lastLat, lastLon, 0, 0);
    }
    fallDetected = false;
  }
}


// ================================================================
//   EMERGENCY
// ================================================================
void triggerEmergency() {
  Serial.println("[ALERT] EMERGENCY! Patient has fallen!");
  showOLED("EMERGENCY!", "HELP SENT", "Alerts active");

  // Send fall alert to ThingSpeak — field2 = 1
  sendToThingSpeak(lastTemperature, 1, lastLat, lastLon, 0, 0);

  for (int i = 0; i < 20; i++) {
    esp_task_wdt_reset();
    digitalWrite(BUZZER_PIN, HIGH); delay(400);
    digitalWrite(BUZZER_PIN, LOW);  delay(200);
    digitalWrite(LED_PIN,    HIGH); delay(200);
    digitalWrite(LED_PIN,    LOW);  delay(200);
  }
}


// ================================================================
//   STATUS LOG
// ================================================================
void logStatus() {
  Serial.print("[LATE] GPS: ");
  if (gps.location.isValid())
    Serial.println(String(lastLat, 6) + ", " + String(lastLon, 6));
  else
    Serial.println("Acquiring fix...");
}


// ================================================================
//   TRIGGER ALERT — Buzzer + LED
// ================================================================
void triggerAlert(int times) {
  for (int i = 0; i < times; i++) {
    esp_task_wdt_reset();
    digitalWrite(BUZZER_PIN, HIGH); delay(400);
    digitalWrite(BUZZER_PIN, LOW);  delay(300);
  }
  for (int i = 0; i < times; i++) {
    esp_task_wdt_reset();
    digitalWrite(LED_PIN, HIGH); delay(300);
    digitalWrite(LED_PIN, LOW);  delay(300);
  }
}


// ================================================================
//   OLED DISPLAY
// ================================================================
void showOLED(String l1, String l2, String l3) {
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(2);
  display.setCursor(0, 0);
  display.println(l1);
  display.setTextSize(1);
  display.setCursor(0, 24);
  display.println(l2);
  display.setCursor(0, 40);
  display.println(l3);
  display.display();
}

// ================================================================
//   END OF CODE
// ================================================================
