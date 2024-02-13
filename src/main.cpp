/***************************************************
  Main of FingerprintDoorbell 
 ****************************************************/

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <ESPAsyncWebServer.h>
#include <WebAuthentication.h> // otherwise the IDE wont find generateDigestHash()
#include <ElegantOTA.h>
//#include "SPIFFS.h"
#include <LittleFS.h>
#define SPIFFS LittleFS  // replace SPIFFS
#include <PubSubClient.h>
#include "FingerprintManager.h"
#include "SettingsManager.h"
#include "global.h"

enum class Mode { scan, enroll, wificonfig, maintenance };

const char* VersionInfo = "1.0";

// ===================================================================================================================
// Caution: below are not the credentials for connecting to your home network, they are for the Access Point mode!!!
// ===================================================================================================================
const char* WifiConfigSsid = "FingerprintDoorbell-Config"; // SSID used for WiFi when in Access Point mode for configuration
const char* WifiConfigPassword = "12345678"; // password used for WiFi when in Access Point mode for configuration. Min. 8 chars needed!
IPAddress   WifiConfigIp(192, 168, 4, 1); // IP of access point in wifi config mode

const char* Selected = "selected";
const char* Checked = "checked";

const long  gmtOffset_sec = 0; // UTC Time
const int   daylightOffset_sec = 0; // UTC Time
const int   doorbellOutputPin = 19; // pin connected to the doorbell (when using hardware connection instead of mqtt to ring the bell)

#ifdef CUSTOM_GPIOS
  const int   customOutput1 = 18; // not used internally, but can be set over MQTT
  const int   customOutput2 = 26; // not used internally, but can be set over MQTT
  const int   customInput1 = 21; // not used internally, but changes are published over MQTT
  const int   customInput2 = 22; // not used internally, but changes are published over MQTT
  bool customInput1Value = false;
  bool customInput2Value = false;
#endif

const int logMessagesCount = 5;
String logMessages[logMessagesCount]; // log messages, 0=most recent log message
bool shouldReboot = false;
unsigned long wifiReconnectPreviousMillis = 0;
unsigned long mqttReconnectPreviousMillis = 0;

String enrollId;
String enrollName;
Mode currentMode = Mode::scan;

FingerprintManager fingerManager;
SettingsManager settingsManager;
bool needMaintenanceMode = false;

const byte DNS_PORT = 53;
DNSServer dnsServer;
AsyncWebServer webServer(80); // AsyncWebServer  on port 80
AsyncEventSource events("/events"); // event source (Server-Sent events)

WiFiClient espClient;
PubSubClient mqttClient(espClient);
long lastMsg = 0;
char msg[50];
int value = 0;
bool mqttConfigValid = true;


Match lastMatch;

void addLogMessage(const String& message) {
  // shift all messages in array by 1, oldest message will die
  for (int i=logMessagesCount-1; i>0; i--)
    logMessages[i]=logMessages[i-1];
  logMessages[0]=message;
}

String getLogMessagesAsHtml() {
  String html = "";
  for (int i=logMessagesCount-1; i>=0; i--) {
    if (logMessages[i]!="")
      html = html + logMessages[i] + "<br>";
  }
  return html;
}

String getTimestampString(){
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    Serial.println("Failed to obtain time");
    return "no time";
  }
  
  char buffer[25];
  strftime(buffer,sizeof(buffer),"%Y-%m-%d %H:%M:%S %Z", &timeinfo);
  String datetime = String(buffer);
  return datetime;
}

/* wait for maintenance mode or timeout 5s */
bool waitForMaintenanceMode() {
  needMaintenanceMode = true;
  unsigned long startMillis = millis();
  while (currentMode != Mode::maintenance) {
    if ((millis() - startMillis) >= 5000ul) {
      needMaintenanceMode = false;
      return false;
    }
    delay(50);
  }
  needMaintenanceMode = false;
  return true;
}

// Replaces placeholder in HTML pages
String processor(const String& var){
  if(var == "LOGMESSAGES"){
    return getLogMessagesAsHtml();
  } else if (var == "FINGERLIST") {
    return fingerManager.getFingerListAsHtmlOptionList();
  } else if (var == "HOSTNAME") {
    return settingsManager.getWifiSettings().hostname;
  } else if (var == "VERSIONINFO") {
    return VersionInfo;
  } else if (var == "WIFI_SSID") {
    return settingsManager.getWifiSettings().ssid;
  } else if (var == "WIFI_PASSWORD") {
    if (settingsManager.getWifiSettings().password.isEmpty())
      return "";
    else
      return "********"; // for security reasons the wifi password will not leave the device once configured
  } else if (var.indexOf("DHCP_SETTING") != -1) {
    return (settingsManager.getWifiSettings().dhcp_setting == var.substring(var.length() - 1).toInt()) ? Checked : "";
  } else if (var == "LOCAL_IP") {
    return settingsManager.getWifiSettings().localIP.toString();
  } else if (var == "GATEWAY_IP") {
    return settingsManager.getWifiSettings().gatewayIP.toString();
  } else if (var == "SUBNET_MASK") {
    return settingsManager.getWifiSettings().subnetMask.toString();
  } else if (var == "DNS_IP0") {
    return settingsManager.getWifiSettings().dnsIP0.toString();
  } else if (var == "DNS_IP1") {
    return settingsManager.getWifiSettings().dnsIP1.toString();
  } else if (var == "MQTT_SERVER") {
    return settingsManager.getAppSettings().mqttServer;
  }else if (var == "MQTT_PORT") {
    return String(settingsManager.getAppSettings().mqttPort);
  } else if (var == "MQTT_USERNAME") {
    return settingsManager.getAppSettings().mqttUsername;
  } else if (var == "MQTT_PASSWORD") {
    return settingsManager.getAppSettings().mqttPassword;
  } else if (var == "MQTT_ROOTTOPIC") {
    return settingsManager.getAppSettings().mqttRootTopic;
  } else if (var == "NTP_SERVER") {
    return settingsManager.getAppSettings().ntpServer;
  } else if (var == "WEBPAGE_USERNAME") {
    return settingsManager.getWebPageSettings().webPageUsername;
  } else if (var == "WEBPAGE_PASSWORD") {
    if (settingsManager.getWebPageSettings().webPagePassword.isEmpty())
      return "";
    else
      return "********"; // for security reasons the web page password will not leave the device once configured
  } else if (var.indexOf("ACTIVE_COLOR") != -1) {
    return (settingsManager.getColorSettings().activeColor == var.substring(var.length() - 1).toInt()) ? Selected : "";
  } else if (var.indexOf("ACTIVE_SEQUENCE") != -1) {
    return (settingsManager.getColorSettings().activeSequence == var.substring(var.length() - 1).toInt()) ? Checked : "";
  } else if (var.indexOf("SCAN_COLOR") != -1) {
    return (settingsManager.getColorSettings().scanColor == var.substring(var.length() - 1).toInt()) ? Selected : "";
  } else if (var.indexOf("SCAN_SEQUENCE") != -1) {
    return (settingsManager.getColorSettings().scanSequence == var.substring(var.length() - 1).toInt()) ? Checked : "";
  } else if (var.indexOf("MATCH_COLOR") != -1) {
    return (settingsManager.getColorSettings().matchColor == var.substring(var.length() - 1).toInt()) ? Selected : "";
  } else if (var.indexOf("MATCH_SEQUENCE") != -1) {
    return (settingsManager.getColorSettings().matchSequence == var.substring(var.length() - 1).toInt()) ? Checked : "";
  } else if (var.indexOf("ENROLL_COLOR") != -1) {
    return (settingsManager.getColorSettings().enrollColor == var.substring(var.length() - 1).toInt()) ? Selected : "";
  } else if (var.indexOf("ENROLL_SEQUENCE") != -1) {
    return (settingsManager.getColorSettings().enrollSequence == var.substring(var.length() - 1).toInt()) ? Checked : "";
  } else if (var.indexOf("CONNECT_COLOR") != -1) {
    return (settingsManager.getColorSettings().connectColor == var.substring(var.length() - 1).toInt()) ? Selected : "";
  } else if (var.indexOf("CONNECT_SEQUENCE") != -1) {
    return (settingsManager.getColorSettings().connectSequence == var.substring(var.length() - 1).toInt()) ? Checked : "";
  } else if (var.indexOf("WIFI_COLOR") != -1) {
    return (settingsManager.getColorSettings().wifiColor == var.substring(var.length() - 1).toInt()) ? Selected : "";
  } else if (var.indexOf("WIFI_SEQUENCE") != -1) {
    return (settingsManager.getColorSettings().wifiSequence == var.substring(var.length() - 1).toInt()) ? Checked : "";
  } else if (var.indexOf("ERROR_COLOR") != -1) {
    return (settingsManager.getColorSettings().errorColor == var.substring(var.length() - 1).toInt()) ? Selected : "";
  } else if (var.indexOf("ERROR_SEQUENCE") != -1) {
    return (settingsManager.getColorSettings().errorSequence == var.substring(var.length() - 1).toInt()) ? Checked : "";
  }

  return String();
}


// send LastMessage to websocket clients
void notifyClients(String message) {
  String messageWithTimestamp = "[" + getTimestampString() + "]: " + message;
  Serial.println(messageWithTimestamp);
  addLogMessage(messageWithTimestamp);
  events.send(getLogMessagesAsHtml().c_str(),"message",millis(),1000);
  
  String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
  mqttClient.publish((String(mqttRootTopic) + "/lastLogMessage").c_str(), message.c_str());
}

void updateClientsFingerlist(String fingerlist) {
  Serial.println("New fingerlist was sent to clients");
  events.send(fingerlist.c_str(),"fingerlist",millis(),1000);
}


bool doPairing() {
  String newPairingCode = settingsManager.generateNewPairingCode();

  if (fingerManager.setPairingCode(newPairingCode)) {
    AppSettings settings = settingsManager.getAppSettings();
    settings.sensorPairingCode = newPairingCode;
    settings.sensorPairingValid = true;
    settingsManager.saveAppSettings(settings);
    notifyClients("Pairing successful.");
    return true;
  } else {
    notifyClients("Pairing failed.");
    return false;
  }

}


bool checkPairingValid() {
  AppSettings settings = settingsManager.getAppSettings();

   if (!settings.sensorPairingValid) {
     if (settings.sensorPairingCode.isEmpty()) {
       // first boot, do pairing automatically so the user does not have to do this manually
       return doPairing();
     } else {
      Serial.println("Pairing has been invalidated previously.");   
      return false;
     }
   }

  String actualSensorPairingCode = fingerManager.getPairingCode();
  //Serial.println("Awaited pairing code: " + settings.sensorPairingCode);
  //Serial.println("Actual pairing code: " + actualSensorPairingCode);

  if (actualSensorPairingCode.equals(settings.sensorPairingCode))
    return true;
  else {
    if (!actualSensorPairingCode.isEmpty()) { 
      // An empty code means there was a communication problem. So we don't have a valid code, but maybe next read will succeed and we get one again.
      // But here we just got an non-empty pairing code that was different to the awaited one. So don't expect that will change in future until repairing was done.
      // -> invalidate pairing for security reasons
      AppSettings settings = settingsManager.getAppSettings();
      settings.sensorPairingValid = false;
      settingsManager.saveAppSettings(settings);
    }
    return false;
  }
}


bool initWifi() {
  // Connect to Wi-Fi
  WifiSettings wifiSettings = settingsManager.getWifiSettings();
  WiFi.setHostname(wifiSettings.hostname.c_str()); //define hostname
  WiFi.mode(WIFI_STA);
  WiFi.begin(wifiSettings.ssid.c_str(), wifiSettings.password.c_str());
  int counter = 0;
  while (WiFi.status() != WL_CONNECTED) {
    delay(1000);
    Serial.println("Waiting for WiFi connection...");
    counter++;
    if (counter > 30)
      return false;
  }
  if (!settingsManager.getWifiSettings().dhcp_setting){
    if (settingsManager.getWifiSettings().localIP.toString() != "0.0.0.0" && settingsManager.getWifiSettings().gatewayIP.toString() != "0.0.0.0" && settingsManager.getWifiSettings().subnetMask.toString() != "0.0.0.0" && settingsManager.getWifiSettings().dnsIP0.toString() != "0.0.0.0" && settingsManager.getWifiSettings().dnsIP1.toString() != "0.0.0.0"){
      if (WiFi.config(settingsManager.getWifiSettings().localIP, settingsManager.getWifiSettings().gatewayIP, settingsManager.getWifiSettings().subnetMask, settingsManager.getWifiSettings().dnsIP0, settingsManager.getWifiSettings().dnsIP1))
        notifyClients("Static IP address settings were activated.");
      else
        notifyClients("Static IP address settings could not be activated. DHCP is used instead.");
    } else {
      notifyClients("Static IP address settings are incomplete. DHCP is used instead.");
    }
  }


  //initialize mDNS service
  esp_err_t err = mdns_init();
  if (err) {
      printf("MDNS Init failed: %d\n", err);
  }
  //set hostname
  mdns_hostname_set(wifiSettings.hostname.c_str());
  //set default instance
  mdns_instance_name_set(wifiSettings.hostname.c_str());
  // Add service to MDNS-SD
  MDNS.addService("http", "tcp", 80);

  Serial.println("Connected!");

  // Print ESP32 Local IP Address
  Serial.print("Local IP address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Gateway: ");
  Serial.println(WiFi.gatewayIP());
  Serial.print("Subnet mask: ");
  Serial.println(WiFi.subnetMask());
  Serial.print("DNS server 1: ");
  Serial.println(WiFi.dnsIP(0));
  Serial.print("DNS server 2: ");
  Serial.println(WiFi.dnsIP(1));

  return true;
}

void initWiFiAccessPointForConfiguration() {
  WiFi.softAPConfig(WifiConfigIp, WifiConfigIp, IPAddress(255, 255, 255, 0));
  WiFi.softAP(WifiConfigSsid, WifiConfigPassword);

  // if DNSServer is started with "*" for domain name, it will reply with
  // provided IP to all DNS request
  dnsServer.start(DNS_PORT, "*", WifiConfigIp);

  Serial.print("AP IP address: ");
  Serial.println(WifiConfigIp); 
}

// Function handling the HTTP Digest Authentication
bool authenticate(AsyncWebServerRequest *request, WebPageSettings webPageSettings) {
  if (!request->authenticate(generateDigestHash(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), webPageSettings.webPageRealm.c_str()).c_str())) {
    request->requestAuthentication(webPageSettings.webPageRealm.c_str(), true);
    return false;
  }
  return true;
}

void startWebserver(){
  
  // Initialize SPIFFS
  if(!SPIFFS.begin(true)){
    Serial.println("An Error has occurred while mounting SPIFFS");
    return;
  }

  // Init time by NTP Client
  configTime(gmtOffset_sec, daylightOffset_sec, settingsManager.getAppSettings().ntpServer.c_str());

  // Load web page log in credentials
  WebPageSettings webPageSettings = settingsManager.getWebPageSettings();
  
  // webserver for normal operating or wifi config?
  if (currentMode == Mode::wificonfig)
  {
    // =================
    // WiFi config mode
    // =================

    webServer.on("/", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        request->send(SPIFFS, "/wificonfig.html", String(), false, processor);
      }
    });

    webServer.on("/save", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        if(request->hasArg("hostname"))
        {
          Serial.println("Save wifi config");
          WifiSettings settings = settingsManager.getWifiSettings();
          settings.hostname = request->arg("hostname");
          settings.ssid = request->arg("ssid");
          if (request->arg("password").equals("********")) // password is replaced by wildcards when given to the browser, so if the user didn't changed it, don't save it
            settings.password = settingsManager.getWifiSettings().password; // use the old, already saved, one
          else
            settings.password = request->arg("password");
          settingsManager.saveWifiSettings(settings);
          shouldReboot = true;
        }
        request->redirect("/");
      }
    });

    webServer.onNotFound([](AsyncWebServerRequest *request){
      AsyncResponseStream *response = request->beginResponseStream("text/html");
      response->printf("<!DOCTYPE html><html><head><title>FingerprintDoorbell</title><meta http-equiv=\"refresh\" content=\"0; url=http://%s\" /></head><body>", WiFi.softAPIP().toString().c_str());
      response->printf("<p>Please configure your WiFi settings <a href='http://%s'>here</a> to connect FingerprintDoorbell to your home network.</p>", WiFi.softAPIP().toString().c_str());
      response->print("</body></html>");
      request->send(response);
    });

  }
  else
  {
    // =======================
    // normal operating mode
    // =======================
    events.onConnect([](AsyncEventSourceClient *client){
      if(client->lastId()){
        Serial.printf("Client reconnected! Last message ID it got was: %u\n", client->lastId());
      }
      //send event with message "ready", id current millis
      // and set reconnect delay to 1 second
      client->send(getLogMessagesAsHtml().c_str(),"message",millis(),1000);
    });
    webServer.addHandler(&events);

    // Route for root / web page
    webServer.on("/", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        request->send(SPIFFS, "/index.html", String(), false, processor);
      }
    });

    webServer.on("/enroll", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        if(request->hasArg("startEnrollment"))
        {
          enrollId = request->arg("newFingerprintId");
          enrollName = request->arg("newFingerprintName");
          currentMode = Mode::enroll;
        }
        request->redirect("/");
      }
    });

    webServer.on("/editFingerprints", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        if(request->hasArg("selectedFingerprint"))
        {
          if(request->hasArg("btnDelete"))
          {
            int id = request->arg("selectedFingerprint").toInt();
            waitForMaintenanceMode();
            fingerManager.deleteFinger(id);
            currentMode = Mode::scan;
          }
          else if (request->hasArg("btnRename"))
          {
            int id = request->arg("selectedFingerprint").toInt();
            String newName = request->arg("renameNewName");
            fingerManager.renameFinger(id, newName);
          }
        }
        request->redirect("/");  
      }
    });

    webServer.on("/colorSettings", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        if(request->hasArg("btnSaveColorSettings"))
        {
          Serial.println("Save color and sequence settings");
          ColorSettings colorSettings = settingsManager.getColorSettings();
          colorSettings.activeColor = (uint8_t) request->arg("activeColor").toInt();
          colorSettings.activeSequence = (uint8_t) request->arg("activeSequence").toInt();
          colorSettings.scanColor = (uint8_t) request->arg("scanColor").toInt();
          colorSettings.scanSequence = (uint8_t) request->arg("scanSequence").toInt();
          colorSettings.matchColor = (uint8_t) request->arg("matchColor").toInt();
          colorSettings.matchSequence = (uint8_t) request->arg("matchSequence").toInt();
          colorSettings.enrollColor = (uint8_t) request->arg("enrollColor").toInt();
          colorSettings.enrollSequence = (uint8_t) request->arg("enrollSequence").toInt();
          colorSettings.connectColor = (uint8_t) request->arg("connectColor").toInt();
          colorSettings.connectSequence = (uint8_t) request->arg("connectSequence").toInt();
          colorSettings.wifiColor = (uint8_t) request->arg("wifiColor").toInt();
          colorSettings.wifiSequence = (uint8_t) request->arg("wifiSequence").toInt();
          colorSettings.errorColor = (uint8_t) request->arg("errorColor").toInt();
          colorSettings.errorSequence = (uint8_t) request->arg("errorSequence").toInt();
          settingsManager.saveColorSettings(colorSettings);
          request->redirect("/colorSettings");
          shouldReboot = true;
        } else {
          request->send(SPIFFS, "/colorSettings.html", String(), false, processor);
        }
      }
    });

    webServer.on("/wifiSettings", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        if(request->hasArg("btnSaveWiFiSettings"))
        {
          Serial.println("Save wifi config");
          WifiSettings settings = settingsManager.getWifiSettings();
          settings.hostname = request->arg("hostname");
          settings.ssid = request->arg("ssid");
          if (request->arg("password").equals("********")) // password is replaced by wildcards when given to the browser, so if the user didn't changed it, don't save it
            settings.password = settingsManager.getWifiSettings().password; // use the old, already saved, one
          else
            settings.password = request->arg("password");
          settings.dhcp_setting = request->arg("dhcp_setting").equals("1");
          if(!settings.localIP.fromString(request->arg("local_ip")))
            Serial.println("Local IP address could not be parsed.");
          if(!settings.gatewayIP.fromString(request->arg("gateway_ip")))
            Serial.println("Gateway IP address could not be parsed.");
          if(!settings.subnetMask.fromString(request->arg("subnet_mask")))
            Serial.println("Subnet mask could not be parsed.");
          if(!settings.dnsIP0.fromString(request->arg("dns_ip0")))
            Serial.println("DNS server 1 could not be parsed.");
          if(!settings.dnsIP1.fromString(request->arg("dns_ip1")))
            Serial.println("DNS server 2 could not be parsed.");
          settingsManager.saveWifiSettings(settings);
          request->redirect("/wifiSettings");
          shouldReboot = true;
        } else {
          request->send(SPIFFS, "/wifiSettings.html", String(), false, processor);
        }
      }
    });

    webServer.on("/settings", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        if(request->hasArg("btnSaveSettings"))
        {
          Serial.println("Save settings");
          AppSettings settings = settingsManager.getAppSettings();
          settings.mqttServer = request->arg("mqtt_server");
          settings.mqttPort = (uint16_t) request->arg("mqtt_port").toInt();
          settings.mqttUsername = request->arg("mqtt_username");
          settings.mqttPassword = request->arg("mqtt_password");
          settings.mqttRootTopic = request->arg("mqtt_rootTopic");
          settings.ntpServer = request->arg("ntpServer");
          settingsManager.saveAppSettings(settings);
          request->redirect("/settings");
          shouldReboot = true;
        } else if(request->hasArg("btnSaveWebPageSettings"))
        {
          Serial.println("Save web page settings");
          WebPageSettings webPageSettings = settingsManager.getWebPageSettings();
          webPageSettings.webPageUsername = request->arg("webpage_username");
          webPageSettings.webPagePassword = request->arg("webpage_password");
          settingsManager.saveWebPageSettings(webPageSettings);
          request->redirect("/settings");
          shouldReboot = true;
        } else {
          request->send(SPIFFS, "/settings.html", String(), false, processor);
        }
      }
    });

    webServer.on("/pairing", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        if(request->hasArg("btnDoPairing"))
        {
          Serial.println("Do (re)pairing");
          doPairing();
          request->redirect("/");  
        } else {
          request->send(SPIFFS, "/settings.html", String(), false, processor);
        }
      }
    });

    webServer.on("/factoryReset", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        if(request->hasArg("btnFactoryReset"))
        {
          notifyClients("Factory reset initiated...");
          
          if (!fingerManager.deleteAll())
            notifyClients("Finger database could not be deleted.");

          if (!settingsManager.deleteColorSettings())
            notifyClients("Color settings could not be deleted.");

          if (!settingsManager.deleteWifiSettings())
            notifyClients("Wifi settings could not be deleted.");
          
          if (!settingsManager.deleteAppSettings())
            notifyClients("App settings could not be deleted.");
          
          if (!settingsManager.deleteWebPageSettings())
            notifyClients("Web page settings could not be deleted.");
          
          request->redirect("/");  
          shouldReboot = true;
        } else {
          request->send(SPIFFS, "/settings.html", String(), false, processor);
        }
      }
    });

    webServer.on("/deleteAllFingerprints", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
      if (authenticate(request, webPageSettings)) {
        if(request->hasArg("btnDeleteAllFingerprints"))
        {
          notifyClients("Deleting all fingerprints...");
          
          if (!fingerManager.deleteAll())
            notifyClients("Finger database could not be deleted.");
          
          request->redirect("/");  
          
        } else {
          request->send(SPIFFS, "/settings.html", String(), false, processor);
        }
      }
    });

    webServer.onNotFound([](AsyncWebServerRequest *request){
      request->send(404, "text/plain", "Not found");
    });

    
  } // end normal operating mode


  // common url callbacks
  webServer.on("/reboot", HTTP_GET, [webPageSettings](AsyncWebServerRequest *request){
    if (authenticate(request, webPageSettings)) {
      request->redirect("/");
      shouldReboot = true;
    }
  });

  webServer.on("/bootstrap.min.css", HTTP_GET, [](AsyncWebServerRequest *request){
    request->send(SPIFFS, "/bootstrap.min.css", "text/css");
  });

  webServer.on("/logout", HTTP_GET, [](AsyncWebServerRequest *request){
    File logoutPage = SPIFFS.open("/logout.html", "r");
    // Set the content type
    request->send(401, "text/html", logoutPage.readString());
    logoutPage.close();
  });


  // Enable Over-the-air updates at http://<IPAddress>/update
  ElegantOTA.begin(&webServer);    // Start ElegantOTA
  
  // Start server
  webServer.begin();

  notifyClients("System booted successfully!");

}

void mqttCallback(char* topic, byte* message, unsigned int length) {
  Serial.print("Message arrived on topic: ");
  Serial.print(topic);
  Serial.print(". Message: ");
  String messageTemp;
  
  for (int i = 0; i < length; i++) {
    Serial.print((char)message[i]);
    messageTemp += (char)message[i];
  }
  Serial.println();

  // Check incomming message for interesting topics
  if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/ignoreTouchRing") {
    if(messageTemp == "on"){
      fingerManager.setIgnoreTouchRing(true);
    }
    else if(messageTemp == "off"){
      fingerManager.setIgnoreTouchRing(false);
    }
  }

  #ifdef CUSTOM_GPIOS
    if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/customOutput1") {
      if(messageTemp == "on"){
        digitalWrite(customOutput1, HIGH); 
      }
      else if(messageTemp == "off"){
        digitalWrite(customOutput1, LOW); 
      }
    }
    if (String(topic) == String(settingsManager.getAppSettings().mqttRootTopic) + "/customOutput2") {
      if(messageTemp == "on"){
        digitalWrite(customOutput2, HIGH); 
      }
      else if(messageTemp == "off"){
        digitalWrite(customOutput2, LOW); 
      }
    }
  #endif  

}

void connectMqttClient() {
  if (!mqttClient.connected() && mqttConfigValid) {
    Serial.print("(Re)connect to MQTT broker...");
    // Attempt to connect
    bool connectResult;
    
    // connect with or witout authentication
    String lastWillTopic = settingsManager.getAppSettings().mqttRootTopic + "/lastLogMessage";
    String lastWillMessage = "FingerprintDoorbell disconnected unexpectedly";
    if (settingsManager.getAppSettings().mqttUsername.isEmpty() || settingsManager.getAppSettings().mqttPassword.isEmpty())
      connectResult = mqttClient.connect(settingsManager.getWifiSettings().hostname.c_str(),lastWillTopic.c_str(), 1, false, lastWillMessage.c_str());
    else
      connectResult = mqttClient.connect(settingsManager.getWifiSettings().hostname.c_str(), settingsManager.getAppSettings().mqttUsername.c_str(), settingsManager.getAppSettings().mqttPassword.c_str(), lastWillTopic.c_str(), 1, false, lastWillMessage.c_str());

    if (connectResult) {
      // success
      Serial.println("connected");
      // Subscribe
      mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/ignoreTouchRing").c_str(), 1); // QoS = 1 (at least once)
      #ifdef CUSTOM_GPIOS
        mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/customOutput1").c_str(), 1); // QoS = 1 (at least once)
        mqttClient.subscribe((settingsManager.getAppSettings().mqttRootTopic + "/customOutput2").c_str(), 1); // QoS = 1 (at least once)
      #endif



    } else {
      if (mqttClient.state() == 4 || mqttClient.state() == 5) {
        mqttConfigValid = false;
        notifyClients("Failed to connect to MQTT Server: bad credentials or not authorized. Will not try again, please check your settings.");
      } else {
        notifyClients(String("Failed to connect to MQTT Server, rc=") + mqttClient.state() + ", try again in 30 seconds");
      }
    }
  }
}


void doScan()
{
  Match match = fingerManager.scanFingerprint();
  String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
  switch(match.scanResult)
  {
    case ScanResult::noFinger:
      // standard case, occurs every iteration when no finger touchs the sensor
      if (match.scanResult != lastMatch.scanResult) {
        Serial.println("no finger");
        mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "off");
        mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), "-1");
        mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), "");
        mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), "-1");
      }
      break; 
    case ScanResult::matchFound:
      notifyClients( String("Match Found: ") + match.matchId + " - " + match.matchName  + " with confidence of " + match.matchConfidence );
      if (match.scanResult != lastMatch.scanResult) {
        if (checkPairingValid()) {
          mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "off");
          mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), String(match.matchId).c_str());
          mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), match.matchName.c_str());
          mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), String(match.matchConfidence).c_str());
          Serial.println("MQTT message sent: Open the door!");
        } else {
          notifyClients("Security issue! Match was not sent by MQTT because of invalid sensor pairing! This could potentially be an attack! If the sensor is new or has been replaced by you do a (re)pairing in settings page.");
        }
      }
      delay(3000); // wait some time before next scan to let the LED blink
      break;
    case ScanResult::noMatchFound:
      notifyClients(String("No Match Found (Code ") + match.returnCode + ")");
      if (match.scanResult != lastMatch.scanResult) {
        digitalWrite(doorbellOutputPin, HIGH);
        mqttClient.publish((String(mqttRootTopic) + "/ring").c_str(), "on");
        mqttClient.publish((String(mqttRootTopic) + "/matchId").c_str(), "-1");
        mqttClient.publish((String(mqttRootTopic) + "/matchName").c_str(), "");
        mqttClient.publish((String(mqttRootTopic) + "/matchConfidence").c_str(), "-1");
        Serial.println("MQTT message sent: ring the bell!");
        delay(1000);
        digitalWrite(doorbellOutputPin, LOW); 
      } else {
        delay(1000); // wait some time before next scan to let the LED blink
      }
      break;
    case ScanResult::error:
      notifyClients(String("ScanResult Error (Code ") + match.returnCode + ")");
      break;
  };
  lastMatch = match;

}

void doEnroll()
{
  int id = enrollId.toInt();
  if (id < 1 || id > 200) {
    notifyClients("Invalid memory slot id '" + enrollId + "'");
    return;
  }

  NewFinger finger = fingerManager.enrollFinger(id, enrollName);
  if (finger.enrollResult == EnrollResult::ok) {
    notifyClients("Enrollment successfull. You can now use your new finger for scanning.");
    updateClientsFingerlist(fingerManager.getFingerListAsHtmlOptionList());
  }  else if (finger.enrollResult == EnrollResult::error) {
    notifyClients(String("Enrollment failed. (Code ") + finger.returnCode + ")");
  }
}



void reboot()
{
  notifyClients("System is rebooting now...");
  delay(1000);
    
  mqttClient.disconnect();
  espClient.stop();
  dnsServer.stop();
  webServer.end();
  WiFi.disconnect();
  ESP.restart();
}

void setup()
{
  // open serial monitor for debug infos
  Serial.begin(115200);
  while (!Serial);  // For Yun/Leo/Micro/Zero/...
  delay(100);

  // initialize GPIOs
  pinMode(doorbellOutputPin, OUTPUT); 
  #ifdef CUSTOM_GPIOS
    pinMode(customOutput1, OUTPUT); 
    pinMode(customOutput2, OUTPUT); 
    pinMode(customInput1, INPUT_PULLDOWN);
    pinMode(customInput2, INPUT_PULLDOWN);
  #endif  

  settingsManager.loadWifiSettings();
  settingsManager.loadWebPageSettings();
  settingsManager.loadAppSettings();
  settingsManager.loadColorSettings();

  // Set Authentication Credentials
  ElegantOTA.setAuth(settingsManager.getWebPageSettings().webPageUsername.c_str(), settingsManager.getWebPageSettings().webPagePassword.c_str());

  fingerManager.connect();
  
  if (!checkPairingValid())
    notifyClients("Security issue! Pairing with sensor is invalid. This could potentially be an attack! If the sensor is new or has been replaced by you do a (re)pairing in settings page. MQTT messages regarding matching fingerprints will not been sent until pairing is valid again.");

  if (fingerManager.isFingerOnSensor() || !settingsManager.isWifiConfigured())
  {
    // ring touched during startup or no wifi settings stored -> wifi config mode
    currentMode = Mode::wificonfig;
    Serial.println("Started WiFi-Config mode");
    fingerManager.setLedRingWifiConfig();
    initWiFiAccessPointForConfiguration();
    startWebserver();

  } else {
    Serial.println("Started normal operating mode");
    currentMode = Mode::scan;
    if (initWifi()) {
      startWebserver();
      if (settingsManager.getAppSettings().mqttServer.isEmpty()) {
        mqttConfigValid = false;
        notifyClients("Error: No MQTT Broker is configured! Please go to settings and enter your server URL + user credentials.");
      } else {
        delay(5000);

        IPAddress mqttServerIp;
        if (WiFi.hostByName(settingsManager.getAppSettings().mqttServer.c_str(), mqttServerIp))
        {
          mqttConfigValid = true;
          Serial.println("IP used for MQTT server: " + mqttServerIp.toString() + " | Port: " + String(settingsManager.getAppSettings().mqttPort));
          mqttClient.setServer(mqttServerIp , settingsManager.getAppSettings().mqttPort);
          mqttClient.setCallback(mqttCallback);
          connectMqttClient();
        }
        else {
          mqttConfigValid = false;
          notifyClients("MQTT Server '" + settingsManager.getAppSettings().mqttServer + "' not found. Please check your settings.");
        }
      }
      if (fingerManager.connected) {
        fingerManager.setColorSettings(settingsManager.getColorSettings());
        fingerManager.setLedRingReady();
      }
      else
        fingerManager.setLedRingError();
    }  else {
      fingerManager.setLedRingError();
      shouldReboot = true;
    }

  }
  
}

void loop()
{
  // shouldReboot flag for supporting reboot through webui
  if (shouldReboot) {
    reboot();
  }
  
  // Reconnect handling
  if (currentMode != Mode::wificonfig)
  {
    unsigned long currentMillis = millis();
    // reconnect WiFi if down for 30s
    if ((WiFi.status() != WL_CONNECTED) && (currentMillis - wifiReconnectPreviousMillis >= 30000ul)) {
      Serial.println("Reconnecting to WiFi...");
      WiFi.disconnect();
      WiFi.reconnect();
      wifiReconnectPreviousMillis = currentMillis;
    }

    // reconnect mqtt if down
    if (!settingsManager.getAppSettings().mqttServer.isEmpty()) {
      if (!mqttClient.connected() && (currentMillis - mqttReconnectPreviousMillis >= 30000ul)) {
        connectMqttClient();
        mqttReconnectPreviousMillis = currentMillis;
      }
      mqttClient.loop();
    }
  }


  // do the actual loop work
  switch (currentMode)
  {
  case Mode::scan:
    if (fingerManager.connected)
      doScan();
    break;
  
  case Mode::enroll:
    doEnroll();
    currentMode = Mode::scan; // switch back to scan mode after enrollment is done
    break;
  
  case Mode::wificonfig:
    dnsServer.processNextRequest(); // used for captive portal redirect
    break;

  case Mode::maintenance:
    // do nothing, give webserver exclusive access to sensor (not thread-safe for concurrent calls)
    break;

  }

  // enter maintenance mode (no continous scanning) if requested
  if (needMaintenanceMode)
    currentMode = Mode::maintenance;

  #ifdef CUSTOM_GPIOS
    // read custom inputs and publish by MQTT
    bool i1;
    bool i2;
    i1 = (digitalRead(customInput1) == HIGH);
    i2 = (digitalRead(customInput2) == HIGH);

    String mqttRootTopic = settingsManager.getAppSettings().mqttRootTopic;
    if (i1 != customInput1Value) {
        if (i1)
          mqttClient.publish((String(mqttRootTopic) + "/customInput1").c_str(), "on");      
        else
          mqttClient.publish((String(mqttRootTopic) + "/customInput1").c_str(), "off");      
    }

    if (i2 != customInput2Value) {
        if (i2)
          mqttClient.publish((String(mqttRootTopic) + "/customInput2").c_str(), "on");      
        else
          mqttClient.publish((String(mqttRootTopic) + "/customInput2").c_str(), "off");  
    }

    customInput1Value = i1;
    customInput2Value = i2;

  #endif

  // OTA update handling
  ElegantOTA.loop();
}
