#include <Arduino.h>
#include "env.h"
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"
#include <ESP32Servo.h>
#include <WiFi.h>
#include "time.h"

#define LED_BUILTIN 2

// Pin Definitions
const int pumpPin = 18;           
const int ledPin = 19;            
const int soilMoisturePinA = 32;  
const int soilMoisturePinB = 35;  
const int servoPin = 14;          

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0; 
const int daylightOffset_sec = 3600; 

const unsigned long pumpDuration = 5000; 
const unsigned long servoDelay = 1000; 

// Define constants for servo positions
const int SERVO_POS_A = 0; // Position for Plant A
const int SERVO_POS_B = 1; // Position for Plant B

Servo servo;
FirebaseData fbdo, fbdo_s1, fbdo_s2, fbdo_s3;
FirebaseAuth auth;
FirebaseConfig config;

bool signupOK = false;
bool pumpStatus = false;
unsigned long previousMillis = 0; 
unsigned long sendDataPrevMillis = 0;

int moistureThreshold = 50;
int servoPos = -1; // Default position (none)

// Function prototypes
void sendSensorData();
void handleFirebaseStreams();
int convertToPercent(int analogValue);
void manualControl();
void waterPlant(int plant);
void manageWatering();
void pump(bool state);

void setup() {
    Serial.begin(115200);
    WiFi.begin(ssid, password);
    Serial.print("Connecting to WiFi");
    while (WiFi.status() != WL_CONNECTED) {
        Serial.print(".");
        digitalWrite(LED_BUILTIN, HIGH);
        delay(300);
    }
    digitalWrite(LED_BUILTIN, LOW);
    Serial.println();
    Serial.print("Connected with IP: ");
    Serial.println(WiFi.localIP());
    Serial.println();

    config.api_key = API_KEY;
    config.database_url = DATABASE_URL;
    if (Firebase.signUp(&config, &auth, "", "")) {
        Serial.println("signupOK");
        signupOK = true;
    } else {
        Serial.printf("%s\n", config.signer.signupError.message.c_str());
    }

    config.token_status_callback = tokenStatusCallback;
    Firebase.begin(&config, &auth);
    Firebase.reconnectWiFi(true);

    if (!Firebase.RTDB.beginStream(&fbdo_s1, "/userInput/servoPos"))
        Serial.printf("Stream 1 begin error, %s\n\n", fbdo_s1.errorReason().c_str());
    if (!Firebase.RTDB.beginStream(&fbdo_s2, "/userInput/pumpState"))
        Serial.printf("Stream 2 begin error, %s\n\n", fbdo_s2.errorReason().c_str());
    if (!Firebase.RTDB.beginStream(&fbdo_s3, "userInput/moistureTreshold"))
        Serial.printf("Stream 3 begin error, %s\n\n", fbdo_s3.errorReason().c_str());

    configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
    pinMode(pumpPin, OUTPUT);
    pinMode(ledPin, OUTPUT);
    digitalWrite(pumpPin, HIGH);
    servo.attach(servoPin);
}

void loop() {
    if (Firebase.ready() && signupOK && (millis() - sendDataPrevMillis > 5000 || sendDataPrevMillis == 0)) {
        sendDataPrevMillis = millis();
        sendSensorData();
    }

    if (Firebase.ready() && signupOK) {
        handleFirebaseStreams();
    }

    int humidityValA = convertToPercent(analogRead(soilMoisturePinA));
    int humidityValB = convertToPercent(analogRead(soilMoisturePinB));

    manualControl();

    if (humidityValA < moistureThreshold) {
        waterPlant(SERVO_POS_A);
    } else if (humidityValB < moistureThreshold) {
        waterPlant(SERVO_POS_B);
    } else {
        pump(pumpStatus);
    }

    manageWatering();
}

void sendSensorData() {
    int humidityValA = convertToPercent(analogRead(soilMoisturePinA));
    int humidityValB = convertToPercent(analogRead(soilMoisturePinB));

    if (Firebase.RTDB.setInt(&fbdo, "Sensor/SoilMoisture-A", humidityValA)) {
        Serial.println("Soil Moisture A: " + String(humidityValA));
    } else {
        Serial.println("Failed to save Soil Moisture A: " + fbdo.errorReason());
    }
    if (Firebase.RTDB.setInt(&fbdo, "Sensor/SoilMoisture-B", humidityValB)) {
        Serial.println("Soil Moisture B: " + String(humidityValB));
    } else {
        Serial.println("Failed to save Soil Moisture B: " + fbdo.errorReason());
    }

    unsigned long currentTime = time(nullptr);
    if (Firebase.RTDB.setInt(&fbdo, "thingStat/lastSeen", currentTime)) {
        Serial.println("Sent heartbeat to Firebase");
    } else {
        Serial.println("Failed to send heartbeat: " + fbdo.errorReason());
    }
}

void handleFirebaseStreams() {
    if (!Firebase.RTDB.readStream(&fbdo_s1)) {
        Serial.printf("Stream 1 read error, %s\n\n", fbdo_s1.errorReason().c_str());
    }
    if (fbdo_s1.streamAvailable()) {
        if (fbdo_s1.dataType() == "int") {
            servoPos = fbdo_s1.intData();
            Serial.println("Servo Position Updated: " + String(servoPos));
        }
    }

    if (!Firebase.RTDB.readStream(&fbdo_s2)) {
        Serial.printf("Stream 2 read error, %s\n\n", fbdo_s2.errorReason().c_str());
    }
    if (fbdo_s2.streamAvailable()) {
        if (fbdo_s2.dataType() == "boolean") {
            pumpStatus = fbdo_s2.boolData();
            Serial.println("Pump Status Updated: " + String(pumpStatus));
            pump(pumpStatus);
        }
    }

    if (!Firebase.RTDB.readStream(&fbdo_s3)) {
        Serial.printf("Stream 3 read error, %s\n\n", fbdo_s3.errorReason().c_str());
    }
    if (fbdo_s3.streamAvailable()) {
        if (fbdo_s3.dataType() == "int") {
            moistureThreshold = fbdo_s3.intData();
            Serial.println("Moisture Threshold Updated: " + String(moistureThreshold));
        }
    }
}

int convertToPercent(int analogValue) {
    return map(analogValue, 0, 4095, 100, 0);
}

void manualControl() {
    if (servoPos == SERVO_POS_A) {
        servo.write(0);
    } else if (servoPos == SERVO_POS_B) {
        servo.write(180);
    }
}

void waterPlant(int plant) {
    servoPos = plant;
    pump(true);
    previousMillis = millis();
}

void manageWatering() {
    unsigned long currentMillis = millis();
    if (currentMillis - previousMillis >= pumpDuration) {
        pump(false);
        previousMillis = currentMillis;
    }
}

void pump(bool state) {
    digitalWrite(pumpPin, state ? LOW : HIGH);
    digitalWrite(ledPin, state ? LOW : HIGH);
    pumpStatus = state;
}
   