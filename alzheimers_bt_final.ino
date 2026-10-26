// ================================================================
//   ALZHEIMER'S IoT MONITORING SYSTEM — ALL STAGES COMBINED
//   Platform: ESP32 | Based on alz.org Alzheimer's Stages
//
//   FIXES IN THIS VERSION:
//   - MPU6050 library replaced with direct I2C read (GY-521 clone fix)
//   - Watchdog crash fixed — delay() replaced with wdt-safe loops
//
//   HOW TO USE (CAREGIVER):
//   1. Upload code to ESP32
//   2. On Android phone: Settings → Bluetooth → pair with "AlzMonitor"
//   3. Open "Serial Bluetooth Terminal" app → connect to AlzMonitor
//   4. Open Serial Monitor at 115200 baud
//   5. Type stage number and press Enter:
//        1  →  EARLY STAGE   (Medication reminders only)
//        2  →  MIDDLE STAGE  (GPS + Geofence + SOS)
//        3  →  LATE STAGE    (Fall detection + Auto alerts)
//   6. All alerts appear on both Serial Monitor AND phone Bluetooth app
//
//   POWER CONNECTIONS:
//   18650 (+) → TP4056 B+
//   18650 (–) → TP4056 B–
//   TP4056 OUT+ → ESP32 VIN
//   TP4056 OUT– → Common GND rail
// ================================================================


// ================================================================
//   LIBRARIES  (MPU6050 library removed — using direct I2C now)
// ================================================================
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPS++.h>
#include <HardwareSerial.h>
#include <TimeLib.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <BluetoothSerial.h>
#include <esp_task_wdt.h>


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
#define MPU_ADDR  0x68   // MPU6050 I2C address


// ================================================================
//   CAREGIVER SETTINGS — EDIT THESE BEFORE UPLOADING
// ================================================================
float homeLat    = 12.8948;
float homeLon    = 77.6762;
float safeRadius = 200.0;     // Safe zone in metres

// Medication times — 24-hour format
int medHour[]   = {16, 14, 20};
int medMinute[] = {30,  0,  0};
int medCount    = 3;

// Fall detection thresholds
float FALL_THRESHOLD = 2.5;   // G-force to detect impact
float REST_THRESHOLD = 0.3;   // G-force below this = still lying down

// Battery warning threshold
float BATT_WARN_VOLTAGE = 3.75;

// Bluetooth device name
String BT_DEVICE_NAME = "AlzMonitor";


// ================================================================
//   GLOBAL OBJECTS
// ================================================================
Adafruit_SSD1306  display(128, 64, &Wire, -1);
TinyGPSPlus       gps;
HardwareSerial    gpsSerial(1);
OneWire           oneWire(TEMP_PIN);
DallasTemperature tempSensor(&oneWire);
BluetoothSerial   BTSerial;


// ================================================================
//   GLOBAL STATE
// ================================================================
int           currentStage    = 0;
bool          reminderSent    = false;
bool          fallDetected    = false;
bool          geofenceAlerted = false;
bool          sosArmed        = true;
bool          battWarnSent    = false;
bool          btConnected     = false;
bool          mpuOK           = false;
unsigned long fallTime        = 0;
unsigned long lastBattCheck   = 0;
float         lastTemperature = 0.0;


// ================================================================
//   MPU6050 DIRECT I2C FUNCTIONS (replaces library)
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

void mpuInit() {
  Wire.setClock(100000);
  delay(500);

  // Wake up MPU6050 — it boots in sleep mode
  mpuWrite(0x6B, 0x00);   // PWR_MGMT_1 = 0 → wake up
  delay(200);
  mpuWrite(0x1C, 0x00);   // ACCEL_CONFIG = 0 → ±2g range
  delay(100);

  // Read WHO_AM_I register — clones return 0x70, real returns 0x68
  Wire.beginTransmission(MPU_ADDR);
  Wire.write(0x75);
  Wire.endTransmission(false);
  Wire.requestFrom(MPU_ADDR, 1, true);
  byte whoami = Wire.read();

  Serial.print("[MPU] WHO_AM_I = 0x"); Serial.println(whoami, HEX);

  if (whoami == 0x68 || whoami == 0x70) {
    mpuOK = true;
    Serial.println("[OK] MPU6050 ready (direct I2C)");
  } else {
    mpuOK = false;
    Serial.println("[WARNING] MPU6050 not responding");
  }
}

void mpuGetAccel(float &gX, float &gY, float &gZ) {
  int16_t ax = mpuRead16(0x3B);
  int16_t ay = mpuRead16(0x3D);
  int16_t az = mpuRead16(0x3F);
  gX = ax / 16384.0;
  gY = ay / 16384.0;
  gZ = az / 16384.0;
}


// ================================================================
//   WDT-SAFE DELAY  (replaces plain delay() in stage loops)
// ================================================================
void safeDelay(int ms) {
  int steps = ms / 100;
  for (int i = 0; i < steps; i++) {
    esp_task_wdt_reset();
    delay(100);
  }
}


// ================================================================
//   SETUP
// ================================================================
void setup() {
  Serial.begin(115200);

  // Watchdog — 30 second timeout
  esp_task_wdt_config_t wdt_config = {
    .timeout_ms      = 30000,
    .idle_core_mask  = (1 << portNUM_PROCESSORS) - 1,
    .trigger_panic   = true
  };
  esp_task_wdt_init(&wdt_config);
  esp_task_wdt_add(NULL);

  Wire.begin(21, 22);

  gpsSerial.begin(4800 , SERIAL_8N1, GPS_RX, GPS_TX);

  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN,    OUTPUT);
  pinMode(SOS_BTN,    INPUT_PULLUP);
  pinMode(BATT_PIN,   INPUT);

  digitalWrite(BUZZER_PIN, LOW);
  digitalWrite(LED_PIN,    LOW);

  // OLED
  if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[ERROR] OLED failed — check GPIO 21 and GPIO 22");
    for (int i = 0; i < 10; i++) {
      digitalWrite(LED_PIN, HIGH); delay(100);
      digitalWrite(LED_PIN, LOW);  delay(100);
    }
    while (1) { esp_task_wdt_reset(); }
  }
  Serial.println("[OK] OLED initialized");

  // MPU6050 — direct I2C init
  mpuInit();

  // DS18B20
  tempSensor.begin();
  if (tempSensor.getDeviceCount() > 0) {
    Serial.println("[OK] DS18B20 found");
  } else {
    Serial.println("[WARNING] DS18B20 not found — check GPIO 4");
  }

  // Bluetooth
  if (BTSerial.begin(BT_DEVICE_NAME)) {
    Serial.println("[OK] Bluetooth started: " + BT_DEVICE_NAME);
  } else {
    Serial.println("[ERROR] Bluetooth failed");
  }

  // Set time — EDIT to current time before uploading
  // setTime(HH, MM, SS, DD, MM, YYYY)
  setTime(16, 29, 0, 9, 5, 2026);

  showOLED("ALZHEIMER'S", "MONITOR", "Waiting...");

  Serial.println("========================================");
  Serial.println("  ALZHEIMER'S MONITORING SYSTEM READY  ");
  Serial.println("========================================");
  Serial.println("Bluetooth: Pair phone with " + BT_DEVICE_NAME);
  Serial.println("App: Serial Bluetooth Terminal (Android)");
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

  // Stage selection via Serial Monitor
  if (Serial.available()) {
    char input = Serial.read();
    if      (input == '1') setStage(1);
    else if (input == '2') setStage(2);
    else if (input == '3') setStage(3);
  }

  checkBluetoothStatus();

  if (currentStage == 0) {
    safeDelay(500);
    return;
  }

  // Battery check every 60 seconds
  if (millis() - lastBattCheck > 60000) {
    checkBatteryVoltage();
    lastBattCheck = millis();
  }

  // ── EARLY STAGE ─────────────────────────────────────────────
  if (currentStage == 1) {
    readTemperature();
    checkMedication();
    safeDelay(10000);   // 10 sec — watchdog safe
  }

  // ── MIDDLE STAGE ────────────────────────────────────────────
  else if (currentStage == 2) {
    readTemperature();
    readGPS();
    checkGeofence();
    checkSOSButton();
    checkMedication();
    safeDelay(5000);    // 5 sec — watchdog safe
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
//   BLUETOOTH STATUS CHECK
// ================================================================
void checkBluetoothStatus() {
  bool nowConnected = BTSerial.connected();
  if (nowConnected && !btConnected) {
    btConnected = true;
    Serial.println("[BT] Phone connected via Bluetooth!");
    showOLED("BLUETOOTH", "Phone connected", "Alerts ON");
    safeDelay(1500);
    BTSerial.println("=== AlzMonitor Connected ===");
    BTSerial.println("You will receive all alerts here.");
    String stageNames[] = {"Not set", "EARLY", "MIDDLE", "LATE"};
    BTSerial.println("Current stage: " + stageNames[currentStage]);
  }
  if (!nowConnected && btConnected) {
    btConnected = false;
    Serial.println("[BT] Phone disconnected");
  }
}


// ================================================================
//   SEND BLUETOOTH ALERT
// ================================================================
void sendAlert(String message) {
  Serial.println("[ALERT] " + message);
  if (BTSerial.connected()) {
    BTSerial.println(message);
    Serial.println("[BT] Alert sent to phone");
  } else {
    Serial.println("[BT] Phone not connected — alert not delivered");
    for (int i = 0; i < 4; i++) {
      digitalWrite(LED_PIN, HIGH); delay(100);
      digitalWrite(LED_PIN, LOW);  delay(100);
    }
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
  Serial.print  ("STAGE SET: "); Serial.println(labels[stage]);
  Serial.println("========================================");

  sendAlert("Stage changed to: " + labels[stage]);

  // Two short beeps to confirm
  digitalWrite(BUZZER_PIN, HIGH); delay(200);
  digitalWrite(BUZZER_PIN, LOW);  delay(100);
  digitalWrite(BUZZER_PIN, HIGH); delay(200);
  digitalWrite(BUZZER_PIN, LOW);
}


// ================================================================
//   BATTERY VOLTAGE MONITOR
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
    sendAlert("WARNING: Device battery low (" + String(actualV, 1) + "V). Please charge soon.");
    digitalWrite(BUZZER_PIN, HIGH); delay(1000);
    digitalWrite(BUZZER_PIN, LOW);
    battWarnSent = true;
  }

  if (actualV >= BATT_WARN_VOLTAGE + 0.1) {
    battWarnSent = false;
  }
}


// ================================================================
//   TEMPERATURE READ — DS18B20
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

        String tempStr = (lastTemperature > 0)
                         ? "Temp: " + String(lastTemperature, 1) + "C"
                         : "Time for dose";
        showOLED("TAKE YOUR", "MEDICINE!", tempStr);

        Serial.println("[MED] Reminder at "
                       + String(h) + ":" + (m < 10 ? "0" : "") + String(m));

        if (currentStage >= 2) {
          String msg = "MEDICINE TIME! Patient should take medication now.";
          if (lastTemperature > 0)
            msg += " Temp: " + String(lastTemperature, 1) + "C";
          sendAlert(msg);
        }

        reminderSent = true;
      }
    }
  }

  if (!anyMatch) reminderSent = false;
}


// ================================================================
//   GPS READ
// ================================================================
void readGPS() {
  while (gpsSerial.available() > 0)
    gps.encode(gpsSerial.read());
}


// ================================================================
//   GEOFENCE CHECK
// ================================================================
void checkGeofence() {
  if (!gps.location.isValid()) {
    Serial.println("[GPS] Waiting for fix...");
    showOLED("GPS", "No fix yet...", "Near window?");
    return;
  }

  float lat  = gps.location.lat();
  float lon  = gps.location.lng();
  float dist = gps.distanceBetween(lat, lon, homeLat, homeLon);

  Serial.print("[GEOFENCE] Distance: ");
  Serial.print(dist); Serial.println("m");

  if (dist > safeRadius) {
    if (!geofenceAlerted) {
      String msg = "ALERT: Patient outside safe zone! "
                   "Lat:" + String(lat, 6) + " Lon:" + String(lon, 6)
                   + " Distance:" + String((int)dist) + "m";
      sendAlert(msg);
      geofenceAlerted = true;
    }
    triggerAlert(3);
    showOLED("GEOFENCE!", "OUTSIDE ZONE", "Alert sent!");
  } else {
    if (geofenceAlerted) {
      Serial.println("[GEOFENCE] Patient back inside zone");
      showOLED("GEOFENCE", "Back inside", "Zone OK");
      sendAlert("Patient returned inside safe zone.");
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

      String locStr = gps.location.isValid()
                      ? "Lat:" + String(gps.location.lat(), 6)
                        + " Lon:" + String(gps.location.lng(), 6)
                      : "Location unavailable";

      String msg = "SOS! Patient needs help! " + locStr;
      sendAlert(msg);

      triggerAlert(5);
      showOLED("SOS SENT!", "Help is coming", "Stay calm");
    }
  }
  if (digitalRead(SOS_BTN) == HIGH) sosArmed = true;
}


// ================================================================
//   FALL DETECTION — Direct I2C MPU6050
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
    sendAlert("FALL DETECTED! Impact: " + String(magnitude, 1) + "G — monitoring for 10 seconds...");
  }

  if (fallDetected && (millis() - fallTime > 10000)) {
    mpuGetAccel(gX, gY, gZ);
    magnitude = sqrt(gX*gX + gY*gY + gZ*gZ);
    if (magnitude < REST_THRESHOLD) {
      triggerEmergency();
    } else {
      showOLED("OK", "Person up", "Monitoring...");
      Serial.println("[FALL] False alarm — recovered");
      sendAlert("False alarm — patient recovered after impact.");
    }
    fallDetected = false;
  }
}


// ================================================================
//   EMERGENCY ALERT — Late Stage
// ================================================================
void triggerEmergency() {
  String locStr = gps.location.isValid()
                  ? "Lat:" + String(gps.location.lat(), 6)
                    + " Lon:" + String(gps.location.lng(), 6)
                  : "Location unavailable";

  String msg = "EMERGENCY! Patient has fallen and not moved. " + locStr;

  sendAlert(msg);
  delay(2000);
  sendAlert("EMERGENCY REPEAT: " + msg);

  showOLED("EMERGENCY!", "HELP SENT", "Alerts active");

  for (int i = 0; i < 20; i++) {
    esp_task_wdt_reset();
    digitalWrite(BUZZER_PIN, HIGH); delay(400);
    digitalWrite(BUZZER_PIN, LOW);  delay(200);
    digitalWrite(LED_PIN,    HIGH); delay(200);
    digitalWrite(LED_PIN,    LOW);  delay(200);
  }
}


// ================================================================
//   SERIAL STATUS LOG (Late Stage)
// ================================================================
void logStatus() {
  Serial.print("[LATE] GPS: ");
  if (gps.location.isValid())
    Serial.println(String(gps.location.lat(), 6) + ", "
                   + String(gps.location.lng(), 6));
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
//   OLED DISPLAY — 3 lines
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
