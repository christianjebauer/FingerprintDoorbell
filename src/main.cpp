/***************************************************
  Main of FingerprintDoorbell 
 ****************************************************/

#include <WiFi.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <PsychicHttp.h>
#ifdef PSY_ENABLE_SSL
  #include <PsychicHttpsServer.h>
#endif
#include <ElegantOTA.h>
#include <LittleFS.h>
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

String selected = "selected";
String checked = "checked";

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
unsigned long ota_progress_millis = 0;
unsigned long lastUpdate = 0;
char output[60];

String enrollId;
String enrollName;
Mode currentMode = Mode::scan;

FingerprintManager fingerManager;
SettingsManager settingsManager;
bool needMaintenanceMode = false;

const byte DNS_PORT = 53;
DNSServer dnsServer;
// #define PSY_ENABLE_SSL to enable SSL encryption
#ifdef PSY_ENABLE_SSL
  bool app_enable_ssl = true;
  String server_cert;
  String server_key;
  PsychicHttpsServer webServer;
#else
  PsychicHttpServer webServer;
#endif
PsychicEventSource events; // event source (Server-Sent events)

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

String processFile(const String& fileContent) {
  String processedContent = fileContent;
  processedContent.replace("%LOGMESSAGES%", getLogMessagesAsHtml());
  processedContent.replace("%FINGERLIST%", fingerManager.getFingerListAsHtmlOptionList());
  processedContent.replace("%HOSTNAME%", settingsManager.getWifiSettings().hostname);
  processedContent.replace("%VERSIONINFO%", VersionInfo);
  processedContent.replace("%WIFI_SSID%", settingsManager.getWifiSettings().ssid);
  if (settingsManager.getWifiSettings().password.isEmpty())
    processedContent.replace("%WIFI_PASSWORD%", "");
  else
    processedContent.replace("%WIFI_PASSWORD%", "********"); // for security reasons the wifi password will not leave the device once configured
  processedContent.replace(("%DHCP_SETTING_" + String((int)settingsManager.getWifiSettings().dhcp_setting) + "%"), checked);
  processedContent.replace("%LOCAL_IP%", settingsManager.getWifiSettings().localIP.toString());
  processedContent.replace("%GATEWAY_IP%", settingsManager.getWifiSettings().gatewayIP.toString());
  processedContent.replace("%SUBNET_MASK%", settingsManager.getWifiSettings().subnetMask.toString());
  processedContent.replace("%DNS_IP0%", settingsManager.getWifiSettings().dnsIP0.toString());
  processedContent.replace("%DNS_IP1%", settingsManager.getWifiSettings().dnsIP1.toString());
  processedContent.replace("%MQTT_SERVER%", settingsManager.getAppSettings().mqttServer);
  processedContent.replace("%MQTT_PORT%", String(settingsManager.getAppSettings().mqttPort));
  processedContent.replace("%MQTT_USERNAME%", settingsManager.getAppSettings().mqttUsername);
  if (settingsManager.getAppSettings().mqttPassword.isEmpty())
    processedContent.replace("%MQTT_PASSWORD%", "");
  else
    processedContent.replace("%MQTT_PASSWORD%", "********"); // for security reasons the MQTT password will not leave the device once configured
  processedContent.replace("%MQTT_ROOTTOPIC%", settingsManager.getAppSettings().mqttRootTopic);
  processedContent.replace("%NTP_SERVER%", settingsManager.getAppSettings().ntpServer);
  processedContent.replace("%WEBPAGE_USERNAME%", settingsManager.getWebPageSettings().webPageUsername);
  if (settingsManager.getWebPageSettings().webPagePassword.isEmpty())
    processedContent.replace("%WEBPAGE_PASSWORD%", "");
  else
    processedContent.replace("%WEBPAGE_PASSWORD%", "********"); // for security reasons the web page password will not leave the device once configured
  processedContent.replace(("%ACTIVE_COLOR_" + String(settingsManager.getColorSettings().activeColor) + "%"), selected);
  processedContent.replace(("%ACTIVE_SEQUENCE_" + String(settingsManager.getColorSettings().activeSequence) + "%"), checked);
  processedContent.replace(("%SCAN_COLOR_" + String(settingsManager.getColorSettings().scanColor) + "%"), selected);
  processedContent.replace(("%SCAN_SEQUENCE_" + String(settingsManager.getColorSettings().scanSequence) + "%"), checked);
  processedContent.replace(("%MATCH_COLOR_" + String(settingsManager.getColorSettings().matchColor) + "%"), selected);
  processedContent.replace(("%MATCH_SEQUENCE_" + String(settingsManager.getColorSettings().matchSequence) + "%"), checked);
  processedContent.replace(("%ENROLL_COLOR_" + String(settingsManager.getColorSettings().enrollColor) + "%"), selected);
  processedContent.replace(("%ENROLL_SEQUENCE_" + String(settingsManager.getColorSettings().enrollSequence) + "%"), checked);
  processedContent.replace(("%CONNECT_COLOR_" + String(settingsManager.getColorSettings().connectColor) + "%"), selected);
  processedContent.replace(("%CONNECT_SEQUENCE_" + String(settingsManager.getColorSettings().connectSequence) + "%"), checked);
  processedContent.replace(("%WIFI_COLOR_" + String(settingsManager.getColorSettings().wifiColor) + "%"), selected);
  processedContent.replace(("%WIFI_SEQUENCE_" + String(settingsManager.getColorSettings().wifiSequence) + "%"), checked);
  processedContent.replace(("%ERROR_COLOR_" + String(settingsManager.getColorSettings().errorColor) + "%"), selected);
  processedContent.replace(("%ERROR_SEQUENCE_" + String(settingsManager.getColorSettings().errorSequence) + "%"), checked);

  return processedContent;
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

// Function to send a html file as a response to a request
esp_err_t sendHTML(PsychicRequest *request, String fileName) {
  File file = LittleFS.open(fileName.c_str(), "r");
  if (!file) {
    return request->reply(404, "text/plain", "File not found");
  }
  String fileContent;
  while (file.available()) {
    fileContent += (char)file.read();
  }
  file.close();
  // Process the file content using the processor function
  String processedContent = processFile(fileContent);
  return request->reply(processedContent.c_str());
}

void onOTAStart() {
  // Log when OTA has started
  Serial.println("OTA update started!");
}

void onOTAProgress(size_t current, size_t final) {
  // Log every 1 second
  if (millis() - ota_progress_millis > 1000) {
    ota_progress_millis = millis();
    Serial.printf("OTA Progress Current: %u bytes, Final: %u bytes\n", current, final);
  }
}

void onOTAEnd(bool success) {
  // Log when OTA has finished
  if (success) {
    Serial.println("OTA update finished successfully!");
  } else {
    Serial.println("There was an error during OTA update!");
  }
}

void startWebserver(){
  
  // Initialize LittleFS
  if(!LittleFS.begin(true)){
    Serial.println("An Error has occurred while mounting LittleFS");
    return;
  }

  // Init time by NTP Client
  configTime(gmtOffset_sec, daylightOffset_sec, settingsManager.getAppSettings().ntpServer.c_str());

  // Load web page log in credentials
  WebPageSettings webPageSettings = settingsManager.getWebPageSettings();

  //optional low level setup server config stuff here.
  //server.config is an ESP-IDF httpd_config struct
  //see: https://docs.espressif.com/projects/esp-idf/en/v4.4.6/esp32/api-reference/protocols/esp_http_server.html#_CPPv412httpd_config
  //increase maximum number of uri endpoint handlers (.on() calls)
  webServer.config.max_uri_handlers = 20;

  //look up our keys?
  #ifdef PSY_ENABLE_SSL
    if (app_enable_ssl)
    {
      File fp = LittleFS.open("/server.crt");
      if (fp){
        server_cert = fp.readString();
      } else {
        Serial.println("server.pem not found, SSL not available");
        app_enable_ssl = false;
      }
      fp.close();

      File fp2 = LittleFS.open("/server.key");
      if (fp2) {
        server_key = fp2.readString();
      } else {
        Serial.println("server.key not found, SSL not available");
        app_enable_ssl = false;
      }
      fp2.close();
    }
  #endif


  // Start server
  #ifdef PSY_ENABLE_SSL
    if (app_enable_ssl)
      {
        webServer.ssl_config.httpd.max_uri_handlers = 20; //maximum number of uri handlers (.on() calls)

        webServer.listen(443, server_cert.c_str(), server_key.c_str());
        //this creates a 2nd server listening on port 80 and redirects all requests HTTPS
        PsychicHttpServer *redirectServer = new PsychicHttpServer();
        redirectServer->config.ctrl_port = 20420; // just a random port different from the default one
        redirectServer->listen(80);
        redirectServer->onNotFound([](PsychicRequest *request){
          String url = "https://" + request->host() + request->url();
          return request->redirect(url.c_str());
        });
      } else {
        webServer.listen(80);
      }
  #else
    webServer.listen(80);
  #endif

  // Set Authentication Credentials
  ElegantOTA.setAuth(settingsManager.getWebPageSettings().webPageUsername.c_str(), settingsManager.getWebPageSettings().webPagePassword.c_str());

  // Enable Over-the-air updates at http://<IPAddress>/update
  ElegantOTA.begin(&webServer);    // Start ElegantOTA
  // ElegantOTA callbacks
  ElegantOTA.onStart(onOTAStart);
  ElegantOTA.onProgress(onOTAProgress);
  ElegantOTA.onEnd(onOTAEnd);
  
  // webserver for normal operation mode or WiFi config mode?
  if (currentMode == Mode::wificonfig)
  {
    // =================
    // WiFi config mode
    // =================

    webServer.on("/", HTTP_GET, [](PsychicRequest *request){
      return sendHTML(request, "/wificonfig.html");
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");

    webServer.on("/save", HTTP_GET, [](PsychicRequest *request){
      if(request->hasParam("hostname")){
        Serial.println("Save wifi config");
        WifiSettings settings = settingsManager.getWifiSettings();
        settings.hostname = request->getParam("hostname")->value();
        settings.ssid = request->getParam("ssid")->value();
        if (request->getParam("password")->value().equals("********")) // password is replaced by wildcards when given to the browser, so if the user didn't changed it, don't save it
          settings.password = settingsManager.getWifiSettings().password; // use the old, already saved, one
        else
          settings.password = request->getParam("password")->value();
        settingsManager.saveWifiSettings(settings);
        shouldReboot = true;
      }
      return request->redirect("/");
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");
  }
  else
  {
    // =======================
    // normal operation mode
    // =======================

    events.onOpen([](PsychicEventSourceClient *client){
      if(client->lastId()){
        Serial.printf("Client reconnected! Last message ID it got was: %u\n", client->lastId());
      }
      // send event with message "ready", id current millis
      // and set reconnect delay to 1 second
      client->send(getLogMessagesAsHtml().c_str(),"message",millis(),1000);
    });

    webServer.on("/", HTTP_GET, [](PsychicRequest *request){
      return sendHTML(request, "/index.html");
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");

    webServer.on("/enroll", HTTP_GET, [webPageSettings](PsychicRequest *request){
      if(request->hasParam("startEnrollment")){
        enrollId = request->getParam("newFingerprintId")->value();
        enrollName = request->getParam("newFingerprintName")->value();
        currentMode = Mode::enroll;
      }
      return request->redirect("/");
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");

    webServer.on("/editFingerprints", HTTP_GET, [webPageSettings](PsychicRequest *request){
      if(request->hasParam("selectedFingerprint")){
        if(request->hasParam("btnDelete"))
        {
          int id = request->getParam("selectedFingerprint")->value().toInt();
          waitForMaintenanceMode();
          fingerManager.deleteFinger(id);
          currentMode = Mode::scan;
        }
        else if (request->hasParam("btnRename"))
        {
          int id = request->getParam("selectedFingerprint")->value().toInt();
          String newName = request->getParam("renameNewName")->value();
          fingerManager.renameFinger(id, newName);
        }
      }
      return request->redirect("/");
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");

    webServer.on("/colorSettings", HTTP_GET, [webPageSettings](PsychicRequest *request){
      if(request->hasParam("btnSaveColorSettings")){
        Serial.println("Save color and sequence settings");
        ColorSettings colorSettings = settingsManager.getColorSettings();
        colorSettings.activeColor = (uint8_t) request->getParam("activeColor")->value().toInt();
        colorSettings.activeSequence = (uint8_t) request->getParam("activeSequence")->value().toInt();
        colorSettings.scanColor = (uint8_t) request->getParam("scanColor")->value().toInt();
        colorSettings.scanSequence = (uint8_t) request->getParam("scanSequence")->value().toInt();
        colorSettings.matchColor = (uint8_t) request->getParam("matchColor")->value().toInt();
        colorSettings.matchSequence = (uint8_t) request->getParam("matchSequence")->value().toInt();
        colorSettings.enrollColor = (uint8_t) request->getParam("enrollColor")->value().toInt();
        colorSettings.enrollSequence = (uint8_t) request->getParam("enrollSequence")->value().toInt();
        colorSettings.connectColor = (uint8_t) request->getParam("connectColor")->value().toInt();
        colorSettings.connectSequence = (uint8_t) request->getParam("connectSequence")->value().toInt();
        colorSettings.wifiColor = (uint8_t) request->getParam("wifiColor")->value().toInt();
        colorSettings.wifiSequence = (uint8_t) request->getParam("wifiSequence")->value().toInt();
        colorSettings.errorColor = (uint8_t) request->getParam("errorColor")->value().toInt();
        colorSettings.errorSequence = (uint8_t) request->getParam("errorSequence")->value().toInt();
        settingsManager.saveColorSettings(colorSettings);
        shouldReboot = true;
        return request->redirect("/colorSettings");
      } else {
        return sendHTML(request, "/colorSettings.html");
      }
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");

    webServer.on("/wifiSettings", HTTP_GET, [webPageSettings](PsychicRequest *request){
      if(request->hasParam("btnSaveWiFiSettings")){
        Serial.println("Save wifi config");
        WifiSettings settings = settingsManager.getWifiSettings();
        settings.hostname = request->getParam("hostname")->value();
        settings.ssid = request->getParam("ssid")->value();
        if (request->getParam("password")->value().equals("********")) // password is replaced by wildcards when given to the browser, so if the user didn't changed it, don't save it
          settings.password = settingsManager.getWifiSettings().password; // use the old, already saved, one
        else
          settings.password = request->getParam("password")->value();
        settings.dhcp_setting = request->getParam("dhcp_setting")->value().equals("1");
        if(!settings.localIP.fromString(request->getParam("local_ip")->value()))
          Serial.println("Local IP address could not be parsed.");
        if(!settings.gatewayIP.fromString(request->getParam("gateway_ip")->value()))
          Serial.println("Gateway IP address could not be parsed.");
        if(!settings.subnetMask.fromString(request->getParam("subnet_mask")->value()))
          Serial.println("Subnet mask could not be parsed.");
        if(!settings.dnsIP0.fromString(request->getParam("dns_ip0")->value()))
          Serial.println("DNS server 1 could not be parsed.");
        if(!settings.dnsIP1.fromString(request->getParam("dns_ip1")->value()))
          Serial.println("DNS server 2 could not be parsed.");
        settingsManager.saveWifiSettings(settings);
        shouldReboot = true;
        return request->redirect("/wifiSettings");
      } else {
        return sendHTML(request, "/wifiSettings.html");
      }
      return request->redirect("/wifiSettings");
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");

    webServer.on("/settings", HTTP_GET, [webPageSettings](PsychicRequest *request){
      if(request->hasParam("btnSaveSettings")){
        Serial.println("Save settings");
        AppSettings settings = settingsManager.getAppSettings();
        settings.mqttServer = request->getParam("mqtt_server")->value();
        String mqttPortString = request->getParam("mqtt_port")->value();
        settings.mqttPort = (uint16_t) mqttPortString.toInt();
        settings.mqttUsername = request->getParam("mqtt_username")->value();
        settings.mqttPassword = request->getParam("mqtt_password")->value();
        settings.mqttRootTopic = request->getParam("mqtt_rootTopic")->value();
        settings.ntpServer = request->getParam("ntpServer")->value();
        settingsManager.saveAppSettings(settings);
        shouldReboot = true;
        return request->redirect("/settings");
      } else if(request->hasParam("btnSaveWebPageSettings"))
      {
        Serial.println("Save web page settings");
        WebPageSettings webPageSettings = settingsManager.getWebPageSettings();
        webPageSettings.webPageUsername = request->getParam("webpage_username")->value();
        webPageSettings.webPagePassword = request->getParam("webpage_password")->value();
        settingsManager.saveWebPageSettings(webPageSettings);
        shouldReboot = true;
        return request->redirect("/settings");
      } else {
        return sendHTML(request, "/settings.html");
      }
      return request->redirect("/settings");
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");

    webServer.on("/pairing", HTTP_GET, [webPageSettings](PsychicRequest *request){
      if(request->hasParam("btnDoPairing"))
      {
        Serial.println("Do (re)pairing");
        doPairing();
        return request->redirect("/");  
      } else {
        return sendHTML(request, "/settings.html");
      }
      return request->redirect("/");
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");

    webServer.on("/factoryReset", HTTP_GET, [webPageSettings](PsychicRequest *request){
      if(request->hasParam("btnFactoryReset")){
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

        shouldReboot = true;
        return request->redirect("/");  
      } else {
        return sendHTML(request, "/settings.html");
      }
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");

    webServer.on("/deleteAllFingerprints", HTTP_GET, [webPageSettings](PsychicRequest *request){
      if(request->hasParam("btnDeleteAllFingerprints")){
        notifyClients("Deleting all fingerprints...");
        
        if (!fingerManager.deleteAll())
          notifyClients("Finger database could not be deleted.");
        
        return request->redirect("/");
        
      } else {
        return sendHTML(request, "/settings.html");
      }
      return request->redirect("/");
    })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");
  } // end normal operating mode

  // common url callbacks
  webServer.on("/reboot", HTTP_GET, [webPageSettings](PsychicRequest *request){
    shouldReboot = true;
    return request->redirect("/");
  })->setAuthentication(webPageSettings.webPageUsername.c_str(), webPageSettings.webPagePassword.c_str(), BASIC_AUTH, webPageSettings.webPageRealm.c_str(), "You must log in.");

  webServer.on("/bootstrap.min.css", HTTP_GET, [](PsychicRequest *request){
    String filename = "/bootstrap.min.css";
    PsychicFileResponse response(request, LittleFS, filename, "text/css");
    return response.send();
  });

  webServer.on("/logout", HTTP_GET, [](PsychicRequest *request){
    File file = LittleFS.open("/logout.html", "r");
    if (!file) {
      return request->reply(401, "text/plain", "Successfully logged out!");
    }
    String fileContent;
    while (file.available()) {
      fileContent += (char)file.read();
    }
    file.close();
    // Process the file content using the processor function
    String processedContent = processFile(fileContent);
    return request->reply(401, "text/html", processedContent.c_str());
  });

  webServer.onNotFound([](PsychicRequest *request){
    return request->redirect("/");
  });

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
  webServer.stop();
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
