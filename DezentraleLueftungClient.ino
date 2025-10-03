/*
  Beispielprogramm: Dezentrale Lüftungsanlage (Client)
  
  Funktion:
   - Der Client reagiert nur dann auf weitere Befehle, wenn er durch
     "ACTIVATECLIENTx" (wobei x der konfigurierte CLIENT_ID entspricht)
     aktiviert wurde. Er wird automatisch deaktiviert, wenn ein 
     "ACTIVATECLIENT" Befehl mit einer anderen ID empfangen wird.
  
  Weitere Funktionen:
   - Periodische Sensoraktualisierung (BME280, 4 Sensoren)
   - Lüftersteuerung (FANIN, FANOUT, FANSTOP)
   - Klappensteuerung per Servo (VENTOPEN, VENTCLOSE)
   - Kommunikation über RS485 (über HardwareSerial)
*/

#include <Wire.h>
#include <Adafruit_BME280.h>
#include <Servo.h>

// CLIENT_ID: Stelle hier ein, welche Clientnummer dieser Arduino ist (z. B. 1 bis 6)
#define CLIENT_ID 1

// I²C-Multiplexer (TCA9548A)
#define TCA9548A_ADDRESS 0x70

// Anzahl der BME280-Sensoren (angeschlossen an die Kanäle 0 bis 3)
#define NUM_SENSORS 4

// Pinbelegung
#define FAN_IN_PIN      6    // Lüfter IN (FANIN) an Port 6
#define FAN_OUT_PIN     9    // Lüfter OUT (FANOUT) an Port 9
#define RS485_CTRL_PIN  4    // RS485-Steuerung (DI+DE) an Port 4
#define SERVO_PIN       3    // Servo (Klappensteuerung) an Port 3

Servo ventServo;         // Servo-Objekt zur Klappensteuerung
bool clientActive = false; // Status, ob dieser Client aktiv ist

// Sensorobjekte, Messwerte und Status
Adafruit_BME280 bme[NUM_SENSORS];   // Array mit 4 Sensorobjekten
float sensorTemps[NUM_SENSORS];      // Letzte Temperaturwerte
float sensorHums[NUM_SENSORS];       // Letzte Luftfeuchtigkeitswerte
bool sensorOk[NUM_SENSORS];          // Status, ob der jeweilige Sensor initialisiert wurde

// Zeitsteuerung für periodische Sensoraktualisierung
unsigned long lastSensorUpdate = 0;
const unsigned long sensorUpdateInterval = 1000;  // 1000 ms = 1 Sekunde

// Hilfsfunktion: RS485-Antwort senden
void sendResponse(String response) {
  digitalWrite(RS485_CTRL_PIN, HIGH); // RS485 in Sendemodus
  Serial.println(response);
  Serial.flush();                     // Warten, bis die Übertragung abgeschlossen ist
  digitalWrite(RS485_CTRL_PIN, LOW);  // Zurück in den Empfangsmodus
}

// Hilfsfunktion: I²C-Kanal des Multiplexers auswählen
void selectI2CChannel(uint8_t channel) {
  if (channel > 7) return;  // Es gibt nur 8 Kanäle (0 bis 7)
  Wire.beginTransmission(TCA9548A_ADDRESS);
  Wire.write(1 << channel); // Bitmaske: z.B. Kanal 0 → 0x01, Kanal 1 → 0x02 usw.
  Wire.endTransmission();
}

// Sensorwerte aktualisieren (nicht-blockierend)
void updateSensors() {
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    selectI2CChannel(i);
    if (sensorOk[i]) {
      sensorTemps[i] = bme[i].readTemperature();
      sensorHums[i]  = bme[i].readHumidity();
    }
  }
}

// Verarbeitung der empfangenen Befehle
void processCommand(String cmd) {
  cmd.trim(); // Entfernt führende/nachgestellte Leerzeichen

  // Geänderte Verarbeitung von ACTIVATECLIENT:
  // Nur wenn die im Befehl enthaltene ID mit CLIENT_ID übereinstimmt,
  // wird der Client aktiviert; ansonsten deaktiviert.
  if (cmd.startsWith("ACTIVATECLIENT")) {
    String numStr = cmd.substring(String("ACTIVATECLIENT").length());
    int num = numStr.toInt();
    if (num == CLIENT_ID) {
      clientActive = true;
      sendResponse("Client " + String(CLIENT_ID) + " aktiviert.");
    } else {
      clientActive = false;
      sendResponse("Client " + String(CLIENT_ID) + " deaktiviert.");
    }
    return;
  }
  
  // Befehl zum Deaktivieren aller Clients
  if (cmd == "DEACTIVATECLIENTS") {
    clientActive = false;
    sendResponse("Clients deaktiviert.");
    return;
  }
  
  // Reagiere nur, wenn der Client aktiv ist
  if (!clientActive) {
    return;
  }
  
  // Sensorbefehle:
  if (cmd == "GETTEMPALL") {
    String response = "";
    for (uint8_t i = 0; i < NUM_SENSORS; i++) {
      response += "Sensor" + String(i + 1) + ": ";
      if (sensorOk[i]) {
        response += "Temp=" + String(sensorTemps[i], 1) + "°C, Hum=" + String(sensorHums[i], 1) + "%; ";
      } else {
        response += "Fehler; ";
      }
    }
    sendResponse(response);
    return;
  }
  
  if (cmd.startsWith("GETTEMP")) {
    String sensorNumStr = cmd.substring(String("GETTEMP").length());
    int sensorNum = sensorNumStr.toInt();
    if (sensorNum >= 1 && sensorNum <= NUM_SENSORS) {
      float temp, hum;
      bool ok = false;
      selectI2CChannel(sensorNum - 1);
      if (sensorOk[sensorNum - 1]) {
        temp = bme[sensorNum - 1].readTemperature();
        hum  = bme[sensorNum - 1].readHumidity();
        ok = true;
      }
      String response = "Sensor" + String(sensorNum) + ": ";
      if (ok) {
        response += "Temp=" + String(temp, 1) + "°C, Hum=" + String(hum, 1) + "%";
      } else {
        response += "Fehler";
      }
      sendResponse(response);
    }
    return;
  }
  
  // Lüfterbefehle:
  if (cmd == "FANIN") {
    analogWrite(FAN_IN_PIN, 255);
    analogWrite(FAN_OUT_PIN, 0);
    sendResponse("FANIN aktiviert.");
    return;
  }
  
  if (cmd == "FANOUT") {
    analogWrite(FAN_OUT_PIN, 255);
    analogWrite(FAN_IN_PIN, 0);
    sendResponse("FANOUT aktiviert.");
    return;
  }
  
  if (cmd == "FANSTOP") {
    analogWrite(FAN_IN_PIN, 0);
    analogWrite(FAN_OUT_PIN, 0);
    sendResponse("Lüfter gestoppt.");
    return;
  }
  
  // Ventilbefehle (Servo):
  if (cmd == "VENTCLOSE") {
    ventServo.write(90);
    sendResponse("Vent geschlossen.");
    return;
  }
  
  if (cmd == "VENTOPEN") {
    ventServo.write(0);
    sendResponse("Vent geöffnet.");
    return;
  }
}

void setup() {
  // Serielle Kommunikation starten (RS485 über HardwareSerial)
  Serial.begin(9600);
  Wire.begin();
  
  pinMode(FAN_IN_PIN, OUTPUT);
  pinMode(FAN_OUT_PIN, OUTPUT);
  
  pinMode(RS485_CTRL_PIN, OUTPUT);
  digitalWrite(RS485_CTRL_PIN, LOW); // Standard: Empfangsmodus
  
  ventServo.attach(SERVO_PIN);
  ventServo.write(90); // Klappe initial geschlossen
  
  // BME280-Sensoren initialisieren (an den Kanälen 0 bis 3)
  for (uint8_t i = 0; i < NUM_SENSORS; i++) {
    selectI2CChannel(i);
    if (!bme[i].begin(0x76)) {
      sensorOk[i] = false;
    } else {
      sensorOk[i] = true;
    }
  }
  
  lastSensorUpdate = millis();
  updateSensors();
}

void loop() {
  // Sensorwerte periodisch aktualisieren (alle 1000 ms)
  if (millis() - lastSensorUpdate >= sensorUpdateInterval) {
    updateSensors();
    lastSensorUpdate = millis();
  }
  
  // Befehle über RS485 (HardwareSerial) empfangen
  if (Serial.available() > 0) {
    String command = Serial.readStringUntil('\n');
    processCommand(command);
  }
}
