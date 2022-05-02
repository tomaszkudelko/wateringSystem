#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include <ThingsBoard.h>
#include <ArduinoJson.h>
#include <DHT.h> 



#define DHTTYPE             DHT22

// PINOUTs
const int PUMP_RELAY_PIN = 16;
const int DHT_PIN = 5;

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

// Sensors
float temperature = 0;
float humidity = 0;
float moisture = 0;
float current = 0;
float batteryVoltage = 0;

DHT dht(DHT_PIN, DHTTYPE);

void setup() {
  Serial.begin(115200);
  dht.begin();
  pinMode(PUMP_RELAY_PIN, OUTPUT);
  
  stopPump();

  delay(10);
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
  // TODO delete the delay
  delay(5000);
}

/* --------------- WATERING --------------- */
boolean needsWatering() {
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
  Serial.print(timeNow());
  Serial.println(": starting pump...");
  digitalWrite(PUMP_RELAY_PIN, HIGH);
}

void stopPump() {
  Serial.print(timeNow());
  Serial.println(": stopping pump...");
  digitalWrite(PUMP_RELAY_PIN, LOW);  
}


/* --------------- SENSORS --------------- */
void measureSensorValues() {
  temperature = dht.readTemperature();
  humidity = dht.readHumidity();
}

/* --------------- MQQT --------------- */

RPC_Response wateringButtonTriggered(const RPC_Data &data) {
  Serial.println("Received watering trigger from server");
  triggerManuallyWateringFlag = true;
  return RPC_Response(NULL, triggerManuallyWateringFlag);
}

const size_t callbacks_size = 1;
RPC_Callback callbacks[callbacks_size] = {
  { "watering_switch", wateringButtonTriggered }
};

/* --------------- MISC --------------- */

void initWiFi() {
  Serial.println("Connecting to AP ...");
  WiFi.begin(WIFI_AP, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected to AP");
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
  tb.sendTelemetryFloat("temperature", temperature);
  tb.sendTelemetryFloat("humidity", humidity);

  Serial.print("Temp: ");
  Serial.println(temperature);
  Serial.print("Hum: ");
  Serial.println(humidity);
  Serial.println("Telemetry sent.");
}

/* --------------- TIME MANAGEMENT --------------- */

void synchroniseTime() {
  int hoursSinceLastSync = abs(timeClient.getHours() - lastSyncHour);
  /*
  Used to debug:
  Serial.print("hours since last sync: ");
  Serial.println(hoursSinceLastSync);
  Serial.println(timeNow());
    */
  if (hoursSinceLastSync > 10) {
    Serial.println("Time synced");
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
