#include <NTPClient.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ThingsBoard.h>
#include <ArduinoJson.h>
#include <DHT.h> 
#include <Adafruit_ADS1X15.h>
#include <Wire.h>
#include <ESP_Mail_Client.h>
#include <Arduino.h>
#include <EEPROM.h>
// display
#include <SPI.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_GFX.h>



#define DHTTYPE             DHT22

#define OLED_WIDTH          128
#define OLED_HEIGHT         64
#define OLED_RESET          -1
#define SCREEN_ADDRESS      0x3C

Adafruit_SSD1306 display(OLED_WIDTH, OLED_HEIGHT, &Wire, OLED_RESET);
char displayArray[21][8];

// PINOUTs
const int PUMP_RELAY_PIN = 14;
const int DHT_PIN = 23;
const int SENSOR_TRANSISTOR_PIN = 27;
const int BATT_V_PIN = 35;

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
char thingsboardServer[] = "107.175.142.252";
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

// Sensors
const int moistureAirValue = 4200;
const int moistureWaterValue = 3750;
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
  display.begin(SSD1306_SWITCHCAPVCC, SCREEN_ADDRESS);
  display.display();
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(WHITE);
  
  pinMode(BATT_V_PIN, INPUT);
  pinMode(SENSOR_TRANSISTOR_PIN, OUTPUT);
  // TODO
  // pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(SENSOR_TRANSISTOR_PIN, LOW); // turn off power for moisture sensors
  // digitalWrite(PUMP_RELAY_PIN, HIGH); // stop pump manually without using stopPump(), because we use there WebSerial with WIFI

  // readFromEeprom();
  
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

  if ((triggerAutomaticallyWateringFlag && needsWatering() && !lowWaterLevel()) || triggeredManually()) {
    waterPlants();
    mailSent = false;
  }

  // TODO
  // if (lowWaterLevel() && mailSent == false && timeToSendMail()) {
  //   sendMail();
  //   mailSent = true;
  // }
  // delay(500);
  
  // if (isNight() || lowBattery()) {
  //   logPrintln("Going deep sleep");
  //   logDisplay();
  //   tb.sendTelemetryBool("deepSleep", true);
  //   delay(500);
  //   ESP.deepSleep(5 * 60e6); // 5 minutes 
  // }
  // else if (moreThanFiveMinutesFromSystemStart()) {
  //   logPrintln("Restarting");
  //   logDisplay();
  //   tb.sendTelemetryBool("deepSleep", true);
  //   delay(500);
  //   ESP.deepSleep(1e6); // 1 second 
  // }
  // else {
  //   tb.sendTelemetryBool("deepSleep", false);
  //   delay(10 * 1000); // 10 seconds
  // }
  // TODO remove delay
  delay(3000);
}

/* --------------- WATERING --------------- */
bool needsWatering() {
  // trigger watering at 8:00 and 21:00
  if (((hourNow() == 8 || hourNow() == 21) && hoursSinceLastWatering() > 0)) {
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
    logPrintln("Watering triggered manually");
  } else {
    logPrintln("Watering triggered by time");
  } 
  logDisplay();
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
  logPrint(timeNow());
  logPrintln(": starting pump...");
  logDisplay();
  digitalWrite(PUMP_RELAY_PIN, LOW);
}

void stopPump() {
  logPrint(timeNow());
  logPrintln(": stopping pump...");
  logDisplay();
  digitalWrite(PUMP_RELAY_PIN, HIGH);  
}


/* --------------- SENSORS --------------- */
void measureSensorValues() {
  // turn on power for moisture sensors
  digitalWrite(SENSOR_TRANSISTOR_PIN, HIGH);
  delay(500);
  dht.begin();
  delay(500);
  temperature = dht.readTemperature();
  delay(100);
  humidity = dht.readHumidity();
  delay(100);
  measureBatteryVoltage();
  delay(5);
  measureMoistureSensors();
  // turn off power for moisture sensors
  digitalWrite(SENSOR_TRANSISTOR_PIN, LOW);
}

void measureMoistureSensors() {
  // sensor voltage reduced from 5v to 3.3v using voltage divider (15k Ohm and 7,5k Ohm)
  /* TODO get rid of "ads":
  int16_t adcValueSoilMoisture1 = ads.readADC_SingleEnded(3);
  delay(5);
  int16_t adcValueSoilMoisture2 = ads.readADC_SingleEnded(2);
  delay(5);
  int16_t adcValueWaterLevel = ads.readADC_SingleEnded(1);

  // logPrintln(adcValueSoilMoisture1);
  // logPrintln(adcValueSoilMoisture2);
  // logPrintln(adcValueWaterLevel);
  // logDisplay();

  soilMoisturePercent1 = map(adcValueSoilMoisture1, moistureAirValue, moistureWaterValue, 0, 100);
  soilMoisturePercent2 = map(adcValueSoilMoisture2, moistureAirValue, moistureWaterValue, 0, 100);
  waterLevelMoisturePercent = map(adcValueWaterLevel, moistureAirValue+100, moistureWaterValue+130, 0, 100);
  */
}

void measureBatteryVoltage() {
  float calibration = 0.8; //-0.13;
  float refVoltage = 20.566;
  int adcValue = analogRead(BATT_V_PIN);
  delay(5);
  float adcVoltage  = refVoltage * (adcValue / 4096.0); 
  batteryVoltage = adcVoltage + calibration;
}

/* --------------- MQQT --------------- */

RPC_Response manuallywateringButtonTriggered(const RPC_Data &data) {
  logPrintln("Received manually watering trigger from server");
  logDisplay();
  triggerManuallyWateringFlag = true;
  return RPC_Response(NULL, triggerManuallyWateringFlag);
}

RPC_Response autoWateringButtonTriggered(const RPC_Data &data) {
  logPrintln("Received automatically watering trigger from server");
  logPrint("Current autoWatering flag: ");
  logPrint(triggerAutomaticallyWateringFlag);
  triggerAutomaticallyWateringFlag = !triggerAutomaticallyWateringFlag;
  logPrint(", new autoWatering flag: ");
  logPrintln(triggerAutomaticallyWateringFlag);
  logDisplay();
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
  logPrint("Received watering time: ");
  logPrintln(wateringTimeInSecondsFromRequest);
  logDisplay();
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
void logPrint(const char str[]) {
  display.print(str);
  Serial.print(str);
}

void logPrintln(const char str[]) {
  display.println(str);
  Serial.println(str);
}

void logPrint(String str) {
  display.print(str);
  Serial.print(str);
}

void logPrintln(String str) {
  display.println(str);
  Serial.println(str);
}

void logPrint(bool str) {
  display.print(str);
  Serial.print(str);
}

void logPrintln(bool str) {
  display.println(str);
  Serial.println(str);
}

void logPrint(float str) {
  display.print(str);
  Serial.print(str);
}

void logPrintln(float str) {
  display.println(str);
  Serial.println(str);
}

void logPrint(int str) {
  display.print(str);
  Serial.print(str);
}

void logPrintln(int str) {
  display.println(str);
  Serial.println(str);
}

void logDisplay() {
  display.display();
  display.clearDisplay();
  display.setCursor(0, 0);
}

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
    logPrintln("Error sending Email, " + smtp.errorReason());
    logDisplay();
  }
}

/* Callback function to get the Email sending status */
void smtpCallback(SMTP_Status status) {
  /* Print the current status */
  logPrintln(status.info());

  /* Print the sending result */
  if (status.success()){
    logPrintln("----------------");
    ESP_MAIL_PRINTF("Message sent success: %d\n", status.completedCount());
    ESP_MAIL_PRINTF("Message sent failled: %d\n", status.failedCount());
    logPrintln("----------------\n");
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
    logPrintln("----------------\n");
    logDisplay();
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
  logPrintln("Connecting to AP ...");
  WiFi.begin(WIFI_AP, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    logPrint(".");
  }
  logPrintln("Connected to AP");
  logDisplay();
  delay(5000); // needed to initalize the WebSerial
}

void reconnect() {
  // Loop until we're reconnected
  while (!tb.connected()) {
    if (WiFi.status() != WL_CONNECTED) {
      initWiFi();
    }
    logPrint("Connecting to ThingsBoard node ...");
    if (tb.connect(thingsboardServer, TOKEN) ) {
      logPrintln( "[DONE]" );
      logPrintln("Subscribing for RPC...");

      // Perform a subscription
      if (!tb.RPC_Subscribe(callbacks, callbacks_size)) {
        logPrintln("Failed to subscribe for RPC");
      }
    } else {
      logPrint( "[FAILED]" );
      logPrintln( ": retrying in 5 seconds]" );
      // Wait 5 seconds before retrying
      delay(5000);
    }
    logDisplay();
  }
}

void sendTelemetry() {
  logPrintln(timeDateNow());
  delay(200);
  /* For debugging   */
  logPrint("T: ");
  logPrint(temperature);
  logPrint("C Hum: ");
  logPrint(humidity);
  logPrintln("%");
  logPrint("Batt: ");
  logPrint(batteryVoltage);
  logPrintln("V. ");
  delay(200);
  logPrint("Soil1: ");
  logPrint(soilMoisturePercent1);
  logPrint("%, Soil2: ");
  logPrint(soilMoisturePercent2);
  logPrint("%, Water: ");
  logPrint(waterLevelMoisturePercent);
  logPrintln("%");
  delay(200);


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
  logPrintln("Telemetry sent.");
  logDisplay();
}

/* --------------- TIME MANAGEMENT --------------- */

void synchroniseTime() {
  int hoursSinceLastSync = abs(timeClient.getHours() - lastSyncHour);
  /*
  Used to debug:
  logPrint("hours since last sync: ");
  logPrintln(hoursSinceLastSync);
  logPrintln(timeNow());
    */
  if (hoursSinceLastSync > 10) {
    logPrintln("Time synced");
    timeClient.update();
    lastSyncHour = timeClient.getHours();
    logDisplay();
  }
}

int hourNow() {
  return timeClient.getHours();
}

String timeNow() {
  return timeClient.getFormattedTime();
}

String timeDateNow() {
  time_t epochTime = timeClient.getEpochTime();
  struct tm *ptm = gmtime ((time_t *)&epochTime); 

  int currentMonthDay = ptm->tm_mday;
  int currentMonth = ptm->tm_mon+1;
  int currentYear = ptm->tm_year+1900;
  return timeClient.getFormattedTime() + " " + currentMonthDay + "." + currentMonth + "." + currentYear;
}

bool isNight() {
  return hourNow() < 8 || hourNow() > 19;
}

bool timeToSendMail() {
  return hourNow() == 14 && timeClient.getMinutes() > 0 && timeClient.getMinutes() < 10;
}
