#include <NTPClient.h>
#include <ESP8266WiFi.h>
#include <WiFiUdp.h>

#define WIFI_AP             "xxxxxxxxxx"
#define WIFI_PASSWORD       "xxxxxxxxxx"

// Time variables
WiFiUDP ntpUDP;
const long utcOffsetInSeconds = 7200;
NTPClient timeClient(ntpUDP, "pool.ntp.org", utcOffsetInSeconds);
int lastSyncHour = 0;
int hourSinceLastWatering = 0;

// Watering variables
const long WATERING_TIME_IN_MS = 1000 * 60;

void setup() {
  Serial.begin(115200);

  delay(10);
  initWiFi();
  timeClient.update();
}

void loop() {
  synchroniseTime();
  Serial.println(timeNow());
  if (needsWatering()) {
    waterPlants();
  }
  delay(1000);
}

/* --------------- WATERING --------------- */
boolean needsWatering() {
  if (hourNow() == 9 && hourSinceLastWatering > 10) {
    hourSinceLastWatering = hourNow();
    return true;
  } else {
    return false;
  }
}

void waterPlants() {
  long startWateringTime = millis();
  while (millis() - startWateringTime < WATERING_TIME_IN_MS) {
    startPump
  }
}


/* --------------- MISC --------------- */

void initWiFi() {
  // TODO add reconnecting
  Serial.println("Connecting to AP ...");
  WiFi.begin(WIFI_AP, WIFI_PASSWORD);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("Connected to AP");
}


/* --------------- TIME MANAGEMENT --------------- */

void synchroniseTime() {
  int hoursSinceLastSync = abs(timeClient.getHours() - lastSyncHour);
  Serial.print("hours since last sync: ");
  Serial.println(hoursSinceLastSync);
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
