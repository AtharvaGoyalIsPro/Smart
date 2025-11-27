/***************************************************
 * ultrasonic.ino — BLE pairing + WiFi + Firebase
 * - BLE GATT server accepts ssid;password;uid;containerID
 * - Notifies status via status characteristic (notify)
 * - Saves prefs and connects to WiFi
 * - Measures ultrasonic distance and pushes fill% to Firebase
 ***************************************************/

#include <WiFi.h>
#include <HTTPClient.h>
#include <Preferences.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <BLE2902.h>

// ---------- PINS ----------
#define TRIG_PIN 5
#define ECHO_PIN 18

// ---------- CONFIG ----------
const float CONTAINER_HEIGHT_CM = 20.0;      // adjust
const unsigned long SEND_INTERVAL_MS = 30000; // 30s
const char* FIREBASE_DB_URL = "https://ultrasonic-c1867-default-rtdb.firebaseio.com";

// ---------- PREFS ----------
Preferences prefs;
const char* PREF_SSID = "wifi_ssid";
const char* PREF_PASS = "wifi_pass";
const char* PREF_UID  = "user_uid";
const char* PREF_CID  = "cont_id";

// ---------- BLE UUIDs ----------
#define SERVICE_UUID        "12345678-1234-5678-1234-56789abcdef0"
#define CHAR_WRITE_UUID     "12345678-1234-5678-1234-56789abcdef1"
#define CHAR_STATUS_UUID    "12345678-1234-5678-1234-56789abcdef2"

BLECharacteristic *pWriteChar;
BLECharacteristic *pStatusChar;
BLEServer *pServer = nullptr;

bool wifiConnected = false;
unsigned long lastSend = 0;

// Forward declarations
bool connectWiFi(const char* ssid, const char* pass, unsigned long timeout_ms);
bool sendFillToFirebase(const char* uid, const char* cid, float fillPercent);
void sendStatus(const char* txt);
void sendPairingHeartbeat(const char* uid, const char* cid, bool paired);
float readDistanceCm();
float computeFillPercent(float d);
void startBLE();

// ---------------- BLE Server callbacks (optional logging) ----------------
class MyServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* pServer) {
    Serial.println("BLE client connected");
  }
  void onDisconnect(BLEServer* pServer) {
    Serial.println("BLE client disconnected");
    // Continue advertising so browser can reconnect
    pServer->getAdvertising()->start();
  }
};

// ---------------- Write callback ----------------
class WriteCallback : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *pChar) {
    std::string rx = pChar->getValue();
    if (rx.length() == 0) return;

    String s(rx.c_str());
    Serial.print("BLE Received: ");
    Serial.println(s);

    int idx1 = s.indexOf(';');
    int idx2 = s.indexOf(';', idx1 + 1);
    int idx3 = s.indexOf(';', idx2 + 1);

    if (idx1 < 0 || idx2 < 0 || idx3 < 0) {
      Serial.println("Invalid format");
      sendStatus("BAD_FORMAT");
      return;
    }

    String ssid = s.substring(0, idx1);
    String pass = s.substring(idx1 + 1, idx2);
    String uid  = s.substring(idx2 + 1, idx3);
    String cid  = s.substring(idx3 + 1);

    // Save
    prefs.putString(PREF_SSID, ssid);
    prefs.putString(PREF_PASS, pass);
    prefs.putString(PREF_UID, uid);
    prefs.putString(PREF_CID, cid);

    Serial.println("Saved credentials");
    sendStatus("SAVED");

    bool ok = connectWiFi(ssid.c_str(), pass.c_str(), 15000);
    if (ok) {
      sendStatus("WIFI_OK");
      sendPairingHeartbeat(uid.c_str(), cid.c_str(), true);
    } else {
      sendStatus("WIFI_FAIL");
    }
  }
};

// ---------------- WiFi connect ----------------
bool connectWiFi(const char* ssid, const char* pass, unsigned long timeout_ms) {
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, pass);
  Serial.printf("Connecting to WiFi: %s\n", ssid);
  unsigned long start = millis();
  while (millis() - start < timeout_ms) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("WiFi connected.");
      Serial.print("IP: "); Serial.println(WiFi.localIP());
      wifiConnected = true;
      return true;
    }
    delay(200);
  }
  Serial.println("WiFi connect failed.");
  wifiConnected = false;
  return false;
}

// ---------------- send status (notify) ----------------
void sendStatus(const char* txt) {
  if (!pStatusChar) return;
  pStatusChar->setValue(String(txt));
  pStatusChar->notify();
  Serial.print("Status -> "); Serial.println(txt);
}

// ---------------- Firebase send ----------------
bool sendFillToFirebase(const char* uid, const char* cid, float fillPercent) {
  String path = String("/users/") + uid + "/containers/" + cid + ".json";
  String url  = String(FIREBASE_DB_URL) + path;

  String body = "{";
  body += "\"fillPercent\":" + String(fillPercent,1) + ",";
  body += "\"lastUpdate\":\"" + String((unsigned long)time(nullptr)) + "\"";
  body += "}";

  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.PUT(body);
  if (code > 0) {
    Serial.printf("Firebase HTTP %d\n", code);
    http.end();
    return (code >= 200 && code < 300);
  }
  Serial.println("Firebase send failed");
  http.end();
  return false;
}

void sendPairingHeartbeat(const char* uid, const char* cid, bool paired) {
  String url = String(FIREBASE_DB_URL) + "/users/" + uid + "/containers/" + cid + ".json";
  String body = String("{\"paired\":") + (paired ? "true" : "false") + "}";
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  int code = http.PATCH(body);
  Serial.printf("Pairing PATCH code %d\n", code);
  http.end();
}

// ---------------- Ultrasonic ----------------
float readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);
  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;
  float dist = (duration / 2.0) * 0.0343;
  return dist;
}
float computeFillPercent(float d) {
  if (d < 0) return 0;
  float filled = CONTAINER_HEIGHT_CM - d;
  if (filled < 0) filled = 0;
  float pct = (filled / CONTAINER_HEIGHT_CM) * 100.0;
  if (pct < 0) pct = 0;
  if (pct > 100) pct = 100;
  return pct;
}

// ---------------- BLE start ----------------
void startBLE() {
  BLEDevice::init("SmartContainer");
  pServer = BLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  BLEService *service = pServer->createService(SERVICE_UUID);

  pWriteChar = service->createCharacteristic(
    CHAR_WRITE_UUID,
    BLECharacteristic::PROPERTY_WRITE
  );
  pWriteChar->setCallbacks(new WriteCallback());

  pStatusChar = service->createCharacteristic(
    CHAR_STATUS_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );
  pStatusChar->addDescriptor(new BLE2902());
  pStatusChar->setValue("WAITING");

  service->start();

  BLEAdvertising *adv = pServer->getAdvertising();
  adv->addServiceUUID(SERVICE_UUID);
  adv->start();

  Serial.println("BLE advertising started.");
}

// ---------------- Setup & Loop ----------------
void setup() {
  Serial.begin(115200);
  delay(1000);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);
  prefs.begin("container", false);

  String ssid = prefs.getString(PREF_SSID, "");
  String pass = prefs.getString(PREF_PASS, "");
  if (ssid.length() > 0) {
    connectWiFi(ssid.c_str(), pass.c_str(), 8000);
  }

  startBLE();
  lastSend = millis();
}

void loop() {
  if (!wifiConnected) {
    String ssid = prefs.getString(PREF_SSID, "");
    String pass = prefs.getString(PREF_PASS, "");
    if (ssid.length() > 0) {
      connectWiFi(ssid.c_str(), pass.c_str(), 8000);
    }
  }

  if (wifiConnected && millis() - lastSend >= SEND_INTERVAL_MS) {
    lastSend = millis();
    String uid = prefs.getString(PREF_UID, "");
    String cid = prefs.getString(PREF_CID, "");
    if (uid.length() == 0 || cid.length() == 0) return;
    float d = readDistanceCm();
    if (d < 0) {
      sendStatus("NO_ECHO");
      return;
    }
    float pct = computeFillPercent(d);
    Serial.printf("Distance %.1f cm -> Fill %.1f%%\n", d, pct);
    sendStatus(String("FILL " + String(pct,1)).c_str());
    sendFillToFirebase(uid.c_str(), cid.c_str(), pct);
  }
}
