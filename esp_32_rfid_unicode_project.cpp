/*
  esp32_rfid_unicode_project.cpp
  -------------------------------
  Purpose:
  A practical, polished ESP32 + MFRC522 RFID attendance project that fully supports
  Unicode (UTF-8) user names, CSV/JSON UTF-8 logging, and a simple web management UI.

  Key features (designed to be unique and useful for a company demo):
  - Uses ESP32 with MFRC522 RFID reader
  - Stores user profiles as UTF-8 JSON files in SPIFFS (/users/<UID>.json)
  - Logs attendance to CSV on SPIFFS or optional SD card using UTF-8 with BOM
  - Provides a lightweight async web UI (ESPAsyncWebServer) to add/edit users with Unicode names
  - Sends websocket messages to web clients on scans (UTF-8 safe)
  - Clear, modular, well-commented single-file code for demonstration and easy extension

  Notes / Requirements:
  - Libraries: MFRC522, SPIFFS (built-in for ESP32 core), SPI, Wire, WiFi, AsyncTCP,
    ESPAsyncWebServer, ArduinoJson, SD (optional)
  - Flash this to an ESP32 board. Connect MFRC522 with SPI (SDA=SS_PIN, SCK, MOSI, MISO, RST)
  - Web UI will show /index.html, and you can add user names in any language (UTF-8)
  - For production, add authentication to web UI (left simple here for clarity)

  Wiring example (MFRC522):
    ESP32  MOSI -> MOSI
    ESP32  MISO -> MISO
    ESP32  SCK  -> SCK
    ESP32  SS   -> SS_PIN (default 5)
    ESP32  RST  -> RST_PIN (default 22)

  Author: ChatGPT (project-style educational example)
*/

#include <Arduino.h>
#include <WiFi.h>
#include <SPI.h>
#include <FS.h>
#include <SPIFFS.h>
#include <MFRC522.h>
#include <ArduinoJson.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SD.h> // optional: define ENABLE_SD to enable SD logging

#define ENABLE_SD 0

// ------------------ CONFIG ------------------
// Put your WiFi credentials here (or implement WiFiManager later)
const char* WIFI_SSID = "YourSSID";
const char* WIFI_PASS = "YourPassword";

// Pins for MFRC522
const uint8_t SS_PIN = 5;   // SDA
const uint8_t RST_PIN = 22; // RST

// Attendance log file (UTF-8 CSV)
const char* ATTENDANCE_CSV = "/attendance.csv"; // on SPIFFS

// Directory for users
const char* USERS_DIR = "/users";

// Buzzer and LED
const uint8_t BUZZER_PIN = 13;
const uint8_t LED_PIN = 2; // onboard LED

// Webserver port
const int WEB_PORT = 80;

// ------------------ GLOBALS ------------------
MFRC522 mfrc522(SS_PIN, RST_PIN);
AsyncWebServer server(WEB_PORT);
AsyncWebSocket ws("/ws");

// Simple map in memory to cache user UID -> name (UTF-8). We load at startup.
// Using STL String (Arduino) which supports UTF-8 byte sequences.
#include <map>
#include <vector>
std::map<String, String> userCache; // uid -> utf8 name

// ------------------ UTILITIES ------------------

// Ensure SPIFFS is mounted and users dir exists
void ensureSPIFFS()
{
  if (!SPIFFS.begin(true)) {
    Serial.println("[ERR] SPIFFS mount failed");
    return;
  }
  if (!SPIFFS.exists(USERS_DIR)) {
    SPIFFS.mkdir(USERS_DIR);
  }
}

// Write CSV header with UTF-8 BOM if file doesn't exist
void ensureAttendanceCSV()
{
  if (!SPIFFS.exists(ATTENDANCE_CSV)) {
    File f = SPIFFS.open(ATTENDANCE_CSV, FILE_WRITE);
    if (!f) {
      Serial.println("[ERR] Cannot create attendance CSV");
      return;
    }
    // Write UTF-8 BOM so Excel recognizes UTF-8
    const uint8_t bom[3] = {0xEF, 0xBB, 0xBF};
    f.write(bom, 3);
    f.println("timestamp,uid,name,method");
    f.close();
  }
}

// CSV-safe: wrap string in quotes and escape internal quotes
String csvEsc(const String &s) {
  String out = "\"";
  for (size_t i = 0; i < s.length(); ++i) {
    char c = s[i];
    if (c == '"') out += '"';
    out += c;
  }
  out += "\"";
  return out;
}

// Get current timestamp string (Unix seconds). For demo we use millis()/1000 + boot epoch.
String nowTimestamp() {
  unsigned long t = millis() / 1000; // demo timestamp
  return String(t);
}

// Log attendance (append to CSV). method = "rfid" or "web" etc.
void logAttendance(const String &uid, const String &name, const String &method)
{
  ensureAttendanceCSV();
  File f = SPIFFS.open(ATTENDANCE_CSV, FILE_APPEND);
  if (!f) {
    Serial.println("[ERR] Cannot open attendance CSV for append");
    return;
  }
  String line = nowTimestamp() + "," + csvEsc(uid) + "," + csvEsc(name) + "," + csvEsc(method);
  f.println(line);
  f.close();
  Serial.println("[LOG] " + line);
#ifdef ENABLE_SD
  // If SD enabled, also append to SD for redundancy
  File sd = SD.open(ATTENDANCE_CSV, FILE_APPEND);
  if (sd) { sd.println(line); sd.close(); }
#endif
}

// Utility: write a user JSON to SPIFFS: /users/<uid>.json
bool writeUserToFS(const String &uid, const String &utf8name)
{
  String path = String(USERS_DIR) + "/" + uid + ".json";
  DynamicJsonDocument doc(256);
  doc["uid"] = uid;
  doc["name"] = utf8name;
  File f = SPIFFS.open(path, FILE_WRITE);
  if (!f) return false;
  if (serializeJson(doc, f) == 0) {
    f.close();
    return false;
  }
  f.close();
  return true;
}

// Load all users from /users into userCache
void loadUsers()
{
  userCache.clear();
  File root = SPIFFS.open(USERS_DIR);
  if (!root) {
    Serial.println("[WARN] No users directory");
    return;
  }
  File file = root.openNextFile();
  while (file) {
    String name = file.name();
    if (name.endsWith(".json")) {
      File u = SPIFFS.open(name);
      if (u) {
        size_t sz = u.size();
        std::unique_ptr<char[]> buf(new char[sz + 1]);
        u.readBytes(buf.get(), sz);
        buf[sz] = '\0';
        DynamicJsonDocument doc(512);
        DeserializationError err = deserializeJson(doc, buf.get());
        if (!err) {
          String uid = doc["uid"].as<String>();
          String uname = doc["name"].as<String>();
          userCache[uid] = uname;
          Serial.println("[USER] Loaded: " + uid + " -> " + uname);
        }
        u.close();
      }
    }
    file = root.openNextFile();
  }
}

// ------------------ WEB HANDLERS ------------------

// Serve a minimal index.html with JS to add users and show websocket events
const char index_html[] PROGMEM = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8" />
<title>ESP32 RFID Unicode Attendance</title>
<style>body{font-family:system-ui,Segoe UI,Roboto,Arial;padding:12px}label{display:block;margin-top:8px}</style>
</head>
<body>
<h2>ESP32 RFID - Unicode Attendance</h2>
<div>
  <h3>Add User</h3>
  <label>UID (hex): <input id="uid" /></label>
  <label>Name (Unicode): <input id="name" /></label>
  <button onclick="addUser()">Add User</button>
  <div id="addres"></div>
</div>
<div>
  <h3>Live Events</h3>
  <ul id="events"></ul>
</div>
<script>
let ws = new WebSocket('ws://' + location.host + '/ws');
ws.onmessage = (evt)=>{
  try{ let d = JSON.parse(evt.data); let el = document.createElement('li'); el.textContent = '['+d.timestamp+'] '+d.uid+' - '+d.name+' ('+d.result+')'; document.getElementById('events').prepend(el);}catch(e){console.log(e)}
}
function addUser(){
  let uid = document.getElementById('uid').value.trim();
  let name = document.getElementById('name').value.trim();
  if(!uid||!name){document.getElementById('addres').textContent='UID and Name required';return}
  fetch('/adduser', {method:'POST', body: JSON.stringify({uid:uid,name:name})}).then(r=>r.text()).then(t=>document.getElementById('addres').textContent=t)
}
</script>
</body>
</html>
)rawliteral";

// Add user POST handler
void handleAddUser(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total)
{
  // data is raw body
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, data, len);
  if (err) {
    request->send(400, "text/plain", "Invalid JSON");
    return;
  }
  String uid = doc["uid"].as<String>();
  String name = doc["name"].as<String>();
  if (uid.length() == 0 || name.length() == 0) {
    request->send(400, "text/plain", "Missing fields");
    return;
  }
  // save
  if (!writeUserToFS(uid, name)) {
    request->send(500, "text/plain", "Failed to save user");
    return;
  }
  userCache[uid] = name;
  request->send(200, "text/plain", "User saved");
  Serial.println("[WEB] Added user: " + uid + " -> " + name);
}

// Websockets: broadcast scan event
void broadcastScan(const String &uid, const String &name, const String &result)
{
  DynamicJsonDocument root(256);
  root["timestamp"] = nowTimestamp();
  root["uid"] = uid;
  root["name"] = name;
  root["result"] = result;
  String out;
  serializeJson(root, out);
  ws.textAll(out);
}

// ------------------ RFID HANDLING ------------------

// Convert MFRC522 UID bytes to uppercase hex string (no spaces)
String uidFromMfrc(const MFRC522::Uid &u)
{
  String s = "";
  for (byte i = 0; i < u.size; i++) {
    char buf[3];
    sprintf(buf, "%02X", u.uidByte[i]);
    s += buf;
  }
  return s;
}

// Give audible/visual feedback
void feedbackOK() {
  digitalWrite(LED_PIN, HIGH);
  tone(BUZZER_PIN, 1500, 120);
  delay(120);
  digitalWrite(LED_PIN, LOW);
}
void feedbackFail() {
  for (int i=0;i<2;i++){
    digitalWrite(LED_PIN, HIGH);
    tone(BUZZER_PIN, 600, 100);
    delay(120);
    digitalWrite(LED_PIN, LOW);
    delay(80);
  }
}

// Process a scanned UID
void processUID(const String &uid)
{
  String name = "(unknown)";
  String result = "denied";
  auto it = userCache.find(uid);
  if (it != userCache.end()) {
    name = it->second;
    result = "accepted";
    feedbackOK();
  } else {
    feedbackFail();
  }
  logAttendance(uid, name, "rfid");
  broadcastScan(uid, name, result);
  // Print UTF-8 name to Serial (Serial monitor must be UTF-8 aware)
  Serial.printf("Scan: %s -> %s (%s)\n", uid.c_str(), name.c_str(), result.c_str());
}

// ------------------ SETUP ------------------

void setup()
{
  Serial.begin(115200);
  delay(1000);
  pinMode(BUZZER_PIN, OUTPUT);
  pinMode(LED_PIN, OUTPUT);

  ensureSPIFFS();
  ensureAttendanceCSV();

#ifdef ENABLE_SD
  if (!SD.begin()) Serial.println("[WARN] SD card not initialized");
#endif

  // init rfid
  SPI.begin();
  mfrc522.PCD_Init();
  Serial.println("[OK] MFRC522 init done");

  // load users
  loadUsers();

  // Connect WiFi
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  Serial.print("[WIFI] Connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500); Serial.print('.'); attempts++;
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println();
    Serial.print("[WIFI] Connected: "); Serial.println(WiFi.localIP());
  } else {
    Serial.println();
    Serial.println("[WIFI] Failed to connect - starting AP mode");
    WiFi.softAP("ESP32-RFID-AP");
    Serial.print("[AP] "); Serial.println(WiFi.softAPIP());
  }

  // Setup websocket
  ws.onEvent([](AsyncWebSocket * server, AsyncWebSocketClient * client, AwsEventType type, void * arg, uint8_t *data, size_t len){
    // no-op for demo
  });
  server.addHandler(&ws);

  // HTTP routes
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){ request->send_P(200, "text/html", index_html); });
  server.on("/adduser", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, handleAddUser);

  // serve SPIFFS files
  server.serveStatic("/files", SPIFFS, "/");

  server.begin();
  Serial.println("[WEB] Server started");
}

// ------------------ MAIN LOOP ------------------

void loop()
{
  // RFID loop: if a new card present
  if (mfrc522.PICC_IsNewCardPresent() && mfrc522.PICC_ReadCardSerial()) {
    String uid = uidFromMfrc(mfrc522.uid);
    processUID(uid);
    mfrc522.PICC_HaltA();
    delay(300); // debounce
  }

  // Optional: perform maintenance tasks
  ws.cleanupClients();
}

// EOF
