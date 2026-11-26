from flask import Flask, render_template, request, jsonify
import paho.mqtt.client as mqtt
import json
import os
import threading
import time
from datetime import datetime, timezone, timedelta

app = Flask(__name__, template_folder='.')

# Zona Waktu WIB (Asia/Jakarta = UTC+7)
WIB_TZ = timezone(timedelta(hours=7))
STATE_CACHE_FILE = os.path.join(
    "/tmp" if os.path.exists("/tmp") else os.path.dirname(os.path.abspath(__file__)),
    "otomatisasi_sampling_state_cache.json"
)

def get_wib_now():
    return datetime.now(WIB_TZ)

def get_current_shift_info(dt=None):
    """
    Menentukan Shift kerja berdasarkan jam WIB (Asia/Jakarta):
    - Shift 1: 07:00 - 15:00 (07:00:00 s/d 14:59:59)
    - Shift 2: 15:00 - 23:00 (15:00:00 s/d 22:59:59)
    - Shift 3: 23:00 - 07:00 (23:00:00 s/d 06:59:59, lintas hari)
    """
    if dt is None:
        dt = get_wib_now()
    hour = dt.hour
    if 7 <= hour < 15:
        return "Shift 1", "07:00 - 15:00 WIB"
    elif 15 <= hour < 23:
        return "Shift 2", "15:00 - 23:00 WIB"
    else:
        return "Shift 3", "23:00 - 07:00 WIB"

def create_empty_shifts():
    return {
        "Shift 1": {"berhasil": 0, "gagal": 0, "jam": "07:00 - 15:00 WIB"},
        "Shift 2": {"berhasil": 0, "gagal": 0, "jam": "15:00 - 23:00 WIB"},
        "Shift 3": {"berhasil": 0, "gagal": 0, "jam": "23:00 - 07:00 WIB"}
    }

_init_shift, _init_jam = get_current_shift_info()

# Variabel Global untuk menyimpan state data dashboard secara real-time
data_state = {
    "berhasil": 0,
    "gagal": 0,
    "gagal_beruntun": 0,
    "status": "STANDBY — MENUNGGU KELIPATAN 100",
    "koneksi_mqtt": False,
    "esp_online": False,
    "shift_aktif": _init_shift,
    "jam_shift_aktif": _init_jam,
    "shifts": create_empty_shifts(),
    "logs": []  # Menyimpan histori aktivitas sampling
}

# Lock untuk memastikan pembaruan state global aman antar-thread (thread-safe)
state_lock = threading.Lock()
MAX_LOGS = 100
BATAS_GAGAL_ALARM = 2

# Timestamp terakhir kali ESP32 mengirim data/pesan
last_esp_activity = 0
esp_uses_heartbeat = False

# Menyimpan nilai counter mentah terakhir dari ESP32 untuk mendeteksi perubahan sinyal sensor nyata
last_raw_berhasil = None
last_raw_gagal = None

def save_state_cache():
    """Simpan state ke file cache lokal/tmp agar tidak mudah hilang antar-request."""
    try:
        cache_payload = {
            "berhasil": data_state["berhasil"],
            "gagal": data_state["gagal"],
            "gagal_beruntun": data_state["gagal_beruntun"],
            "status": data_state["status"],
            "shifts": data_state["shifts"],
            "logs": data_state["logs"][:MAX_LOGS]
        }
        with open(STATE_CACHE_FILE, "w", encoding="utf-8") as f:
            json.dump(cache_payload, f, ensure_ascii=False)
    except Exception:
        pass

def load_state_cache():
    """Muat state dari file cache jika tersedia."""
    try:
        if os.path.exists(STATE_CACHE_FILE):
            with open(STATE_CACHE_FILE, "r", encoding="utf-8") as f:
                cached = json.load(f)
            if isinstance(cached, dict):
                data_state["berhasil"] = int(cached.get("berhasil", 0))
                data_state["gagal"] = int(cached.get("gagal", 0))
                data_state["gagal_beruntun"] = int(cached.get("gagal_beruntun", 0))
                data_state["status"] = str(cached.get("status", data_state["status"]))
                if isinstance(cached.get("shifts"), dict):
                    for s_key in ["Shift 1", "Shift 2", "Shift 3"]:
                        if s_key in cached["shifts"] and isinstance(cached["shifts"][s_key], dict):
                            data_state["shifts"][s_key]["berhasil"] = int(cached["shifts"][s_key].get("berhasil", 0))
                            data_state["shifts"][s_key]["gagal"] = int(cached["shifts"][s_key].get("gagal", 0))
                if isinstance(cached.get("logs"), list):
                    data_state["logs"] = cached["logs"][:MAX_LOGS]
    except Exception:
        pass

load_state_cache()

# ================= KONFIGURASI HIVEMQ =================
MQTT_BROKER = "311c949a8e4d44afb405b9efa6f0a4e4.s1.eu.hivemq.cloud"
MQTT_PORT = 8883
MQTT_USER = "otomatisasi_sampling"
MQTT_PASS = "sampling123"
MQTT_TOPIC = "otomatisasi_sampling/qc/sampling"
MQTT_STATUS_TOPIC = "otomatisasi_sampling/qc/status"

# ================= CALLBACK MQTT =================
def on_connect(client, userdata, flags, rc):
    with state_lock:
        if rc == 0:
            print("[MQTT] Berhasil terhubung ke HiveMQ Cloud!")
            data_state["koneksi_mqtt"] = True
            client.subscribe(MQTT_TOPIC)
            client.subscribe(MQTT_STATUS_TOPIC)
        else:
            print(f"[MQTT] Gagal terhubung. Kode RC: {rc}")
            data_state["koneksi_mqtt"] = False

def on_disconnect(client, userdata, rc):
    print(f"[MQTT] Terputus dari HiveMQ Cloud (RC: {rc})")
    with state_lock:
        data_state["koneksi_mqtt"] = False
        data_state["esp_online"] = False

def on_message(client, userdata, msg):
    global last_esp_activity, esp_uses_heartbeat, last_raw_berhasil, last_raw_gagal
    raw_text = msg.payload.decode(errors="ignore").strip()

    # 1. Jika pesan masuk dari topik status/heartbeat khusus (otomatisasi_sampling/qc/status)
    if msg.topic == MQTT_STATUS_TOPIC:
        with state_lock:
            if raw_text.upper() in ["OFFLINE", "0", "DISCONNECTED"]:
                last_esp_activity = 0
                data_state["esp_online"] = False
                print("[MQTT STATUS] ESP32 terputus (OFFLINE via Last Will / Status).")
            else:
                last_esp_activity = time.time()
                data_state["esp_online"] = True
        return

    # 2. Jika pesan retained dari broker saat baru start, tandai ONLINE & sinkronkan counter awal tanpa menambah log baru
    if msg.retain:
        try:
            payload = json.loads(raw_text)
            if isinstance(payload, dict):
                with state_lock:
                    last_esp_activity = time.time()
                    data_state["esp_online"] = True
                    if "berhasil" in payload and last_raw_berhasil is None:
                        last_raw_berhasil = int(payload["berhasil"])
                        data_state["berhasil"] = last_raw_berhasil
                    if "gagal" in payload and last_raw_gagal is None:
                        last_raw_gagal = int(payload["gagal"])
                        data_state["gagal"] = last_raw_gagal
        except Exception:
            pass
        print("[MQTT] Pesan retained diterima (log lama diabaikan).")
        return

    # Setiap kali ada pesan masuk non-retained pada topik ESP32, tandai ESP32 ONLINE
    with state_lock:
        last_esp_activity = time.time()
        data_state["esp_online"] = True

    # Jika hanya pesan teks ping/heartbeat/offline singkat non-JSON
    upper_raw = raw_text.upper()
    if upper_raw in ["PING", "HEARTBEAT", "ONLINE"]:
        with state_lock:
            esp_uses_heartbeat = True
        return
    if upper_raw in ["OFFLINE", "DISCONNECTED"]:
        with state_lock:
            last_esp_activity = 0
            data_state["esp_online"] = False
        return

    print(f"[MQTT DITERIMA] Topic: {msg.topic} | Pesan: {raw_text}")
    try:
        payload = json.loads(raw_text)
        if not isinstance(payload, dict):
            return

        with state_lock:
            in_berhasil = int(payload["berhasil"]) if "berhasil" in payload else None
            in_gagal = int(payload["gagal"]) if "gagal" in payload else None

            # 3. Jika paket adalah heartbeat, sinkronkan baseline counter (jika pertama kali) lalu langsung return
            is_heartbeat = (
                payload.get("heartbeat") is True
                or payload.get("heartbeat") in (1, "true", "True", "1")
                or str(payload.get("type", "")).lower() == "heartbeat"
                or str(payload.get("status", "")).upper() in ["HEARTBEAT", "PING", "ONLINE"]
            )
            if is_heartbeat:
                esp_uses_heartbeat = True
                if last_raw_berhasil is None and in_berhasil is not None:
                    last_raw_berhasil = in_berhasil
                    data_state["berhasil"] = in_berhasil
                if last_raw_gagal is None and in_gagal is not None:
                    last_raw_gagal = in_gagal
                    data_state["gagal"] = in_gagal
                return

            now_wib = get_wib_now()
            waktu_sekarang = now_wib.strftime("%H:%M:%S")
            shift_aktif, jam_shift = get_current_shift_info(now_wib)
            data_state["shift_aktif"] = shift_aktif
            data_state["jam_shift_aktif"] = jam_shift

            raw_status = str(payload.get("status", "")).strip()
            upper_status = raw_status.upper()

            # 4. Saat menerima status RESET MANUAL dari ESP32, reset gagal_beruntun & kembalikan status ke STANDBY
            if "RESET" in upper_status:
                data_state["status"] = "STANDBY — MENUNGGU KELIPATAN 100"
                data_state["gagal_beruntun"] = 0
                if in_berhasil is not None:
                    last_raw_berhasil = in_berhasil
                    data_state["berhasil"] = in_berhasil
                if in_gagal is not None:
                    last_raw_gagal = in_gagal
                    data_state["gagal"] = in_gagal
                save_state_cache()
                print("[MQTT RESET MANUAL] Alarm 24V di-reset, status kembali STANDBY.")
                return

            # Jika ESP32 mengirim status STANDBY biasa, jangan reset gagal_beruntun yang sudah tercatat
            if upper_status in ["STANDBY", "STANDBY — MENUNGGU KELIPATAN 100"]:
                if data_state["gagal_beruntun"] < BATAS_GAGAL_ALARM:
                    data_state["status"] = "STANDBY — MENUNGGU KELIPATAN 100"
                return

            is_success_event = (
                upper_status in ["SAMPLE TERDETEKSI", "OK", "BERHASIL", "SAMPLE TERDETEKSI — OK!"]
                or "TERDETEKSI" in upper_status
                or "BERHASIL" in upper_status
            )
            is_fail_event = (
                upper_status in ["GAGAL", "MACET", "TIMEOUT", "GAGAL / MACET"]
                or "GAGAL" in upper_status
                or "MACET" in upper_status
                or "TIMEOUT" in upper_status
                or "ALARM" in upper_status
            )

            # Hitung perubahan counter dari ESP32
            should_log = False
            delta_b = 0
            delta_g = 0

            if in_berhasil is not None or in_gagal is not None:
                cur_b = in_berhasil if in_berhasil is not None else data_state["berhasil"]
                cur_g = in_gagal if in_gagal is not None else data_state["gagal"]

                if last_raw_berhasil is None:
                    should_log = (cur_b + cur_g) > 0
                    delta_b = 1 if (is_success_event and should_log) else 0
                    delta_g = 1 if (is_fail_event and should_log) else 0
                else:
                    if cur_b != last_raw_berhasil or cur_g != (last_raw_gagal or 0):
                        should_log = (cur_b + cur_g) > 0
                        if cur_b > last_raw_berhasil:
                            delta_b = cur_b - last_raw_berhasil
                        elif is_success_event and should_log:
                            delta_b = 1
                        if cur_g > (last_raw_gagal or 0):
                            delta_g = cur_g - (last_raw_gagal or 0)
                        elif is_fail_event and should_log:
                            delta_g = 1

                last_raw_berhasil = cur_b
                last_raw_gagal = cur_g
                data_state["berhasil"] = cur_b
                data_state["gagal"] = cur_g
            else:
                if is_success_event:
                    data_state["berhasil"] += 1
                    should_log = True
                    delta_b = 1
                elif is_fail_event:
                    data_state["gagal"] += 1
                    should_log = True
                    delta_g = 1

            cycle_num = data_state["berhasil"] + data_state["gagal"]
            siklus_label = f"#{cycle_num} ({cycle_num * 100} Karung)"

            # 5. Update status dashboard, statistik shift aktif, & catat satu entri log
            if is_success_event:
                data_state["status"] = "SAMPLE TERDETEKSI — OK!"
                data_state["gagal_beruntun"] = 0
                if should_log and cycle_num > 0:
                    data_state["shifts"][shift_aktif]["berhasil"] += max(1, delta_b)
                    print(f"[MQTT LOG BARU] [{shift_aktif}] Sensor mendeteksi sampel -> Siklus {siklus_label} BERHASIL")
                    data_state["logs"].insert(0, {
                        "waktu": waktu_sekarang,
                        "shift": shift_aktif,
                        "siklus": siklus_label,
                        "respon": "Terdeteksi ESP32",
                        "status": "BERHASIL"
                    })
            elif is_fail_event:
                if "gagal_beruntun" in payload:
                    data_state["gagal_beruntun"] = int(payload["gagal_beruntun"])
                elif should_log and cycle_num > 0:
                    data_state["gagal_beruntun"] += 1

                if data_state["gagal_beruntun"] >= BATAS_GAGAL_ALARM:
                    data_state["status"] = f"ALARM 24V AKTIF: {data_state['gagal_beruntun']}x GAGAL BERTURUT-TURUT — CORONG MACET!"
                else:
                    data_state["status"] = f"GAGAL ({data_state['gagal_beruntun']}/{BATAS_GAGAL_ALARM}) — TIMEOUT 3.0s"

                if should_log and cycle_num > 0:
                    data_state["shifts"][shift_aktif]["gagal"] += max(1, delta_g)
                    print(f"[MQTT LOG BARU] [{shift_aktif}] Timeout sensor -> Siklus {siklus_label} GAGAL / MACET (Beruntun: {data_state['gagal_beruntun']}/{BATAS_GAGAL_ALARM})")
                    data_state["logs"].insert(0, {
                        "waktu": waktu_sekarang,
                        "shift": shift_aktif,
                        "siklus": siklus_label,
                        "respon": f"Timeout (>3.0s) — Gagal Beruntun {data_state['gagal_beruntun']}/{BATAS_GAGAL_ALARM}",
                        "status": "GAGAL / MACET"
                    })
            elif raw_status:
                data_state["status"] = raw_status
                if "gagal_beruntun" in payload:
                    data_state["gagal_beruntun"] = int(payload["gagal_beruntun"])

            # Batasi panjang logs maksimal 100 item terbaru
            if len(data_state["logs"]) > MAX_LOGS:
                data_state["logs"] = data_state["logs"][:MAX_LOGS]

            save_state_cache()

    except Exception as e:
        print(f"Error parsing JSON dari MQTT: {e}")

# Background thread untuk mengecek apakah ESP32 mati (jika menggunakan mode heartbeat berkala)
def check_esp_timeout():
    global data_state, last_esp_activity, esp_uses_heartbeat
    while True:
        time.sleep(2)
        with state_lock:
            shift_aktif, jam_shift = get_current_shift_info()
            data_state["shift_aktif"] = shift_aktif
            data_state["jam_shift_aktif"] = jam_shift
            if esp_uses_heartbeat and last_esp_activity > 0 and (time.time() - last_esp_activity) > 15:
                data_state["esp_online"] = False

timeout_thread = threading.Thread(target=check_esp_timeout)
timeout_thread.daemon = True
timeout_thread.start()

# Inisialisasi MQTT Client
mqtt_client = mqtt.Client()
mqtt_client.tls_set()  # Wajib untuk HiveMQ Cloud
mqtt_client.username_pw_set(MQTT_USER, MQTT_PASS)
mqtt_client.on_connect = on_connect
mqtt_client.on_disconnect = on_disconnect
mqtt_client.on_message = on_message

# Jalankan MQTT di thread terpisah agar tidak memblokir Flask
def start_mqtt():
    try:
        mqtt_client.connect(MQTT_BROKER, MQTT_PORT, 60)
        mqtt_client.loop_forever()
    except Exception as e:
        print(f"[MQTT Thread Error]: {e}")

mqtt_thread = threading.Thread(target=start_mqtt)
mqtt_thread.daemon = True
mqtt_thread.start()


# ================= ROUTING FLASK (WEB) =================
@app.after_request
def add_cors_headers(response):
    response.headers["Access-Control-Allow-Origin"] = "*"
    response.headers["Access-Control-Allow-Headers"] = "Content-Type"
    response.headers["Access-Control-Allow-Methods"] = "GET, POST, OPTIONS"
    return response

@app.route('/')
def index():
    return render_template('index.html')

@app.route('/api/data')
def get_data():
    # Mengembalikan status data termasuk shift aktif & rekap 3 shift terkini
    with state_lock:
        shift_aktif, jam_shift = get_current_shift_info()
        data_state["shift_aktif"] = shift_aktif
        data_state["jam_shift_aktif"] = jam_shift
        return jsonify(data_state)

@app.route('/api/update', methods=['POST'])
def update_data():
    data = request.get_json()
    with state_lock:
        shift_aktif, jam_shift = get_current_shift_info()
        data_state["shift_aktif"] = shift_aktif
        data_state["jam_shift_aktif"] = jam_shift

        if data:
            prev_b = data_state["berhasil"]
            prev_g = data_state["gagal"]
            new_b = int(data.get("berhasil", prev_b))
            new_g = int(data.get("gagal", prev_g))

            data_state["berhasil"] = new_b
            data_state["gagal"] = new_g

            if "gagal_beruntun" in data:
                data_state["gagal_beruntun"] = int(data["gagal_beruntun"])
            if "status" in data:
                data_state["status"] = data.get("status")

            if "shifts" in data and isinstance(data["shifts"], dict):
                for s_key in ["Shift 1", "Shift 2", "Shift 3"]:
                    if s_key in data["shifts"] and isinstance(data["shifts"][s_key], dict):
                        data_state["shifts"][s_key]["berhasil"] = int(data["shifts"][s_key].get("berhasil", 0))
                        data_state["shifts"][s_key]["gagal"] = int(data["shifts"][s_key].get("gagal", 0))
            else:
                if new_b > prev_b:
                    data_state["shifts"][shift_aktif]["berhasil"] += (new_b - prev_b)
                if new_g > prev_g:
                    data_state["shifts"][shift_aktif]["gagal"] += (new_g - prev_g)

            if "logs" in data and isinstance(data["logs"], list):
                for item in data["logs"]:
                    if isinstance(item, dict) and "shift" not in item:
                        item["shift"] = shift_aktif
                data_state["logs"] = data["logs"][:MAX_LOGS]

            save_state_cache()
        return jsonify({"status": "success", "data": data_state})

@app.route('/api/ack_alarm', methods=['POST'])
def ack_alarm():
    # Matikan status alarm & reset hitungan gagal_beruntun TANPA menghapus counter berhasil/gagal maupun logs
    with state_lock:
        data_state["gagal_beruntun"] = 0
        data_state["status"] = "STANDBY — MENUNGGU KELIPATAN 100"
        save_state_cache()
    return jsonify({"status": "alarm_acknowledged", "data": data_state})

@app.route('/api/reset', methods=['POST'])
def reset_data():
    global last_raw_berhasil, last_raw_gagal
    with state_lock:
        shift_aktif, jam_shift = get_current_shift_info()
        data_state["berhasil"] = 0
        data_state["gagal"] = 0
        data_state["gagal_beruntun"] = 0
        data_state["status"] = "STANDBY — MENUNGGU KELIPATAN 100"
        data_state["shift_aktif"] = shift_aktif
        data_state["jam_shift_aktif"] = jam_shift
        data_state["shifts"] = create_empty_shifts()
        data_state["logs"] = []
        last_raw_berhasil = 0
        last_raw_gagal = 0
        save_state_cache()
    return jsonify({"status": "reset", "data": data_state})

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=5000, debug=False)
