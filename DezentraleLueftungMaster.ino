#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <PubSubClient.h>

// =======================
// RS485-Konfiguration
// =======================
#define RS485_CTRL_PIN 4       // Steuerung (DE/RE) des MAX485
#define RS485_RX_PIN   16      // RS485 RX (Serial2)
#define RS485_TX_PIN   17      // RS485 TX (Serial2)
const unsigned long RS485_BAUD = 9600;

// =======================
// Client-Paare (dynamisch konfigurierbar)
// =======================
struct ClientPair {
  int clientA;
  int clientB;
  bool state; // false: A = FANIN, B = FANOUT; true: A = FANOUT, B = FANIN
};

#define MAX_PAIRS 10
ClientPair pairs[MAX_PAIRS];
int pairCount = 0; // Anzahl konfigurierte Paare

// Zum schnellen Polling: Alle Clients aus den Paaren in einem Array
int clientList[MAX_PAIRS * 2];
int totalClients = 0;
int currentPollIndex = 0;     // Index im clientList, welcher Client wird als Nächstes abgefragt
int currentPolledClient = 0;  // Aktuell abgefragter Client (ID)

// =======================
// Sensor-Daten der Clients
// Für jeden Client (IDs 1 bis MAX_CLIENT_ID) speichern wir 4 Sensorwerte
struct SensorData {
  float temperatures[4];
  float humidities[4];
  bool valid;
};

#define MAX_CLIENT_ID 20
SensorData sensorData[MAX_CLIENT_ID + 1];  // Index 0 unbenutzt

// =======================
// Konfigurationsvariablen (Standardswerte)
// =======================
String stationSSID = "";
String stationPassword = "";
bool useStationMode = false;  // true: Station-Modus, false: AP-Modus

String mqttBroker = "";
int mqttPort = 1883;
String mqttUser = "";
String mqttPassword = "";
String mqttTopic = "esp32/status";

unsigned long switchInterval = 70000;      // Umschaltintervall für Lüfter (Standard 70 Sekunden)
unsigned long sensorUpdateInterval = 5000;   // Sensor-Abfrageintervall (Standard 5 Sekunden)

unsigned long lastSwitchTime = 0;
unsigned long lastSensorPollTime = 0;

// =======================
// Netzwerk & MQTT
// =======================
WiFiClient wifiClient;
PubSubClient mqttClient(wifiClient);
WebServer server(80);
Preferences preferences;

// =======================
// RS485-Funktionen
// =======================
void sendRS485(String msg) {
  digitalWrite(RS485_CTRL_PIN, HIGH); // Sende-Modus
  Serial2.println(msg);
  Serial2.flush();
  digitalWrite(RS485_CTRL_PIN, LOW);  // Zurück in Empfangsmodus
  Serial.println("RS485 gesendet: " + msg);
}

void sendClientCommand(int clientId, String command) {
  String activateCmd = "ACTIVATECLIENT" + String(clientId);
  sendRS485(activateCmd);
  delay(50);
  sendRS485(command);
  delay(50);
}

// =======================
// Funktionen zum Laden/Speichern der Client-Paare
// Im Format: "1,2;3,4;5,6"
void loadClientPairs(String pairStr) {
  pairCount = 0;
  totalClients = 0;
  int start = 0;
  while (true) {
    int semicolonIndex = pairStr.indexOf(';', start);
    String pairEntry;
    if (semicolonIndex == -1) {
      pairEntry = pairStr.substring(start);
    } else {
      pairEntry = pairStr.substring(start, semicolonIndex);
    }
    pairEntry.trim();
    if (pairEntry.length() > 0) {
      int commaIndex = pairEntry.indexOf(',');
      if (commaIndex != -1) {
        String aStr = pairEntry.substring(0, commaIndex);
        String bStr = pairEntry.substring(commaIndex + 1);
        int a = aStr.toInt();
        int b = bStr.toInt();
        if (a > 0 && b > 0 && pairCount < MAX_PAIRS) {
          pairs[pairCount].clientA = a;
          pairs[pairCount].clientB = b;
          pairs[pairCount].state = false;  // Standardzustand
          clientList[totalClients++] = a;
          clientList[totalClients++] = b;
          pairCount++;
        }
      }
    }
    if (semicolonIndex == -1) break;
    start = semicolonIndex + 1;
  }
  Serial.println("Client-Paare geladen: " + String(pairCount) + " Paare, " + String(totalClients) + " Clients.");
}

String saveClientPairs() {
  String pairStr = "";
  for (int i = 0; i < pairCount; i++) {
    pairStr += String(pairs[i].clientA) + "," + String(pairs[i].clientB);
    if (i < pairCount - 1)
      pairStr += ";";
  }
  return pairStr;
}

// =======================
// Laden und Speichern der Einstellungen (Preferences)
// =======================
void loadSettings() {
  preferences.begin("esp32master", true);
  stationSSID = preferences.getString("ssid", "");
  stationPassword = preferences.getString("pass", "");
  useStationMode = preferences.getBool("useStation", false);
  mqttBroker = preferences.getString("mqttBroker", "");
  mqttPort = preferences.getInt("mqttPort", 1883);
  mqttUser = preferences.getString("mqttUser", "");
  mqttPassword = preferences.getString("mqttPass", "");
  mqttTopic = preferences.getString("mqttTopic", "esp32/status");
  switchInterval = preferences.getULong("switchInterval", 70000);
  sensorUpdateInterval = preferences.getULong("sensorInterval", 5000);
  String pairStr = preferences.getString("clientPairs", "1,2;3,4;5,6");
  loadClientPairs(pairStr);
  preferences.end();
}

void saveSettings() {
  preferences.begin("esp32master", false);
  preferences.putString("ssid", stationSSID);
  preferences.putString("pass", stationPassword);
  preferences.putBool("useStation", useStationMode);
  preferences.putString("mqttBroker", mqttBroker);
  preferences.putInt("mqttPort", mqttPort);
  preferences.putString("mqttUser", mqttUser);
  preferences.putString("mqttPass", mqttPassword);
  preferences.putString("mqttTopic", mqttTopic);
  preferences.putULong("switchInterval", switchInterval);
  preferences.putULong("sensorInterval", sensorUpdateInterval);
  String pairStr = saveClientPairs();
  preferences.putString("clientPairs", pairStr);
  preferences.end();
}

// =======================
// WiFi-Verbindung aufbauen
// =======================
void setupWiFi() {
  if (useStationMode && stationSSID.length() > 0) {
    Serial.println("Versuche, als Station mit SSID " + stationSSID + " zu verbinden...");
    WiFi.mode(WIFI_STA);
    WiFi.begin(stationSSID.c_str(), stationPassword.c_str());
    unsigned long startAttemptTime = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttemptTime < 10000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\nWiFi verbunden, IP: " + WiFi.localIP().toString());
      return;
    }
    Serial.println("\nWiFi-Verbindung fehlgeschlagen, starte AP-Modus.");
  }
  // AP-Modus
  WiFi.mode(WIFI_AP);
  WiFi.softAP("ESP32_Master_AP", "esp32ap");
  Serial.println("Access Point gestartet, IP: " + WiFi.softAPIP().toString());
}

// =======================
// MQTT-Funktionen
// =======================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  // Hier könnten eingehende MQTT-Nachrichten verarbeitet werden.
}

void setupMQTT() {
  if (mqttBroker.length() > 0) {
    mqttClient.setServer(mqttBroker.c_str(), mqttPort);
    mqttClient.setCallback(mqttCallback);
  }
}

void reconnectMQTT() {
  if (mqttBroker.length() == 0) return;
  while (!mqttClient.connected()) {
    Serial.print("MQTT-Verbindung wird aufgebaut...");
    String clientId = "ESP32Master-";
    clientId += String(random(0xffff), HEX);
    if (mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPassword.c_str())) {
      Serial.println(" verbunden.");
    } else {
      Serial.print("Fehler, rc=");
      Serial.print(mqttClient.state());
      Serial.println(" versuche in 5 Sekunden erneut.");
      delay(5000);
    }
  }
}

void publishStatus() {
  if (!mqttClient.connected()) return;
  String payload = "{";
  payload += "\"switchInterval\":" + String(switchInterval) + ",";
  payload += "\"sensorInterval\":" + String(sensorUpdateInterval) + ",";
  payload += "\"pairs\":[";
  for (int i = 0; i < pairCount; i++) {
    payload += "{";
    payload += "\"clientA\":" + String(pairs[i].clientA) + ",";
    payload += "\"clientB\":" + String(pairs[i].clientB) + ",";
    payload += "\"modeA\":\"" + String(pairs[i].state ? "FANOUT" : "FANIN") + "\",";
    payload += "\"modeB\":\"" + String(pairs[i].state ? "FANIN" : "FANOUT") + "\"";
    payload += "}";
    if (i < pairCount - 1)
      payload += ",";
  }
  payload += "],";
  payload += "\"sensors\":{";
  for (int i = 0; i < totalClients; i++) {
    int cid = clientList[i];
    payload += "\"client" + String(cid) + "\":{";
    if (sensorData[cid].valid) {
      payload += "\"sensors\":[";
      for (int j = 0; j < 4; j++) {
        payload += "{";
        payload += "\"temp\":" + String(sensorData[cid].temperatures[j], 1) + ",";
        payload += "\"hum\":" + String(sensorData[cid].humidities[j], 1);
        payload += "}";
        if (j < 3)
          payload += ",";
      }
      payload += "]";
    } else {
      payload += "\"error\":\"no data\"";
    }
    payload += "}";
    if (i < totalClients - 1)
      payload += ",";
  }
  payload += "}";
  payload += "}";
  mqttClient.publish(mqttTopic.c_str(), payload.c_str());
  Serial.println("MQTT Status veröffentlicht: " + payload);
}

// =======================
// RS485-Antworten verarbeiten
// =======================
void processRS485() {
  if (Serial2.available()) {
    String response = Serial2.readStringUntil('\n');
    response.trim();
    if (response.length() > 0) {
      Serial.println("RS485 Antwort: " + response);
      // Einfaches Parsing: Suche nach "Temp=" und "Hum=" in der Antwort
      int index = 0;
      for (int sensor = 0; sensor < 4; sensor++) {
        int tempIndex = response.indexOf("Temp=", index);
        if (tempIndex == -1)
          break;
        int startNum = tempIndex + 5;
        int endNum = response.indexOf("°C", startNum);
        if (endNum == -1)
          break;
        String tempStr = response.substring(startNum, endNum);
        float temp = tempStr.toFloat();
        
        int humIndex = response.indexOf("Hum=", endNum);
        if (humIndex == -1)
          break;
        int startHum = humIndex + 4;
        int endHum = response.indexOf("%", startHum);
        if (endHum == -1)
          break;
        String humStr = response.substring(startHum, endHum);
        float hum = humStr.toFloat();
        
        sensorData[currentPolledClient].temperatures[sensor] = temp;
        sensorData[currentPolledClient].humidities[sensor] = hum;
        sensorData[currentPolledClient].valid = true;
        
        index = endHum;
      }
    }
  }
}

// =======================
// Webserver: HTML-Seiten
// =======================

// Erzeugt die Status-Seite mit Tabs (Status & Einstellungen)
String getStatusPage() {
  String html = "<!DOCTYPE html><html><head><meta charset='UTF-8'><title>ESP32 Master Status</title>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; }";
  html += ".tab { overflow: hidden; border: 1px solid #ccc; background-color: #f1f1f1; }";
  html += ".tab button { background-color: inherit; float: left; border: none; outline: none; cursor: pointer; padding: 14px 16px; transition: 0.3s; font-size: 17px; }";
  html += ".tab button:hover { background-color: #ddd; }";
  html += ".tab button.active { background-color: #ccc; }";
  html += ".tabcontent { display: none; padding: 20px; border: 1px solid #ccc; border-top: none; }";
  // Styling für Client-Paare
  html += ".pair { border: 1px solid #ccc; padding: 10px; margin-bottom: 20px; }";
  html += ".client { display: inline-block; width: 45%; text-align: center; vertical-align: top; }";
  html += ".fan-icon { font-size: 40px; display: inline-block; }";
  html += ".rotating { animation: rotation 2s infinite linear; }";
  html += "@keyframes rotation { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }";
  html += ".arrow { font-size: 40px; margin: 10px; }";
  html += "</style>";
  html += "<script>";
  html += "function openTab(evt, tabName) { var i, tabcontent, tablinks; tabcontent = document.getElementsByClassName('tabcontent'); for (i = 0; i < tabcontent.length; i++) { tabcontent[i].style.display = 'none'; } tablinks = document.getElementsByClassName('tablinks'); for (i = 0; i < tablinks.length; i++) { tablinks[i].className = tablinks[i].className.replace(' active', ''); } document.getElementById(tabName).style.display = 'block'; evt.currentTarget.className += ' active'; }";
  html += "window.onload = function() { document.getElementsByClassName('tablinks')[0].click(); }";
  html += "</script>";
  html += "</head><body>";
  html += "<h1>ESP32 Master Status</h1>";
  html += "<div class='tab'>";
  html += "<button class='tablinks' onclick=\"openTab(event, 'Status')\">Status</button>";
  html += "<button class='tablinks' onclick=\"openTab(event, 'Einstellungen')\">Einstellungen</button>";
  html += "</div>";
  
  // Tab: Status
  html += "<div id='Status' class='tabcontent'>";
  html += "<h2>Client-Paare und Sensoren</h2>";
  for (int i = 0; i < pairCount; i++) {
    html += "<div class='pair'>";
    html += "<h3>Paar " + String(i + 1) + "</h3>";
    int clientA = pairs[i].clientA;
    int clientB = pairs[i].clientB;
    String modeA = (pairs[i].state ? "FANOUT" : "FANIN");
    String modeB = (pairs[i].state ? "FANIN" : "FANOUT");
    String arrowA = (pairs[i].state ? "&#8594;" : "&#8592;");
    String arrowB = (pairs[i].state ? "&#8592;" : "&#8594;");
    
    html += "<div class='client'>";
    html += "<h4>Client " + String(clientA) + "</h4>";
    html += "<div class='fan-icon rotating'>🌀</div>";
    html += "<div>" + modeA + " " + arrowA + "</div>";
    if (sensorData[clientA].valid) {
      html += "<div>Sensors:<br>";
      for (int j = 0; j < 4; j++) {
        html += "S" + String(j + 1) + ": " + String(sensorData[clientA].temperatures[j], 1) + "°C, " + String(sensorData[clientA].humidities[j], 1) + "%<br>";
      }
      html += "</div>";
    } else {
      html += "<div>No data</div>";
    }
    html += "</div>";
    
    html += "<div class='client'>";
    html += "<h4>Client " + String(clientB) + "</h4>";
    html += "<div class='fan-icon rotating'>🌀</div>";
    html += "<div>" + modeB + " " + arrowB + "</div>";
    if (sensorData[clientB].valid) {
      html += "<div>Sensors:<br>";
      for (int j = 0; j < 4; j++) {
        html += "S" + String(j + 1) + ": " + String(sensorData[clientB].temperatures[j], 1) + "°C, " + String(sensorData[clientB].humidities[j], 1) + "%<br>";
      }
      html += "</div>";
    } else {
      html += "<div>No data</div>";
    }
    html += "</div>";
    html += "<div style='clear: both;'></div>";
    html += "</div>";
  }
  html += "</div>";
  
  // Tab: Einstellungen
  html += "<div id='Einstellungen' class='tabcontent'>";
  html += "<h2>Einstellungen</h2>";
  html += "<form method='POST' action='/saveSettings'>";
  // WiFi-Einstellungen
  html += "<h3>WiFi Einstellungen</h3>";
  html += "Modus: <select name='wifiMode'>";
  html += "<option value='AP'" + String((!useStationMode) ? " selected" : "") + ">Access Point</option>";
  html += "<option value='STA'" + String((useStationMode) ? " selected" : "") + ">Station</option>";
  html += "</select><br>";
  html += "SSID: <input type='text' name='ssid' value='" + stationSSID + "'><br>";
  html += "Passwort: <input type='text' name='pass' value='" + stationPassword + "'><br>";
  // MQTT-Einstellungen
  html += "<h3>MQTT Einstellungen</h3>";
  html += "Broker: <input type='text' name='mqttBroker' value='" + mqttBroker + "'><br>";
  html += "Port: <input type='number' name='mqttPort' value='" + String(mqttPort) + "'><br>";
  html += "User: <input type='text' name='mqttUser' value='" + mqttUser + "'><br>";
  html += "Passwort: <input type='text' name='mqttPass' value='" + mqttPassword + "'><br>";
  html += "Topic: <input type='text' name='mqttTopic' value='" + mqttTopic + "'><br>";
  // Intervalle
  html += "<h3>Intervalle</h3>";
  html += "Fan Umschaltintervall (ms): <input type='number' name='switchInterval' value='" + String(switchInterval) + "'><br>";
  html += "Sensor Update Intervall (ms): <input type='number' name='sensorInterval' value='" + String(sensorUpdateInterval) + "'><br>";
  // Client-Paare
  html += "<h3>Client Paare (Format: 1,2;3,4;...)</h3>";
  String pairStr = saveClientPairs();
  html += "<input type='text' name='clientPairs' value='" + pairStr + "'><br>";
  html += "<input type='submit' value='Speichern'>";
  html += "</form>";
  html += "</div>";
  
  html += "</body></html>";
  return html;
}

void handleSaveSettings() {
  if (server.hasArg("wifiMode")) {
    String mode = server.arg("wifiMode");
    useStationMode = (mode == "STA");
  }
  if (server.hasArg("ssid")) {
    stationSSID = server.arg("ssid");
  }
  if (server.hasArg("pass")) {
    stationPassword = server.arg("pass");
  }
  if (server.hasArg("mqttBroker")) {
    mqttBroker = server.arg("mqttBroker");
  }
  if (server.hasArg("mqttPort")) {
    mqttPort = server.arg("mqttPort").toInt();
  }
  if (server.hasArg("mqttUser")) {
    mqttUser = server.arg("mqttUser");
  }
  if (server.hasArg("mqttPass")) {
    mqttPassword = server.arg("mqttPass");
  }
  if (server.hasArg("mqttTopic")) {
    mqttTopic = server.arg("mqttTopic");
  }
  if (server.hasArg("switchInterval")) {
    switchInterval = server.arg("switchInterval").toInt();
  }
  if (server.hasArg("sensorInterval")) {
    sensorUpdateInterval = server.arg("sensorInterval").toInt();
  }
  if (server.hasArg("clientPairs")) {
    String pairStr = server.arg("clientPairs");
    loadClientPairs(pairStr);
  }
  saveSettings();
  server.sendHeader("Location", "/", true);
  server.send(302, "text/plain", "Einstellungen gespeichert");
}

void handleRoot() {
  String page = getStatusPage();
  server.send(200, "text/html", page);
}

// =======================
// Setup & Loop
// =======================
void setup() {
  Serial.begin(115200);
  Serial.println("ESP32 Master startet...");
  
  pinMode(RS485_CTRL_PIN, OUTPUT);
  digitalWrite(RS485_CTRL_PIN, LOW);
  Serial2.begin(RS485_BAUD, SERIAL_8N1, RS485_RX_PIN, RS485_TX_PIN);
  Serial.println("RS485 (Serial2) gestartet");
  
  // Einstellungen laden
  loadSettings();
  
  setupWiFi();
  setupMQTT();
  
  server.on("/", HTTP_GET, handleRoot);
  server.on("/saveSettings", HTTP_POST, handleSaveSettings);
  server.begin();
  Serial.println("Webserver gestartet");
  
  lastSwitchTime = millis();
  lastSensorPollTime = millis();
  
  // Sensor-Daten initialisieren
  for (int i = 0; i <= MAX_CLIENT_ID; i++) {
    sensorData[i].valid = false;
  }
}

void loop() {
  server.handleClient();
  
  if (useStationMode && WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }
  
  if (mqttBroker.length() > 0) {
    if (!mqttClient.connected()) {
      reconnectMQTT();
    }
    mqttClient.loop();
  }
  
  unsigned long currentMillis = millis();
  
  // Umschalten der Lüfter-Modi (Fan-Paare)
  if (currentMillis - lastSwitchTime >= switchInterval) {
    Serial.println("Wechsel der Lüftermodi...");
    for (int i = 0; i < pairCount; i++) {
      int clientA = pairs[i].clientA;
      int clientB = pairs[i].clientB;
      if (!pairs[i].state) {
        sendClientCommand(clientA, "FANIN");
        sendClientCommand(clientB, "FANOUT");
      } else {
        sendClientCommand(clientA, "FANOUT");
        sendClientCommand(clientB, "FANIN");
      }
      pairs[i].state = !pairs[i].state;
    }
    lastSwitchTime = currentMillis;
  }
  
  // Sensor-Polling: Zyklisch einen Client abfragen
  if (currentMillis - lastSensorPollTime >= sensorUpdateInterval && totalClients > 0) {
    currentPolledClient = clientList[currentPollIndex];
    Serial.println("Poll Client " + String(currentPolledClient));
    sendClientCommand(currentPolledClient, "GETTEMPALL");
    lastSensorPollTime = currentMillis;
    currentPollIndex = (currentPollIndex + 1) % totalClients;
  }
  
  processRS485();
  
  // MQTT-Status veröffentlichen (z. B. jede Sensor-Abfrage)
  if (mqttBroker.length() > 0) {
    publishStatus();
  }
}
