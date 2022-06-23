#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ThingsBoard.h>
#include <ArduinoJson.h>
#include <DHT.h> 
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <WebSerial.h>
#include <Adafruit_ADS1X15.h>
#include <Wire.h>
#include <ESP_Mail_Client.h>
#include <Arduino.h>
#include <EEPROM.h>

#define WIFI_AP        
#define WIFI_PASSWORD  

#define TOKEN          
#define DEVICE_NAME    

#define SMTP_HOST      
#define SMTP_PORT      
#define AUTHOR_EMAIL   
#define AUTHOR_PASSWORD
#define RECIPIENT_EMAIL

#define DHTTYPE             DHT22

// PINOUTs
Adafruit_ADS1115 ads;	
const int PUMP_RELAY_PIN = 12;
const int DHT_PIN = 14;
const int SCL_PIN = 5;
const int SDA_PIN = 4;
const int MOISTURE_TRANSISTOR_PIN = 13;

/* The SMTP Session object used for Email sending */
SMTPSession smtp;
/* Callback function to get the Email sending status */
void smtpCallback(SMTP_Status status);
bool mailSent = false;

// Time variables
WiFiUDP ntpUDP;
const long utcOffsetInSeconds = 7200;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);
int lastSyncHour = 0;
int hourOfTheLastWatering = 0;
int minuteOfSystemStart = 0;
String timeOfTheLastWatering = "";
bool systemStartTimeSent = false; 

// Thingsboard
char thingsboardServer[] = "107.173.15.229";
// int thingsboardPort = 1883; was used for PubSubClient
WiFiClient wifiClient;
ThingsBoard tb(wifiClient);

// Watering variables
const int EEPROM_WATERING_TIME_ADDRESS = 1;
const int EEPROM_AUTO_WATERING_FLAG_ADDRESS = 20;
const int EEPROM_HOUR_OF_THE_LAST_WATERING_ADDRESS = 40;
const int EEPROM_TIME_OF_THE_LAST_WATERING_ADDRESS = 60;
long wateringTimeInSeconds = 0;
bool triggerManuallyWateringFlag = false;
bool triggerAutomaticallyWateringFlag = false;

// WIFI Serial
AsyncWebServer serialServer(80);

// Sensors
const int moistureAirValue = 6700;
const int moistureWaterValue = 4800;
int soilMoisturePercent1 = 0;
int soilMoisturePercent2 = 0;
int waterLevelMoisturePercent = 0;
float temperature = 0;
float humidity = 0;
float current = 0;
float batteryVoltage = 0;

// Misc
DHT dht(DHT_PIN, DHTTYPE);

void setup() {
  Serial.begin(115200);
  dht.begin();
  ads.begin();
  pinMode(MOISTURE_TRANSISTOR_PIN, OUTPUT);
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(MOISTURE_TRANSISTOR_PIN, LOW); // turn off power for moisture sensors
  digitalWrite(PUMP_RELAY_PIN, HIGH); // stop pump manually without using stopPump(), because we use there WebSerial with WIFI

  readFromEeprom();
  
  delay(10);
  WebSerial.begin(&serialServer);
  serialServer.begin();
  delay(100);
  reconnect();
  timeClient.update();
  minuteOfSystemStart = timeClient.getMinutes();
}

void loop() {
  if (!tb.connected()) {
    reconnect();
  }
  tb.loop();

  synchroniseTime();
  measureSensorValues();
  sendTelemetry();

  if (triggerAutomaticallyWateringFlag && needsWatering() && !lowWaterLevel()) {
    waterPlants();
    mailSent = false;
  }

  if (lowWaterLevel() && mailSent == false && timeToSendMail()) {
    sendMail();
    mailSent = true;
  }
  delay(500);
  
  if (isNight() || lowBattery() || moreThanFiveMinutesFromSystemStart()) {
    WebSerial.println("Going deep sleep");
    tb.sendTelemetryBool("deepSleep", true);
    delay(500);
    ESP.deepSleep(5 * 60e6); // 5 minutes 
  }
  else {
    tb.sendTelemetryBool("deepSleep", false);
    delay(10 * 1000); // 10 seconds
  }
}

/* --------------- WATERING --------------- */
bool needsWatering() {
  // trigger watering at 17:00 and 18:00
  if (((hourNow() == 18 || hourNow() == 17) && hoursSinceLastWatering() > 0) || triggeredManually()) {
    triggerManuallyWateringFlag = false;
    return true;
  } else {
    return false;
  }
}

int hoursSinceLastWatering() {
  return abs(hourNow() -  hourOfTheLastWatering);
}

bool moreThanFiveMinutesFromSystemStart() {
  return abs(timeClient.getMinutes() - minuteOfSystemStart) > 4;
}

bool triggeredManually() {
  return triggerManuallyWateringFlag;
}

void waterPlants() {
  if (triggeredManually()) {
    WebSerial.println("Watering triggered manually");
  } else {
    WebSerial.println("Watering triggered by time");
  } 
  long startWateringTime = millis();
  startPump();
  while (millis() - startWateringTime < wateringTimeInSeconds * 1000) {
    // TODO cancel watering?
    yield();
  }

  stopPump();

  hourOfTheLastWatering = hourNow();
  timeOfTheLastWatering = timeNow();
  EEPROM.put(EEPROM_HOUR_OF_THE_LAST_WATERING_ADDRESS, hourOfTheLastWatering);
  EEPROM.put(EEPROM_TIME_OF_THE_LAST_WATERING_ADDRESS, timeOfTheLastWatering);
  EEPROM.commit();
}

void startPump() {
  WebSerial.print(timeNow());
  WebSerial.println(": starting pump...");
  digitalWrite(PUMP_RELAY_PIN, LOW);
}

void stopPump() {
  WebSerial.print(timeNow());
  WebSerial.println(": stopping pump...");
  digitalWrite(PUMP_RELAY_PIN, HIGH);  
}


/* --------------- SENSORS --------------- */
void measureSensorValues() {
  temperature = dht.readTemperature();
  delay(100);
  humidity = dht.readHumidity();
  delay(100);
  measureBatteryVoltage();
  delay(5);
  measureMoistureSensors();
}

void measureMoistureSensors() {
  // turn on power for moisture sensors
  digitalWrite(MOISTURE_TRANSISTOR_PIN, HIGH);
  delay(100);
  // sensor voltage reduced from 5v to 3.3v using voltage divider (15k Ohm and 7,5k Ohm)
  int16_t adcValueSoilMoisture1 = ads.readADC_SingleEnded(3);
  delay(5);
  int16_t adcValueSoilMoisture2 = ads.readADC_SingleEnded(2);
  delay(5);
  int16_t adcValueWaterLevel = ads.readADC_SingleEnded(1);
  // turn off power for moisture sensors
  digitalWrite(MOISTURE_TRANSISTOR_PIN, LOW);

  soilMoisturePercent1 = map(adcValueSoilMoisture1, moistureAirValue-100, moistureWaterValue, 0, 100);
  soilMoisturePercent2 = map(adcValueSoilMoisture2, moistureAirValue-200, moistureWaterValue-200, 0, 100);
  waterLevelMoisturePercent = map(adcValueWaterLevel, moistureAirValue, moistureWaterValue, 0, 100);
}

void measureBatteryVoltage() {
  float calibration = -0.13;
  float refVoltage = 4.8;
  int adcValue = analogRead(A0);
  delay(5);
  float adcVoltage  = (adcValue * refVoltage) / 1024.0; 
  batteryVoltage = adcVoltage + calibration;
}

/* --------------- MQQT --------------- */

RPC_Response manuallywateringButtonTriggered(const RPC_Data &data) {
  WebSerial.println("Received manually watering trigger from server");
  triggerManuallyWateringFlag = true;
  return RPC_Response(NULL, triggerManuallyWateringFlag);
}

RPC_Response autoWateringButtonTriggered(const RPC_Data &data) {
  WebSerial.println("Received automatically watering trigger from server");
  WebSerial.print("Current autoWatering flag: ");
  WebSerial.print(triggerAutomaticallyWateringFlag);
  triggerAutomaticallyWateringFlag = !triggerAutomaticallyWateringFlag;
  WebSerial.print(", new autoWatering flag: ");
  WebSerial.println(triggerAutomaticallyWateringFlag);
  int eepromAutoWatering;
  if (triggerAutomaticallyWateringFlag) {
    eepromAutoWatering = 1;
  } else {
    eepromAutoWatering = 0;
  }
  EEPROM.put(EEPROM_AUTO_WATERING_FLAG_ADDRESS, eepromAutoWatering);
  EEPROM.commit();
  delay(5);
  return RPC_Response(NULL, triggerAutomaticallyWateringFlag);
}

RPC_Response setWateringTimeInSeconds(const RPC_Data &data) {
  int wateringTimeInSecondsFromRequest = data;
  WebSerial.print("Received watering time: ");
  WebSerial.println(wateringTimeInSecondsFromRequest);
  wateringTimeInSeconds = wateringTimeInSecondsFromRequest;
  EEPROM.put(EEPROM_WATERING_TIME_ADDRESS, wateringTimeInSeconds);
  EEPROM.commit();
  delay(5);
  return RPC_Response(NULL, wateringTimeInSecondsFromRequest);
  delay(5);
}

const size_t callbacks_size = 3;
RPC_Callback callbacks[callbacks_size] = {
  { "manuallywateringButtonTriggered", manuallywateringButtonTriggered },
  { "autoWateringButtonTriggered", autoWateringButtonTriggered },
  { "setWateringTimeInSeconds", setWateringTimeInSeconds }
};

/* --------------- MISC --------------- */

void readFromEeprom() {
  EEPROM.begin(200);
  // Locations that have never been written to have the value of 255.
  int wateringTimeReadFromEeprom;
  EEPROM.get(EEPROM_WATERING_TIME_ADDRESS, wateringTimeReadFromEeprom);
  if (wateringTimeReadFromEeprom != 255) {
    wateringTimeInSeconds = wateringTimeReadFromEeprom;
  }

  int autoWatering;
  EEPROM.get(EEPROM_AUTO_WATERING_FLAG_ADDRESS, autoWatering);
  if (autoWatering != 255) {
    if (autoWatering == 0) {
      triggerAutomaticallyWateringFlag = false;
    } else if (autoWatering == 1) {
      triggerAutomaticallyWateringFlag = true;
    }
  }

  EEPROM.get(EEPROM_HOUR_OF_THE_LAST_WATERING_ADDRESS, hourOfTheLastWatering);
  EEPROM.get(EEPROM_TIME_OF_THE_LAST_WATERING_ADDRESS, timeOfTheLastWatering);
}

void sendMail() {
  /* Set the callback function to get the sending results */
  smtp.callback(smtpCallback);
  /* Declare the session config data */
  ESP_Mail_Session session;

  /* Set the session config */
  session.server.host_name = SMTP_HOST;
  session.server.port = SMTP_PORT;
  session.login.email = AUTHOR_EMAIL;
  session.login.password = AUTHOR_PASSWORD;
  session.login.user_domain = "";

  /* Declare the message class */
  SMTP_Message message;

  /* Set the message headers */
  message.sender.name = "Watering system";
  message.sender.email = AUTHOR_EMAIL;
  message.subject = "Low water level";
  message.addRecipient("Servant", RECIPIENT_EMAIL);

  //Send raw text message
  String textMsg = "Please refill the water tank";
  message.text.content = textMsg.c_str();
  message.text.charSet = "us-ascii";
  message.text.transfer_encoding = Content_Transfer_Encoding::enc_7bit;
  
  message.priority = esp_mail_smtp_priority::esp_mail_smtp_priority_low;
  message.response.notify = esp_mail_smtp_notify_success | esp_mail_smtp_notify_failure | esp_mail_smtp_notify_delay;
  smtp.setSystemTime(timeClient.getEpochTime());

  /* Connect to server with the session config */
  if (!smtp.connect(&session))
    return;

  /* Start sending Email and close the session */
  if (!MailClient.sendMail(&smtp, &message)) {
    WebSerial.println("Error sending Email, " + smtp.errorReason());
  }
}

/* Callback function to get the Email sending status */
void smtpCallback(SMTP_Status status) {
  /* Print the current status */
  Serial.println(status.info());

  /* Print the sending result */
  if (status.success()){
    Serial.println("----------------");
    ESP_MAIL_PRINTF("Message sent success: %d\n", status.completedCount());
    ESP_MAIL_PRINTF("Message sent failled: %d\n", status.failedCount());
    Serial.println("----------------\n");
    struct tm dt;

    for (size_t i = 0; i < smtp.sendingResult.size(); i++){
      /* Get the result item */
      SMTP_Result result = smtp.sendingResult.getItem(i);
      time_t ts = (time_t)result.timestamp;
      localtime_r(&ts, &dt);

      ESP_MAIL_PRINTF("Message No: %d\n", i + 1);
      ESP_MAIL_PRINTF("Status: %s\n", result.completed ? "success" : "failed");
      ESP_MAIL_PRINTF("Date/Time: %d/%d/%d %d:%d:%d\n", dt.tm_year + 1900, dt.tm_mon + 1, dt.tm_mday, dt.tm_hour, dt.tm_min, dt.tm_sec);
    }
    Serial.println("----------------\n");
  }
}

bool lowBattery() {
  // 3.9 for LiPo, 3.4 for LiIon
  return batteryVoltage < 3.9;
}

bool lowWaterLevel() {
  return waterLevelMoisturePercent < 20;
}

void initWiFi() {
  Serial.println("Connecting to AP ...");
  WiFi.begin(WIFI_AP, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected to AP");
  delay(5000); // needed to initalize the WebSerial
}

void reconnect() {
  // Loop until we're reconnected
  while (!tb.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
      initWiFi();
    }
    Serial.print("Connecting to ThingsBoard node ...");
    if (tb.connect(thingsboardServer, TOKEN) ) {
      Serial.println( "[DONE]" );
      Serial.println("Subscribing for RPC...");

      // Perform a subscription
      if (!tb.RPC_Subscribe(callbacks, callbacks_size)) {
        Serial.println("Failed to subscribe for RPC");
      }
    } else {
      Serial.print( "[FAILED]" );
      Serial.println( ": retrying in 5 seconds]" );
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void sendTelemetry() {
  WebSerial.println(timeNow());
  delay(200);
  /* For debugging
  WebSerial.print("Temp: ");
  WebSerial.print(temperature);
  WebSerial.print(" *C. Hum: ");
  WebSerial.print(humidity);
  WebSerial.print(" %. Batt: ");
  WebSerial.print(batteryVoltage);
  WebSerial.println(" V");
  delay(200);
  WebSerial.print("Soil moisture1: ");
  WebSerial.print(soilMoisturePercent1);
  WebSerial.print(" %, Soil moisture2: ");
  WebSerial.print(soilMoisturePercent2);
  WebSerial.print(" %, Water level: ");
  WebSerial.print(waterLevelMoisturePercent);
  WebSerial.println(" percent");
  delay(200);
  */

  if (!systemStartTimeSent) {
    char systemStartTimeCharBuf[50];
    timeNow().toCharArray(systemStartTimeCharBuf, 50);
    tb.sendTelemetryString("systemStartTime", systemStartTimeCharBuf);
    systemStartTimeSent = true;
  }

  tb.sendTelemetryFloat("temperature", temperature);
  tb.sendTelemetryFloat("humidity", humidity);
  tb.sendTelemetryFloat("batteryVoltage", batteryVoltage);
  tb.sendTelemetryInt("soilMoisturePercent1", soilMoisturePercent1);
  tb.sendTelemetryInt("soilMoisturePercent2", soilMoisturePercent2);
  tb.sendTelemetryInt("waterLevelMoisturePercent", waterLevelMoisturePercent);
  tb.sendTelemetryInt("wateringTimeInSeconds", wateringTimeInSeconds);
  tb.sendTelemetryBool("triggerAutomaticallyWateringFlag", triggerAutomaticallyWateringFlag);
  char updateTimeCharBuf[50];
  timeNow().toCharArray(updateTimeCharBuf, 50);
  tb.sendTelemetryString("updateTime", updateTimeCharBuf);
  char timeOfTheLastWateringCharBuf[50];
  timeOfTheLastWatering.toCharArray(timeOfTheLastWateringCharBuf, 50);
  tb.sendTelemetryString("timeOfTheLastWatering", timeOfTheLastWateringCharBuf);
  delay(200);
  WebSerial.println("Telemetry sent.");
}

/* --------------- TIME MANAGEMENT --------------- */

void synchroniseTime() {
  int hoursSinceLastSync = abs(timeClient.getHours() - lastSyncHour);
  /*
  Used to debug:
  WebSerial.print("hours since last sync: ");
  WebSerial.println(hoursSinceLastSync);
  WebSerial.println(timeNow());
    */
  if (hoursSinceLastSync > 10) {
    WebSerial.println("Time synced");
    timeClient.update();
    lastSyncHour = timeClient.getHours();
  }
}

int hourNow() {
  return timeClient.getHours();
}

String timeNow() {
  return timeClient.getFormattedTime();
}

bool isNight() {
  return hourNow() < 8 || hourNow() > 19;
}

bool timeToSendMail() {
  return hourNow() == 14 && timeClient.getMinutes() > 0 && timeClient.getMinutes() < 10;
}
