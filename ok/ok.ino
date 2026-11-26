#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ================= KONFIGURASI WIFI & HIVEMQ =================
const char* ssid          = "AndroidAPB6F1";
const char* password      = "azov5127";

const char* mqtt_server   = "311c949a8e4d44afb405b9efa6f0a4e4.s1.eu.hivemq.cloud";
const int   mqtt_port     = 8883;
const char* mqtt_user     = "otomatisasi_sampling";
const char* mqtt_password = "sampling123";
const char* mqtt_topic    = "otomatisasi_sampling/qc/sampling";

// ================= PIN HARDWARE =================
#define TFT_CS          15
#define TFT_DC          21
#define TFT_RST          4
#define PIN_PROX_TURUN  26  // Output optocoupler sensor proximity (active LOW)
#define PIN_TRIG_MESIN  27  // Sinyal trigger dari mesin via optocoupler (active LOW)
#define PIN_BTN_RESET   33  // Push button reset fisik (active LOW)

// ================= WARNA RGB565 (ST7789) =================
#define WARNA_HITAM   0x0000
#define WARNA_PUTIH   0xFFFF
#define WARNA_HIJAU   0x07E0
#define WARNA_MERAH   0xF800
#define WARNA_BIRU    0x001F
#define WARNA_KUNING  0xFFE0
#define WARNA_ABU     0x7BEF

Adafruit_ST7789 tft = Adafruit_ST7789(TFT_CS, TFT_DC, TFT_RST);

int counterBerhasil = 0;
int counterGagal = 0;
String statusSaatIni = "STANDBY";

// State jendela validasi (Trigger Mesin + Timeout)
bool windowAktif = false;
bool prevTriggerState = HIGH;
unsigned long windowStartTime = 0;
unsigned long resetMessageUntil = 0;

// Timer Non-Blocking (millis)
unsigned long lastMqttReconnectAttempt = 0;
unsigned long lastHeartbeatTime = 0;
unsigned long lastSensorDebounceTime = 0;
unsigned long lastTriggerDebounceTime = 0;
unsigned long lastResetDebounceTime = 0;

const unsigned long MQTT_RECONNECT_INTERVAL = 3000; // Coba koneksi ulang tiap 3 detik
const unsigned long HEARTBEAT_INTERVAL      = 4000; // Kirim heartbeat tiap 4 detik (< 10s timeout server)
const unsigned long VALIDATION_WINDOW_MS    = 3000; // Batas waktu tunggu / timeout 3.0 detik setelah trigger mesin
const unsigned long SENSOR_DEBOUNCE_MS      = 150;  // Filter debounce sensor proximity
const unsigned long TRIGGER_DEBOUNCE_MS     = 150;  // Filter debounce sinyal trigger mesin
const unsigned long BUTTON_DEBOUNCE_MS      = 80;   // Filter debounce tombol reset

WiFiClientSecure espClient;
PubSubClient client(espClient);

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

  if (!client.connected()) {
    unsigned long now = millis();
    if (now - lastMqttReconnectAttempt >= MQTT_RECONNECT_INTERVAL) {
      lastMqttReconnectAttempt = now;
      Serial.print("Menghubungkan ke HiveMQ...");
      String clientId = "ESP32_OTOMATISASI_SAMPLING-" + String(random(0xffff), HEX);

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

void tampilkanLogoOtomatisasiSampling() {
  tft.fillScreen(WARNA_PUTIH);
  tft.drawRect(8, 8, 304, 224, WARNA_BIRU);
  tft.drawRect(10, 10, 300, 220, WARNA_BIRU);

  tft.setTextColor(WARNA_MERAH);
  tft.setTextSize(4);
  tft.setCursor(28, 52);
  tft.print("OTOMATISASI");
  tft.setCursor(64, 92);
  tft.print("SAMPLING");

  tft.fillRect(55, 138, 210, 6, WARNA_BIRU);

  tft.setTextColor(WARNA_HITAM);
  tft.setTextSize(2);
  tft.setCursor(40, 158);
  tft.print("FEED QUALITY CONTROL");

  tft.setTextColor(WARNA_ABU);
  tft.setTextSize(1);
  tft.setCursor(85, 190);
  tft.print("AUTOMATED SAMPLING SYSTEM");
  delay(1500);
}

void tampilkanDashboardLCD() {
  tft.fillScreen(WARNA_HITAM);
  tft.drawRect(0, 0, 320, 240, WARNA_PUTIH);

  tft.fillRect(1, 1, 318, 35, WARNA_BIRU);
  tft.setTextColor(WARNA_PUTIH);
  tft.setTextSize(2);
  tft.setCursor(40, 10);
  tft.println("OTOMATISASI SAMPLING");

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

// ================= SETUP =================
void setup() {
  Serial.begin(115200);

  pinMode(PIN_PROX_TURUN, INPUT_PULLUP);
  pinMode(PIN_TRIG_MESIN, INPUT_PULLUP);
  pinMode(PIN_BTN_RESET, INPUT_PULLUP);

  tft.init(240, 320);
  tft.setSPISpeed(20000000);
  tft.setRotation(1);
  tft.invertDisplay(false);

  tampilkanLogoOtomatisasiSampling();

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

  // Kembalikan teks status bawah setelah pesan reset tampil sebentar (non-blocking)
  if (resetMessageUntil > 0 && now >= resetMessageUntil) {
    resetMessageUntil = 0;
    perbaruiStatusBawah("STANDBY / TUNGGU TRIG", WARNA_ABU);
  }

  // 1. Tombol Reset Fisik (GPIO 33) dengan debounce
  if (digitalRead(PIN_BTN_RESET) == LOW && (now - lastResetDebounceTime >= BUTTON_DEBOUNCE_MS)) {
    lastResetDebounceTime = now;
    counterBerhasil = 0;
    counterGagal = 0;
    windowAktif = false;
    statusSaatIni = "STANDBY";

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
      statusSaatIni = "SAMPLE TERDETEKSI";

      perbaruiAngkaLCD();
      perbaruiStatusBawah("SAMPLE TERDETEKSI: +1", WARNA_HIJAU);
      kirimEventSiklus("SAMPLE TERDETEKSI");
      Serial.println("[OK] Sampel terdeteksi dalam window 3.0s");
    }
    // Waktu tunggu 3.0 detik habis (timeout) tanpa ada deteksi sampel
    else if (now - windowStartTime >= VALIDATION_WINDOW_MS) {
      windowAktif = false;
      counterGagal++;
      statusSaatIni = "GAGAL";

      perbaruiAngkaLCD();
      perbaruiStatusBawah("TIMEOUT 3.0s - GAGAL!", WARNA_MERAH);
      kirimEventSiklus("GAGAL");
      Serial.println("[TIMEOUT] 3.0s habis tanpa deteksi -> Gagal +1");
    }
  }
}