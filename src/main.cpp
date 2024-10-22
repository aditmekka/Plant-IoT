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
const int pumpPin = 18;           // Digital pin for pump relay
const int ledPin = 19;            // Digital pin for led relay
const int soilMoisturePinA = 32;  // Analog pin for soil moisture sensor A (Plant A)
const int soilMoisturePinB = 35;  // Analog pin for soil moisture sensor B (Plant B)
const int servoPin = 14;          // Digital pin for servo motor

const char* ntpServer = "pool.ntp.org";
const long gmtOffset_sec = 0; // Adjust based on your timezone
const int daylightOffset_sec = 3600; // Adjust based on daylight savings


int servoPos;
int humidityValA;
int humidityValB;
int interval = 5000;
int moistureTreshold = 50;

Servo servo;
FirebaseData fbdo, fbdo_s1, fbdo_s2, fbdo_s3;
FirebaseAuth auth;
FirebaseConfig config;

bool signupOK = false;
bool pumpStatus = false;
bool pumpState = false;
unsigned long previousMillis = 0; // For non-blocking delay
unsigned long sendDataPrevMillis = 0;
unsigned long getDataCurrentMillis = 0;
const unsigned long pumpDuration = 5000; // Pump on duration
const unsigned long servoDelay = 1000; // Delay for servo
unsigned long servoStartMillis = 0;
bool isWateringA = false;
bool isWateringB = false;

int convertToPercent(int analogValue){
  return map(analogValue, 0, 4095, 100, 0);
}

void manualControl();
void serialDebug();
void pump(bool state);
void waterPlantA();
void waterPlantB();

void setup(){
  Serial.begin(115200);
  WiFi.begin(ssid, password);
  Serial.print("Connecting to WiFi");
  while(WiFi.status() != WL_CONNECTED){
    Serial.print(".");
    digitalWrite(LED_BUILTIN, LOW);
    delay(300);
  }
  Serial.println();
  Serial.print("Connected with IP: ");
  Serial.println(WiFi.localIP());
  Serial.println();

  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  if(Firebase.signUp(&config, &auth, "", "")){
    Serial.println("signupOK");
    signupOK = true;
  }else{
    Serial.printf("%s\n", config.signer.signupError.message.c_str());
  }

  config.token_status_callback = tokenStatusCallback;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if(!Firebase.RTDB.beginStream(&fbdo_s1, "/userInput/servoPos"))
    Serial.printf("Stream 1 begin error, %s\n\n", fbdo_s1.errorReason().c_str());
  if(!Firebase.RTDB.beginStream(&fbdo_s2, "/userInput/pumpState"))
    Serial.printf("Stream 2 begin error, %s\n\n", fbdo_s2.errorReason().c_str());
  if(!Firebase.RTDB.beginStream(&fbdo_s3, "userInput/moistureTreshold"))
    Serial.printf("Stream 3 begin error, %s\n\n", fbdo_s3.errorReason().c_str());

  configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);

  // Test the time synchronization
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
      Serial.println("Failed to obtain time");
      return;
  }
  Serial.println(&timeinfo, "Time initialized: %A, %B %d %Y %H:%M:%S");

  pinMode(pumpPin, OUTPUT);
  pinMode(ledPin, OUTPUT);
  digitalWrite(pumpPin, HIGH);

  servo.attach(servoPin);
}

void loop(){
  if(Firebase.ready() && signupOK && (millis() - sendDataPrevMillis > 5000 || sendDataPrevMillis == 0)){
    sendDataPrevMillis = millis();
    if(Firebase.RTDB.setInt(&fbdo, "Sensor/SoilMoisture-A", humidityValA)){
      Serial.println(); Serial.print(humidityValA);
      Serial.print(" - successfully saved to: " + fbdo.dataPath());
      Serial.println(" (" + fbdo.dataType() + ") ");
    }else{
      Serial.println("FAILED: " + fbdo.errorReason());
    }

    if(Firebase.RTDB.setInt(&fbdo, "Sensor/SoilMoisture-B", humidityValB)){
      Serial.print(humidityValB);
      Serial.print(" - successfully saved to: " + fbdo.dataPath());
      Serial.println(" (" + fbdo.dataType() + ") ");
    }else{
      Serial.println("FAILED: " + fbdo.errorReason());
    }

    unsigned long currentTime = time(nullptr);
    if(Firebase.RTDB.setInt(&fbdo , "thingStat/lastSeen", currentTime)){
      Serial.println("Sent heartbeat to Firebase");
    } else {
      Serial.println("Failed to send heartbeat: " + fbdo.errorReason());
    }
  }

  if(Firebase.ready() && signupOK){
    if(!Firebase.RTDB.readStream(&fbdo_s1))
      Serial.printf("Stream 1 read error, %s\n\n", fbdo_s1.errorReason().c_str());
    if(fbdo_s1.streamAvailable()){
      if(fbdo_s1.dataType() == "int"){
        servoPos = fbdo_s1.intData();
        Serial.println("Successfull READ from " + fbdo_s1.dataPath() + ": " + servoPos + " (" + fbdo_s1.dataType() + ")");
      }
    }

    if(!Firebase.RTDB.readStream(&fbdo_s2))
      Serial.printf("Stream 2 read error, %s\n\n", fbdo_s2.errorReason().c_str());
    if(fbdo_s2.streamAvailable()){
      if(fbdo_s2.dataType() == "boolean"){
        pumpStatus = fbdo_s2.boolData();
        Serial.println("Successfull READ from " + fbdo_s2.dataPath() + ": " + pumpStatus + " (" + fbdo_s2.dataType() + ")");
        pump(pumpStatus);
      }
    }

    if(!Firebase.RTDB.readStream(&fbdo_s3))
      Serial.printf("Stream 3 read error, %s\n\n", fbdo_s3.errorReason().c_str());
    if(fbdo_s3.streamAvailable()){
      if(fbdo_s3.dataType() == "int"){
        moistureTreshold = fbdo_s3.intData();
        Serial.println("Successfull READ from " + fbdo_s3.dataPath() + ": " + moistureTreshold + " (" + fbdo_s3.dataType() + ")");
      }
    }
  }

  humidityValA = convertToPercent(analogRead(soilMoisturePinA));
  humidityValB = convertToPercent(analogRead(soilMoisturePinB));

  manualControl();
  //serialDebug();

  if(humidityValA < moistureTreshold){
    waterPlantA();
  }else if(humidityValB < moistureTreshold){
    waterPlantB();
  }

  if(isWateringA || isWateringB){
    unsigned long currentMillis = millis();
    if(isWateringA && currentMillis - servoStartMillis >= servoDelay){
      servo.write(0);
      servoStartMillis = millis();
      isWateringA = false;
    }else if(isWateringB && currentMillis - servoStartMillis >= servoDelay){
      servo.write(180);
      servoStartMillis = millis();
      isWateringB = false;
    }

    if(currentMillis - previousMillis >= pumpDuration){
      pump(false);
      previousMillis = currentMillis;
    }
  }
}

void manualControl(){
  if(servoPos == 0){
    servo.write(0);
  }else if(servoPos == 1){
    servo.write(180);
  }
}

void serialDebug(){
  Serial.print("Plant A Moisture: ");
  Serial.print(humidityValA);
  Serial.println("%");

  Serial.print("Plant B Moisture: ");
  Serial.print(humidityValB);
  Serial.println("%");
}

void waterPlantA(){
  servoStartMillis = millis();
  isWateringA = true;
  pump(true);
  previousMillis = millis();
}

void waterPlantB(){
  servoStartMillis = millis();
  isWateringB = true;
  pump(true);
  previousMillis = millis();
}

void pump(bool state){
  digitalWrite(pumpPin, state ? LOW : HIGH);
  digitalWrite(ledPin, state ? LOW : HIGH);
  pumpState = state;
}