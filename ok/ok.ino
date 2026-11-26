#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <time.h>

// ================= KONFIGURASI WIFI & HIVEMQ =================
const char* ssid          = "AndroidAPB6F1";
const char* password      = "azov5127";

const char* mqtt_server   = "311c949a8e4d44afb405b9efa6f0a4e4.s1.eu.hivemq.cloud";
const int   mqtt_port     = 8883;
const char* mqtt_user     = "otomatisasi_sampling";
const char* mqtt_password = "sampling123";
const char* mqtt_topic    = "otomatis_sampling/qc/sampling";

// ================= KONFIGURASI WAKTU (WIB / UTC+7) =================
const char* ntpServer          = "pool.ntp.org";
const long  gmtOffset_sec      = 7 * 3600; // WIB = UTC+7 (25200 detik)
const int   daylightOffset_sec = 0;

// ================= PIN HARDWARE =================
#define TFT_CS          15
#define TFT_DC          21
#define TFT_RST          4
#define PIN_PROX_TURUN  26  // Output optocoupler sensor proximity (active LOW)
#define PIN_TRIG_MESIN  27  // Sinyal trigger dari mesin via optocoupler (active LOW)
#define PIN_BTN_RESET   33  // Push button reset fisik (active LOW)
#define PIN_BTN_MUTE    32  // Push button matikan buzzer/alarm saja (active LOW)
#define PIN_RELAY_ALARM 25  // Output ke Relay Lampu Rotator + Buzzer 24V DC

// Logika Relay (kebanyakan modul relay adalah Active LOW)
// Tukar menjadi HIGH dan LOW jika modul relay Anda bertipe Active HIGH
#define RELAY_ON        LOW
#define RELAY_OFF       HIGH

// ================= WARNA RGB565 (ST7789) =================
#define WARNA_HITAM   0x0000
#define WARNA_PUTIH   0xFFFF
#define WARNA_HIJAU   0x07E0
#define WARNA_MERAH   0xF800
#define WARNA_BIRU    0x001F
#define WARNA_KUNING  0xFFE0
#define WARNA_ABU     0x7BEF
#define WARNA_CYAN    0x07FF

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

int counterBerhasil = 0;
int counterGagal = 0;
int gagalBeruntun = 0;
const int BATAS_GAGAL_ALARM = 2; // Sinyal peringatan / alarm nyala jika gagal >= 2 kali beruntun
String statusSaatIni = "STANDBY";

// State Shift (SHIFT 1 / SHIFT 2 / SHIFT 3)
String shiftAktif = "SHIFT 1";
String shiftTerakhir = "";
bool waktuTersinkron = false;

// State jendela validasi (Trigger Mesin + Timeout)
bool windowAktif = false;
bool prevTriggerState = HIGH;
unsigned long windowStartTime = 0;
unsigned long resetMessageUntil = 0;

// Timer Non-Blocking (millis)
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastShiftCheckTime = 0;
unsigned long lastSensorDebounceTime = 0;
unsigned long lastTriggerDebounceTime = 0;
unsigned long lastResetDebounceTime = 0;
unsigned long lastMuteDebounceTime = 0;

const unsigned long MQTT_RECONNECT_INTERVAL = 3000;
const unsigned long HEARTBEAT_INTERVAL      = 4000;
const unsigned long SHIFT_CHECK_INTERVAL    = 2000;
const unsigned long VALIDATION_WINDOW_MS    = 3000;
const unsigned long SENSOR_DEBOUNCE_MS      = 150;
const unsigned long TRIGGER_DEBOUNCE_MS     = 150;
const unsigned long BUTTON_DEBOUNCE_MS      = 80;

WiFiClientSecure espClient;
PubSubClient client(espClient);

// ================= PENENTUAN SHIFT BERDASARKAN JAM WIB =================
String deteksiShiftWIB(int jam) {
  if (jam >= 7 && jam < 15) {
    return "SHIFT 1";
  } else if (jam >= 15 && jam < 23) {
    return "SHIFT 2";
  } else {
    return "SHIFT 3";
  }
}

void sinkronkanWaktuNTP() {
  if (WiFi.status() != WL_CONNECTED) return;

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
  struct tm timeinfo;
  unsigned long startAttempt = millis();
  while (!getLocalTime(&timeinfo) && (millis() - startAttempt < 5000)) {
    delay(200);
  }

  if (getLocalTime(&timeinfo)) {
    waktuTersinkron = true;
    shiftAktif = deteksiShiftWIB(timeinfo.tm_hour);
    shiftTerakhir = shiftAktif;
    Serial.printf("[NTP] Waktu WIB tersinkron: %02d:%02d:%02d | %s\n",
                  timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec, shiftAktif.c_str());
  } else {
    waktuTersinkron = false;
    Serial.println("[NTP] Gagal sinkron waktu saat awal, akan dicoba ulang otomatis.");
  }
}

// ================= KONTROL ALARM LAMPU & BUZZER =================
void setAlarm(bool aktif) {
  digitalWrite(PIN_RELAY_ALARM, aktif ? RELAY_ON : RELAY_OFF);
  Serial.println(aktif ? "[ALARM] Sinyal Peringatan (Lampu & Buzzer 24V) NYALA!" : "[ALARM] Sinyal Peringatan MATI.");
}

// ================= FUNGSI KIRIM HEARTBEAT SAJA (TIAP 4 DETIK) =================
void kirimHeartbeat() {
  if (client.connected()) {
    String jsonPayload = "{\"heartbeat\":true}";
    client.publish(mqtt_topic, jsonPayload.c_str());
    Serial.println("[MQTT HEARTBEAT] " + jsonPayload);
    lastHeartbeatTime = millis();
  }
}

// ================= FUNGSI KIRIM EVENT SIKLUS (HANYA SAAT ADA KEJADIAN) =================
void kirimEventSiklus(String statusTeks) {
  if (client.connected()) {
    String jsonPayload = "{\"berhasil\":" + String(counterBerhasil) +
                         ",\"gagal\":" + String(counterGagal) +
                         ",\"gagal_beruntun\":" + String(gagalBeruntun) +
                         ",\"shift\":\"" + shiftAktif + "\"" +
                         ",\"status\":\"" + statusTeks + "\"" +
                         ",\"heartbeat\":false}";

    client.publish(mqtt_topic, jsonPayload.c_str());
    Serial.println("[MQTT EVENT] " + jsonPayload);
    lastHeartbeatTime = millis();
  }
}

// ================= KONEKSI MQTT NON-BLOCKING =================
void handleMQTT() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  if (!waktuTersinkron) {
    sinkronkanWaktuNTP();
  }

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnectAttempt >= MQTT_RECONNECT_INTERVAL) {
      lastMqttReconnectAttempt = now;
      Serial.print("Menghubungkan ke HiveMQ...");
      String clientId = "ESP32_QC_MONITOR-" + String(random(0xffff), HEX);

      if (client.connect(clientId.c_str(), mqtt_user, mqtt_password)) {
        Serial.println(" [MQTT] Terhubung!");
        lastMqttReconnectAttempt = 0;
        kirimHeartbeat();
      } else {
        Serial.print(" Gagal, rc=");
        Serial.println(client.state());
      }
    }
  } else {
    client.loop();

    if (millis() - lastHeartbeatTime >= HEARTBEAT_INTERVAL) {
      kirimHeartbeat();
    }
  }
}

// ================= FUNGSI TAMPILAN LCD =================
void perbaruiHeaderShiftLCD() {
  tft.fillRect(205, 4, 110, 28, WARNA_BIRU);
  tft.setTextColor(WARNA_KUNING);
  tft.setTextSize(2);
  tft.setCursor(210, 10);
  tft.print(shiftAktif);
}

void perbaruiAngkaLCD() {
  tft.fillRect(18, 85, 129, 95, WARNA_HITAM);
  tft.setTextColor(WARNA_HIJAU);
  tft.setTextSize(6);
  tft.setCursor(45, 105);
  tft.println(counterBerhasil);

  tft.fillRect(173, 85, 129, 95, WARNA_HITAM);
  tft.setTextColor(WARNA_MERAH);
  tft.setTextSize(6);
  tft.setCursor(205, 105);
  tft.println(counterGagal);
}

void perbaruiStatusBawah(String pesan, uint16_t warna) {
  tft.fillRect(10, 195, 300, 38, WARNA_HITAM);
  tft.setTextColor(warna);
  tft.setTextSize(2);
  tft.setCursor(15, 203);
  tft.println(pesan);
}

void tampilkanSplashAwal() {
  tft.fillScreen(WARNA_PUTIH);
  tft.drawRect(8, 8, 304, 224, WARNA_BIRU);
  tft.drawRect(10, 10, 300, 220, WARNA_BIRU);

  tft.setTextColor(WARNA_MERAH);
  tft.setTextSize(5);
  tft.setCursor(55, 85);
  tft.print("QC MONITOR");

  tft.fillRect(55, 140, 210, 6, WARNA_BIRU);

  tft.setTextColor(WARNA_HITAM);
  tft.setTextSize(2);
  tft.setCursor(60, 160);
  tft.print("QUALITY CONTROL");

  tft.setTextColor(WARNA_ABU);
  tft.setTextSize(1);
  tft.setCursor(105, 190);
  tft.print("AUTOMATED SAMPLING SYSTEM");
  delay(1500);
}

void tampilkanDashboardLCD() {
  tft.fillScreen(WARNA_HITAM);
  tft.drawRect(0, 0, 320, 240, WARNA_PUTIH);

  tft.fillRect(1, 1, 318, 35, WARNA_BIRU);
  tft.setTextColor(WARNA_PUTIH);
  tft.setTextSize(2);
  tft.setCursor(12, 10);
  tft.print("QC COUNTER");
  perbaruiHeaderShiftLCD();

  tft.drawFastHLine(0, 36, 320, WARNA_PUTIH);

  // Box Berhasil
  tft.drawRect(15, 45, 135, 140, WARNA_HIJAU);
  tft.fillRect(16, 46, 133, 30, WARNA_HIJAU);
  tft.setTextColor(WARNA_HITAM);
  tft.setTextSize(2);
  tft.setCursor(25, 53);
  tft.println("BERHASIL");

  // Box Gagal
  tft.drawRect(170, 45, 135, 140, WARNA_MERAH);
  tft.fillRect(171, 46, 133, 30, WARNA_MERAH);
  tft.setTextColor(WARNA_PUTIH);
  tft.setTextSize(2);
  tft.setCursor(200, 53);
  tft.println("GAGAL");

  perbaruiAngkaLCD();
  perbaruiStatusBawah("STANDBY / TUNGGU TRIG", WARNA_ABU);
}

// ================= CEK PERGANTIAN SHIFT & AUTO-RESET COUNTER =================
void handlePergantianShift(unsigned long now) {
  if (now - lastShiftCheckTime < SHIFT_CHECK_INTERVAL) return;
  lastShiftCheckTime = now;

  if (WiFi.status() != WL_CONNECTED) return;

  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) return;

  waktuTersinkron = true;
  String shiftTerdeteksi = deteksiShiftWIB(timeinfo.tm_hour);

  if (shiftTerakhir == "") {
    shiftTerakhir = shiftTerdeteksi;
    shiftAktif = shiftTerdeteksi;
    perbaruiHeaderShiftLCD();
    return;
  }

  if (shiftTerdeteksi != shiftTerakhir) {
    shiftAktif = shiftTerdeteksi;
    shiftTerakhir = shiftTerdeteksi;

    counterBerhasil = 0;
    counterGagal = 0;
    gagalBeruntun = 0;
    windowAktif = false;
    statusSaatIni = "STANDBY";

    setAlarm(false);
    perbaruiHeaderShiftLCD();
    perbaruiAngkaLCD();
    perbaruiStatusBawah("GANTI " + shiftAktif + " - RESET", WARNA_CYAN);
    kirimEventSiklus("AUTO RESET SHIFT");
    resetMessageUntil = now + 2000;
    Serial.println("[SHIFT] Pergantian shift terdeteksi -> " + shiftAktif + " (counter di-reset otomatis)");
  }
}

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_RELAY_ALARM, OUTPUT);
  setAlarm(false);

  pinMode(PIN_PROX_TURUN, INPUT_PULLUP);
  pinMode(PIN_TRIG_MESIN, INPUT_PULLUP);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);
  pinMode(PIN_BTN_MUTE, INPUT_PULLUP);

  tft.init(240, 320);
  tft.setSPISpeed(20000000);
  tft.setRotation(1);
  tft.invertDisplay(false);

  tampilkanSplashAwal();

  tft.fillScreen(WARNA_HITAM);
  tft.setTextColor(WARNA_PUTIH);
  tft.setTextSize(2);
  tft.setCursor(25, 90);
  tft.println("Menghubungkan WiFi...");

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }

  tft.fillScreen(WARNA_HITAM);
  tft.setCursor(25, 90);
  tft.setTextColor(WARNA_HIJAU);
  tft.println("WiFi Terhubung!");
  Serial.println("\nWiFi Terhubung!");

  sinkronkanWaktuNTP();
  delay(800);

  espClient.setInsecure();
  client.setServer(mqtt_server, mqtt_port);

  tampilkanDashboardLCD();
}

// ================= LOOP UTAMA =================
void loop() {
  unsigned long now = millis();

  // Kelola koneksi MQTT & heartbeat berkala
  handleMQTT();

  // Cek pergantian shift otomatis berdasarkan jam WIB & reset counter lokal saat berganti shift
  handlePergantianShift(now);

  // Kembalikan teks status bawah setelah pesan reset/mute/shift tampil sebentar (non-blocking)
  if (resetMessageUntil > 0 && now >= resetMessageUntil) {
    resetMessageUntil = 0;
    perbaruiStatusBawah("STANDBY / TUNGGU TRIG", WARNA_ABU);
  }

  // 1a. Tombol Mute Buzzer Saja (GPIO 32) dengan debounce
  if (digitalRead(PIN_BTN_MUTE) == LOW && (now - lastMuteDebounceTime >= BUTTON_DEBOUNCE_MS)) {
    lastMuteDebounceTime = now;
    gagalBeruntun = 0;
    statusSaatIni = "STANDBY";

    setAlarm(false);
    perbaruiStatusBawah("BUZZER DIMATIKAN!", WARNA_KUNING);
    resetMessageUntil = now + 1000;
    Serial.println("[MUTE] Alarm dimatikan lewat GPIO 32");
  }

  // 1b. Tombol Reset Fisik (GPIO 33) dengan debounce
  if (digitalRead(PIN_BTN_RESET) == LOW && (now - lastResetDebounceTime >= BUTTON_DEBOUNCE_MS)) {
    lastResetDebounceTime = now;
    counterBerhasil = 0;
    counterGagal = 0;
    gagalBeruntun = 0;
    windowAktif = false;
    statusSaatIni = "STANDBY";

    setAlarm(false);
    perbaruiAngkaLCD();
    perbaruiStatusBawah("COUNTER DI-RESET!", WARNA_KUNING);
    kirimEventSiklus("RESET MANUAL");
    resetMessageUntil = now + 1000;
  }

  // 2. Baca Sinyal Trigger Mesin (GPIO 27, edge falling / aktif LOW) dengan debounce
  bool currentTriggerState = digitalRead(PIN_TRIG_MESIN);
  if (prevTriggerState == HIGH && currentTriggerState == LOW) {
    if (now - lastTriggerDebounceTime >= TRIGGER_DEBOUNCE_MS) {
      lastTriggerDebounceTime = now;
      prevTriggerState = LOW;
      if (!windowAktif) {
        windowAktif = true;
        windowStartTime = now;
        statusSaatIni = "VALIDASI 3.0s...";
        perbaruiStatusBawah("TRIGGER! CEK PROX 3.0s", WARNA_KUNING);
        Serial.println("[TRIG] Sinyal mesin masuk -> Jendela validasi 3.0s dibuka");
      }
    }
  } else if (currentTriggerState == HIGH) {
    prevTriggerState = HIGH;
  }

  // 3. Jika jendela validasi aktif: cek sensor proximity (debounce) atau timeout 3.0 detik
  if (windowAktif) {
    bool sensorAktif = (digitalRead(PIN_PROX_TURUN) == LOW);

    // Sensor mendeteksi sampel dalam rentang waktu validasi (dengan debounce)
    if (sensorAktif && (now - lastSensorDebounceTime >= SENSOR_DEBOUNCE_MS)) {
      lastSensorDebounceTime = now;
      windowAktif = false;
      counterBerhasil++;
      gagalBeruntun = 0;
      statusSaatIni = "SAMPLE TERDETEKSI";

      setAlarm(false);
      perbaruiAngkaLCD();
      perbaruiStatusBawah("SAMPLE TERDETEKSI: +1", WARNA_HIJAU);
      kirimEventSiklus("SAMPLE TERDETEKSI");
      Serial.println("[OK] Sampel terdeteksi dalam window 3.0s");
    }
    // Waktu tunggu 3.0 detik habis (timeout) tanpa ada deteksi sampel
    else if (now - windowStartTime >= VALIDATION_WINDOW_MS) {
      windowAktif = false;
      counterGagal++;
      gagalBeruntun++;
      statusSaatIni = "GAGAL";

      if (gagalBeruntun >= BATAS_GAGAL_ALARM) {
        setAlarm(true);
        perbaruiStatusBawah("ALARM! GAGAL >= 2x", WARNA_MERAH);
      } else {
        perbaruiStatusBawah("GAGAL (" + String(gagalBeruntun) + "/2)", WARNA_KUNING);
      }

      perbaruiAngkaLCD();
      kirimEventSiklus("GAGAL");
      Serial.println("[TIMEOUT] 3.0s habis tanpa deteksi -> Gagal +1");
    }
  }
}