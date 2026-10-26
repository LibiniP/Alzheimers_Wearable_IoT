# AlzGuard: Stage-Adaptive Alzheimer's IoT Monitoring System

> Alzheimer's patients don't need the same care on Day 1 as they do in Year 5, so their monitoring device shouldn't stay the same either.

AlzGuard is an **ESP32-based wearable / bedside monitoring device** that changes *what it monitors* depending on which of the three clinical Alzheimer's stages (Early, Middle, Late) the patient is in. Instead of applying one fixed set of sensors and alerts to every patient, it activates only the features that are clinically relevant at the current stage, and deliberately switches off the ones that stop making sense.

Alerts reach the caregiver over **Bluetooth** (local, instant) and **Wi-Fi + ThingSpeak** (remote dashboard and history).

**Team:** S. Thrishalya, Durga Lasya, P. Libini (Amrita Vishwa Vidyapeetham, Bengaluru)

---

## Table of Contents

1. [Problem Statement](#problem-statement)
2. [Core Idea: Stage-Adaptive Logic](#core-idea-stage-adaptive-logic)
3. [Features](#features)
4. [System Architecture](#system-architecture)
5. [Hardware](#hardware)
6. [Pin Connections](#pin-connections)
7. [Software and Firmware](#software-and-firmware)
8. [ThingSpeak Cloud Setup](#thingspeak-cloud-setup)
9. [Getting Started](#getting-started)
10. [Usage](#usage)
11. [Results](#results)
12. [Limitations](#limitations)
13. [Roadmap and Future Scope](#roadmap-and-future-scope)
14. [Repository Structure](#repository-structure)
15. [References](#references)

---

## Problem Statement

Alzheimer's disease progressively affects memory, behaviour, mobility and communication. Existing monitoring systems have clear gaps:

- **One-size-fits-all monitoring:** the same sensors and alerts for every patient, regardless of disease stage.
- **No adaptive healthcare logic:** devices can't reconfigure as the patient's condition changes.
- **No automatic fall detection:** falls are a leading cause of injury in late-stage patients and often go unnoticed.
- **Limited remote visibility:** doctors and family who aren't physically present can't track safety or trends.
- **High caregiver burden:** near-constant supervision is expensive and exhausting for families.

AlzGuard addresses these with a low-cost, portable, stage-aware device.

---

## Core Idea: Stage-Adaptive Logic

The caregiver (or doctor) sets the current stage on the device by entering `1`, `2` or `3`. Each stage then activates a different feature set, based on the Alzheimer's Association clinical staging.

| Stage | Patient profile | Features active |
|---|---|---|
| **1: Early (Mild)** | Independent but forgetful | Temperature monitoring, medication reminders |
| **2: Middle (Moderate)** | May wander, needs assistance | GPS tracking, geofence alerts, SOS button, medication reminders |
| **3: Late (Severe)** | Loses mobility and communication | Automatic fall detection, emergency alerts, GPS (emergency location) |

### Why features are removed in the Late stage

Most health wearables only add sensors. AlzGuard also **removes** features once they stop being useful:

| Removed in Stage 3 | Reason |
|---|---|
| **SOS button** | The patient can no longer press a button. A manual SOS is useless and could mislead a caregiver into thinking "no SOS = no problem". |
| **Geofence** | The patient can no longer move around or wander unassisted, so the risk geofencing was built for is gone. |
| **Continuous temperature monitoring** | Late-stage patients have round-the-clock human care, so instant, automatic fall detection takes priority. |

The main loop timing is also stage-adaptive: Stage 1 polls every 10 s, Stage 2 every 5 s, and Stage 3 every 100 ms so a fall is caught almost instantly.

---

## Features

- **Stage-adaptive monitoring** across Early, Middle and Late stages
- **Two-stage fall detection:** an impact check followed by a stillness check, which cuts false alarms
- **GPS tracking and geofencing** with a one-time alert on exit and a "safe return" alert on re-entry
- **SOS button** with debouncing, GPS coordinates in the alert, and re-arming on release
- **Medication reminders** at caregiver-configured times (buzzer + LED + OLED message)
- **Temperature monitoring** using a DS18B20
- **Low-battery warning** through a voltage divider on the battery rail
- **Dual-channel communication:** Bluetooth for local alerts, Wi-Fi + ThingSpeak for remote dashboards and history
- **Redundant emergency alerts:** emergency messages are sent twice
- **30-second hardware watchdog** that resets the ESP32 if the firmware ever hangs
- **Direct I2C access to the MPU6050** so cheap GY-521 clone boards work reliably
- **Low cost:** about ₹1,800 (under $20 USD) in off-the-shelf parts

---

## System Architecture

The firmware runs an 11-step continuous pipeline:

1. **Power on:** 18650 Li-ion cell, then TP4056 charger module, then ESP32 VIN
2. **System initialisation:** OLED, MPU6050, DS18B20, GPS, Bluetooth / Wi-Fi, time sync
3. **Stage selection:** the caregiver enters 1, 2 or 3 over Serial; two beeps confirm
4. **Stage-based operation:** the firmware branches into Stage 1, 2 or 3 logic
5. **Sensor data acquisition:** only the sensors relevant to the active stage are read
6. **Data processing and event detection:** fall, SOS, geofence breach, medicine time, low battery
7. **Alert generation:** buzzer, LED, OLED message
8. **Data transmission:** Bluetooth to the caregiver's phone, Wi-Fi to ThingSpeak (at least 15 s between uploads)
9. **Cloud storage and visualisation:** six ThingSpeak fields with live graphs and history
10. **Caregiver output:** phone alert and/or ThingSpeak dashboard
11. **Continuous monitoring:** the loop repeats indefinitely

```
Battery -> TP4056 -> ESP32 -> [Stage 1 | Stage 2 | Stage 3 logic]
                                   |
                  Sensors (temp / IMU / GPS / SOS) -> Event detection
                                   |
              Buzzer + LED + OLED  ->  Bluetooth (phone)  +  Wi-Fi (ThingSpeak)
```

---

## Hardware

| Component | Purpose |
|---|---|
| **ESP32 Dev Board** | Main controller, built-in Wi-Fi and Bluetooth |
| **MPU6050** (GY-521) | 6-axis IMU for fall detection |
| **DS18B20** | 1-Wire digital temperature sensor |
| **NEO-6M GPS** | Location tracking (NMEA over UART) |
| **SSD1306 OLED (128x64)** | On-device status and alert display |
| **Active buzzer** | Audible alerts |
| **Red LED + 220 ohm resistor** | Visual alerts and error indication |
| **Push button** | SOS trigger |
| **TP4056 module** | Li-ion charging with over-discharge protection |
| **18650 Li-ion cell (~2000 mAh)** | Portable power, roughly 6-8 h of continuous Stage 3 operation |
| 4.7 kohm resistor | DS18B20 pull-up |
| 10 kohm + 3.3 kohm resistors | Battery-sense voltage divider |


---

## Pin Connections

| Function | ESP32 GPIO | Connected to |
|---|---|---|
| OLED / MPU6050 SDA | GPIO 21 | Shared I2C bus |
| OLED / MPU6050 SCL | GPIO 22 | Shared I2C bus |
| DS18B20 DATA | GPIO 4 | DATA line with 4.7 kohm pull-up to 3.3 V |
| GPS RX (ESP side) | GPIO 16 | GPS module TX |
| GPS TX (ESP side) | GPIO 17 | GPS module RX |
| SOS button | GPIO 18 | Button leg 1 (leg 2 to GND) |
| LED anode | GPIO 19 | LED+ through 220 ohm resistor |
| Buzzer + | GPIO 23 | Active buzzer positive terminal |
| Battery sense | GPIO 34 | Voltage divider (10 kohm + 3.3 kohm) off the battery rail |
| VIN | 5 V | TP4056 OUT+ |
| GND | GND | TP4056 OUT- (common ground rail) |

**Power notes**

- 18650 (+) goes to TP4056 B+, 18650 (-) goes to TP4056 B-.
- TP4056 OUT+ goes to ESP32 VIN, and OUT- goes to the common GND rail.
- The OLED, MPU6050, GPS and DS18B20 share the 3.3 V rail and the common GND.

---

## Software and Firmware

Built with the **Arduino framework for ESP32**. Two firmware versions run the same stage-adaptive sensor logic and differ only in how they send data.

| File | Transport | Best for |
|---|---|---|
| `alzheimers_bt_final.ino` | Bluetooth (device name `AlzMonitor`) | Caregiver in the same home. Instant, no internet needed, ~10 m range. |
| `alzheimers_thingspeak_final.ino` | Wi-Fi + ThingSpeak | Remote monitoring, live graphs, historical logs. |

A production version would run both together on one board.

### Startup sequence

1. Initialise Serial, watchdog timer, I2C, GPS serial, OLED, MPU6050, DS18B20 and Bluetooth / Wi-Fi.
2. If the OLED fails to initialise, the LED blinks continuously and the device halts (fail-safe).
3. The OLED shows "System Ready".
4. The device waits for the caregiver to select a stage.

### Main loop

```
LOOP FOREVER
  Reset watchdog
  Read Serial input -> set stage (1/2/3)
  Check Bluetooth / Wi-Fi status
  IF no stage selected -> wait
  Check battery voltage (every 60 s)
  IF stage = EARLY   -> read temperature, check medication      (every 10 s)
  IF stage = MIDDLE  -> read GPS, check geofence, check SOS,
                        check medication                         (every 5 s)
  IF stage = LATE    -> read GPS, detect fall, log status        (every 100 ms)
END LOOP
```

### Key functions

| Function | What it does |
|---|---|
| `setStage(n)` | Saves the stage, resets all alert flags, shows stage info on the OLED, sends a "stage changed" alert, gives two confirmation beeps. |
| `checkMedication()` | Compares the current time with caregiver-set medicine times and triggers buzzer, LED and OLED message. Sends a remote alert from Stage 2 onward. Won't re-send within the same minute. |
| `checkGeofence()` | Uses `gps.distanceBetween()` against the stored home coordinate. If distance exceeds the safe radius (200 m by default), it alerts once and sends a "safe return" alert on re-entry. |
| `checkSOSButton()` | Debounced read. On press it captures GPS, sends an emergency alert with coordinates, sounds the buzzer, shows "SOS SENT!", and re-arms on release. |
| `detectFall()` | Reads acceleration directly from the MPU6050 and computes the magnitude. See below. |
| `triggerEmergency()` | Gets GPS, builds the emergency message, **sends it twice**, shows "EMERGENCY! HELP SENT", and runs a buzzer + LED alarm pattern. |
| `checkBatteryVoltage()` | Reads the ADC through the divider on GPIO 34 and sends a single low-battery warning, with hysteresis before re-arming. |
| `safeDelay()` | Delay that resets the watchdog every 100 ms so long waits never trip it. |

### Fall detection (two-stage)

1. **Impact:** total acceleration `sqrt(gX^2 + gY^2 + gZ^2)` exceeds `FALL_THRESHOLD` (2.5 g). A possible fall is flagged and a 10-second timer starts.
2. **Stillness:** after 10 s the acceleration is read again. If it is below `REST_THRESHOLD` (0.3 g), the patient hasn't moved and `triggerEmergency()` fires. If they moved, the event is logged as a false alarm.

This reduces false positives from sitting down hard or putting the device on a table.

### Why direct I2C for the MPU6050?

Some GY-521 clone boards return `0x70` instead of `0x68` from the `WHO_AM_I` register, and standard Arduino MPU6050 libraries reject them. The firmware talks to the sensor through direct register reads and writes (`mpuWrite` / `mpuRead16`), accepts either ID, wakes the sensor manually (it boots in sleep mode via `PWR_MGMT_1`), and sets the accelerometer range itself.

---

## ThingSpeak Cloud Setup

1. Create a free account at [thingspeak.com](https://thingspeak.com) and create a new channel with **6 fields**.
2. Name the fields as in the table below.
3. Copy the channel's **Write API Key**.
4. In `alzheimers_thingspeak_final.ino`, set your Wi-Fi SSID, password and API key.

| Field | Parameter | Meaning |
|---|---|---|
| 1 | Temperature | Body temperature from DS18B20 (deg C) |
| 2 | Fall / Medicine event | 1 = medicine reminder due, 2 = fall detected |
| 3 | GPS latitude | From NEO-6M |
| 4 | GPS longitude | From NEO-6M |
| 5 | SOS alert | 1 = SOS pressed, 0 = normal |
| 6 | Geofence alert | 1 = outside the safe zone, 0 = inside |

ThingSpeak's free tier allows one update every **15 seconds**, which the firmware respects.

> **Security tip:** never commit your real Wi-Fi password or Write API Key. Use placeholders in the repo, or a git-ignored `secrets.h`.

---

## Getting Started

### Prerequisites

- Arduino IDE (or PlatformIO) with the **ESP32 board package** installed
- Libraries (Library Manager):
  - `TinyGPS++`
  - `Adafruit GFX` and `Adafruit SSD1306`
  - `OneWire` and `DallasTemperature`
  - `BluetoothSerial` (included with the ESP32 core)
  - `WiFi` / `HTTPClient` (included with the ESP32 core)
- A phone with a Bluetooth serial app (for example *Serial Bluetooth Terminal*)

### Steps

1. **Wire the hardware** following the [pin table](#pin-connections).
2. **Clone the repo**
   ```bash
   git clone https://github.com/LibiniP/Alzheimers_Wearable_IoT.git
   cd Alzheimers_Wearable_IoT
   ```
3. **Configure the firmware**
   - Set your **home latitude / longitude** and safe radius for the geofence.
   - Set **medicine times** (`medHour[]`, `medMinute[]`).
   - For the ThingSpeak version, set Wi-Fi credentials and the Write API Key.
4. **Select board:** *ESP32 Dev Module* and the correct COM port.
5. **Upload** the `.ino` file for your chosen version.
6. Open the **Serial Monitor** at the baud rate set in the code.

---

## Usage

1. Power the device. The OLED shows **"System Ready"**.
2. Enter the patient's stage in the Serial Monitor (`1`, `2` or `3`). The doctor decides the stage and the caregiver enters it. Two beeps confirm.
3. **Bluetooth version:** pair your phone with **`AlzMonitor`** and open the serial terminal app to receive alerts.
4. **ThingSpeak version:** open your channel dashboard in any browser to see live graphs.
5. Re-enter the stage number whenever the doctor reassesses the patient. Alert flags reset automatically.

---

## Results

All functional modules were validated on the physical prototype.

| Feature | Test | Result |
|---|---|---|
| Stage-based monitoring | Correct sensor activation in all 3 stages | PASS |
| Temperature monitoring | DS18B20 on OLED and cloud | PASS |
| Medication reminder | Buzzer + OLED at preset times | PASS |
| GPS tracking | Lat/long captured and uploaded | PASS |
| Geofence alert | Alert on leaving safe radius | PASS |
| Fall detection | Threshold, 10 s check, emergency | PASS |
| SOS button | Immediate emergency alert | PASS |
| Bluetooth alerts | Received on paired Android phone | PASS |
| Wi-Fi cloud upload | Data pushed every 15 s | PASS |
| ThingSpeak visualisation | All 6 fields with live graphs | PASS |
| Emergency trigger | Buzzer + LED + BT + cloud all fire | PASS |

**Observations**

- GPS cold start: about 60-90 s to a satellite lock outdoors
- Bluetooth range: reliable up to about 8-10 m line of sight
- Cloud latency: graphs update within about 3 s of each upload
- Temperature accuracy: within about +/-0.5 deg C of a reference
- Battery life: about 6-8 h on a 2000 mAh cell in Stage 3 (all sensors active)

---

## Limitations

- **No cellular (GSM) module in the current prototype:** Bluetooth needs the phone within about 10 m, and cloud upload needs Wi-Fi.
- **Indoor GPS is weak:** the NEO-6M degrades significantly indoors.
- **Manual stage selection:** the stage is set through the Serial console by design, since staging is a clinical decision.
- **Fixed fall thresholds:** not yet personalised per patient.
- **Breadboard prototype:** a wearable enclosure and strap are not yet built.

---

## Roadmap and Future Scope

- [ ] **GSM / SMS alerts (SIM800L):** emergency SMS anywhere with cellular coverage, with no smartphone or data plan. GPRS-based cloud sync would need a data-enabled SIM.
- [ ] **Personalised voice alerts** in the Middle stage, using a recorded caregiver or loved one's voice.
- [ ] **Doctor-in-the-loop reassessment flagging:** the device logs fall and wandering counts and, once a sustained trend appears (for example 2+ falls or 4+ wandering incidents over at least 14 days), prints a plain summary recommending a doctor visit. It never suggests a stage itself.
- [ ] Per-patient fall-threshold calibration at setup
- [ ] BLE smartwatch / wearable form factor
- [ ] Dedicated Android / iOS caregiver app with map-based tracking
- [ ] Edge-AI fall classification (TensorFlow Lite) to reduce false positives further
- [ ] Heart-rate / SpO2 sensor
- [ ] BLE beacons or assisted GPS for indoor positioning
- [ ] Voice assistant integration (Google Assistant / Alexa)

---

## Repository Structure

```
Alzheimers_Wearable_IoT/
|-- alzheimers_bt_final.ino           # Bluetooth version
|-- alzheimers_thingspeak_final.ino   # Wi-Fi + ThingSpeak version
|-- README.md
```

---

## References

1. Alzheimer's Association, "Alzheimer's Stages: Early, Middle and Late Dementia Symptoms." https://www.alz.org/alzheimers-dementia/stages
2. K. S. Adewole et al., "IoT-Based Patient Monitoring System," *IEEE Access*, vol. 10, pp. 4623-4641, 2022.
3. A. Espinosa et al., "Caregiver Burden and Alzheimer's Disease Severity," *Int. J. Geriatric Psychiatry*, vol. 37, no. 1, 2022.
4. Espressif Systems, *ESP32 Technical Reference Manual*, v5.1, 2023.
5. ThingSpeak Documentation, MathWorks. https://thingspeak.com/docs
6. InvenSense, *MPU-6050 Product Specification*, Rev 3.4, 2013.
7. Maxim Integrated, *DS18B20 Programmable Resolution 1-Wire Digital Thermometer*, 2019.
8. u-blox, *NEO-6 u-blox 6 GPS Modules Data Sheet*, UBX-09005211-R06, 2011.
9. World Health Organization, *Dementia: Key Facts*, 2023. https://www.who.int/news-room/fact-sheets/detail/dementia

---

## Disclaimer

AlzGuard is an academic prototype. It is a **monitoring and alerting aid, not a medical device**, and it does not diagnose or stage Alzheimer's disease. Staging and treatment decisions must always be made by a qualified doctor.

## Contact

**P. Libini** | [GitHub](https://github.com/LibiniP) | [LinkedIn](https://linkedin.com/in/libini-p)
