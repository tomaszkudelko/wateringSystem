#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ThingsBoard.h>
#include <ArduinoJson.h>
#include <DHT.h> 
#include <ESPAsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <WebSerial.h>



#define DHTTYPE             DHT22

// PINOUTs
const int PUMP_RELAY_PIN = 5;
const int DHT_PIN = 4;
const int MOISTURE_PIN = A0;

// Time variables
WiFiUDP ntpUDP;
const long utcOffsetInSeconds = 7200;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);
int lastSyncHour = 0;
int hourSinceLastWatering = 0;

// Thingsboard
char thingsboardServer[] = "107.173.15.229";
// int thingsboardPort = 1883; was used for PubSubClient
WiFiClient wifiClient;
ThingsBoard tb(wifiClient);

// Watering variables
// TODO change to 60s
const long WATERING_TIME_IN_MS = 1000 * 5;
boolean triggerManuallyWateringFlag = false;

// WIFI Serial
AsyncWebServer serialServer(80);

// Sensors
const int moistureAirValue = 620;
const int moistureWaterValue = 310;
int moistureValue = 0;
int moisturePercent = 0;
float temperature = 0;
float humidity = 0;
float current = 0;
float batteryVoltage = 0;

DHT dht(DHT_PIN, DHTTYPE);

void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  digitalWrite(PUMP_RELAY_PIN, HIGH); // stop pump manually without using stopPump(), because we use there WebSerial with WIFI
  
  delay(10);
  WebSerial.begin(&serialServer);
  serialServer.begin();
  delay(100);
  reconnect();
  timeClient.update();
}

void loop() {
  if (!tb.connected()) {
    reconnect();
  }
  tb.loop();

  synchroniseTime();
  measureSensorValues();
  sendTelemetry();

  if (needsWatering()) {
    waterPlants();
  }
  delay(500);
  if (isNight() || lowBattery()) {
    WebSerial.println("Going deep sleep");
    tb.sendTelemetryBool("deepSlep", true);
    delay(500);
    ESP.deepSleep(5 * 60e6); // 5 minutes 
  }
  else {
    tb.sendTelemetryBool("deepSlep", false);
    delay(5000);
  }
}

/* --------------- WATERING --------------- */
boolean needsWatering() {
  // trigger watering at 9 AM
  if ((hourNow() == 9 && hourSinceLastWatering > 10) || triggeredManually()) {
    triggerManuallyWateringFlag = false;
    hourSinceLastWatering = hourNow();
    return true;
  } else {
    return false;
  }
}

boolean triggeredManually() {
  return triggerManuallyWateringFlag;
}

void waterPlants() {
  long startWateringTime = millis();
  startPump();
  while (millis() - startWateringTime < WATERING_TIME_IN_MS) {
    // TODO cancel watering?
    yield();
  }
  stopPump();
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
  humidity = dht.readHumidity();
  /* TODO
  moistureValue = analogRead(A0);
  WebSerial.print("Moisture: ");
  WebSerial.println(analogRead(MOISTURE_PIN));
  moisturePercent = map(moistureValue, moistureAirValue, moistureWaterValue, 0, 100);
  */
  delay(5);
  measureBatteryVoltage();
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

RPC_Response wateringButtonTriggered(const RPC_Data &data) {
  WebSerial.println("Received watering trigger from server");
  triggerManuallyWateringFlag = true;
  return RPC_Response(NULL, triggerManuallyWateringFlag);
}

const size_t callbacks_size = 1;
RPC_Callback callbacks[callbacks_size] = {
  { "watering_switch", wateringButtonTriggered }
};

/* --------------- MISC --------------- */

boolean lowBattery() {
  return batteryVoltage < 3.2;
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
    WebSerial.print("Connecting to ThingsBoard node ...");
    if (tb.connect(thingsboardServer, TOKEN) ) {
      WebSerial.println( "[DONE]" );
      WebSerial.println("Subscribing for RPC...");

      // Perform a subscription
      if (!tb.RPC_Subscribe(callbacks, callbacks_size)) {
        WebSerial.println("Failed to subscribe for RPC");
      }
    } else {
      WebSerial.print( "[FAILED]" );
      WebSerial.println( ": retrying in 5 seconds]" );
      // Wait 5 seconds before retrying
      delay(5000);
    }
  }
}

void sendTelemetry() {
  WebSerial.println(timeNow());
  WebSerial.print("Temp: ");
  WebSerial.print(temperature);
  WebSerial.print(" *C. Hum: ");
  WebSerial.print(humidity);
  WebSerial.print(" %. Batt: ");
  WebSerial.print(batteryVoltage);
  WebSerial.println(" V.");

  tb.sendTelemetryFloat("temperature", temperature);
  tb.sendTelemetryFloat("humidity", humidity);
  tb.sendTelemetryFloat("batteryVoltage", batteryVoltage);
  char timeCharBuf[50];
  timeNow().toCharArray(timeCharBuf, 50);
  tb.sendTelemetryString("requestTime", timeCharBuf);
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

boolean isNight() {
  return hourNow() < 6 || hourNow() > 21;
}
