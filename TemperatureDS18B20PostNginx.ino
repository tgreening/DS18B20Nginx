// Import required libraries
//#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <WiFiManager.h>         //https://github.com/tzapu/WiFiManager
#include <FS.h>                   //this needs to be first, or it all crashes and burns...
#include <ArduinoJson.h>          //https://github.com/bblanchon/ArduinoJson
#include <ArduinoOTA.h>


// WiFi parameters
const char* host = "house_temp";
const char* probe_name;
const char* delay_minutes;
const char* server;
HTTPClient http;
OneWire oneWire(D4);

DallasTemperature ds18b20(&oneWire);

//Variables
float temp; //Stores temperature value

//flag for saving data
bool shouldSaveConfig = false;

//callback notifying us of the need to save config
void saveConfigCallback () {
  Serial.println("Should save config");
  shouldSaveConfig = true;
}
void setup(void)
{
  // Start Serial
  Serial.begin(115200);
  WiFiManager wifiManager;
  if (SPIFFS.begin()) {
    Serial.println("mounted file system");
    if (SPIFFS.exists("/config.json")) {
      //file exists, reading and loading
      Serial.println("reading config file");
      File configFile = SPIFFS.open("/config.json", "r");
      if (configFile) {
        Serial.println("opened config file");
        size_t size = configFile.size();
        // Allocate a buffer to store contents of the file.
        std::unique_ptr<char[]> buf(new char[size]);

        configFile.readBytes(buf.get(), size);
        DynamicJsonBuffer jsonBuffer;
        JsonObject& json = jsonBuffer.parseObject(buf.get());
        json.printTo(Serial);
        if (json.success()) {
          Serial.println("\nparsed json");
          server = json["serverAddress"];
          delay_minutes = json["delay_minutes"];
          probe_name = json["probe_name"];
        } else {
          Serial.println("failed to load json config");
        }
      }
    }
  } else {
    Serial.println("failed to mount FS");
  }
  WiFiManagerParameter custom_delay_minutes("delayMinutes", "Delay Minutes", delay_minutes, 3);
  WiFiManagerParameter custom_server("serverAddress", "Server Address", server, 40);
  WiFiManagerParameter custom_probe_name("probeName", "Probe Name", probe_name, 40);
  wifiManager.setSaveConfigCallback(saveConfigCallback);

  //add all your parameters here
  wifiManager.addParameter(&custom_delay_minutes);
  wifiManager.addParameter(&custom_server);
  wifiManager.addParameter(&custom_probe_name);
  WiFi.hostname(String(host));

  wifiManager.setConfigPortalTimeout(90);
  if (!wifiManager.startConfigPortal(host)) {
    Serial.println("failed to connect and hit timeout");
    delay(3000);
    //reset and try again, or maybe put it to deep sleep
    ESP.reset();
    delay(5000);
  }

  //if you get here you have connected to the WiFi
  Serial.println("connected...yeey :)");
  if (!MDNS.begin(host)) {
    Serial.println("Error setting up MDNS responder!");
  }

  server = custom_server.getValue();
  Serial.println("server: " + String(server));
  delay_minutes = custom_delay_minutes.getValue();
  Serial.println("delay minutes: " + String(delay_minutes));
  probe_name = custom_probe_name.getValue();
  Serial.println("probe name: " + String(probe_name));
  
  //save the custom parameters to FS
  if (shouldSaveConfig) {
    Serial.println("saving config");
    DynamicJsonBuffer jsonBuffer;
    JsonObject& json = jsonBuffer.createObject();
    json["serverAddress"] = server;
    json["delay_minutes"] = delay_minutes;
    json["probe_name"] = probe_name;

    File configFile = SPIFFS.open("/config.json", "w");
    if (!configFile) {
      Serial.println("failed to open config file for writing");
    }

    json.printTo(Serial);
    json.printTo(configFile);
    configFile.close();
    //end save
  }

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  ArduinoOTA.setHostname(host);
  ArduinoOTA.onStart([]() {
    String type;
    if (ArduinoOTA.getCommand() == U_FLASH)
      type = "sketch";
    else // U_SPIFFS
      type = "filesystem";

    // NOTE: if updating SPIFFS this would be the place to unmount SPIFFS using SPIFFS.end()
    Serial.println("Start updating " + type);
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();


  // Connect to WiFi
  WiFi.hostname(host);
  Serial.println("");
  Serial.println("WiFi connected");

  // Print the IP address
  Serial.println(WiFi.localIP());
  ds18b20.begin();
}

void loop() {
  String URL = "http://" + String(server) + "/temperature/" +  String(probe_name) + "/reading/";
  Serial.println(URL);
  http.begin(URL);
  http.addHeader("Content-Type", "application/x-www-form-urlencoded");
  ds18b20.requestTemperatures(); // Send the command to get temperatures
  float currentReading = getReading(ds18b20);
  String payload = "degrees=" ;
  payload.concat(currentReading);
  payload += "&units=F";
  Serial.println(payload);
  int httpCode = http.POST(payload);
  String answer = http.getString();                  //Get the response payload

  Serial.println(httpCode);   //Print HTTP return code
  Serial.println(answer);    //Print request response payload

  http.end();  //Close connection
  delay(atoi(delay_minutes) * 60000);
}

float getReading(DallasTemperature sensor) {
  int retryCount = 0;
  float firstReading = sensor.getTempFByIndex(0);
  //always good to wait between readings
  delay(500);
  //Get second reading to ensure that we don't have an anomaly
  float secondReading = sensor.getTempFByIndex(0);
  //If the two readings are more than a degree celsius different - retake both
  while (((firstReading - secondReading) > 1.0F || (secondReading - firstReading) > 1.0F) && ((int)firstReading * 100)  < 200 && retryCount < 10) {
    firstReading = sensor.getTempFByIndex(0);
    retryCount++;
    if (retryCount != 10) {
      delay(retryCount * 1000);
    }
    secondReading = sensor.getTempFByIndex(0);
  }
  //If after ten tries we're still off - restart
  if (retryCount == 10) {
    ESP.restart();
  }
  return secondReading;
}
