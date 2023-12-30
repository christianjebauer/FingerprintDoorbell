#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <Preferences.h>
#include "global.h"

struct WifiSettings {    
    String ssid = "";
    String password = "";
    String hostname = "";
};

struct AppSettings {
    String mqttServer = "";
    String mqttUsername = "";
    String mqttPassword = "";
    uint16_t mqttPort = 1883;
    String mqttRootTopic = "fingerprintDoorbell";
    String ntpServer = "pool.ntp.org";
    String sensorPin = "00000000";
    String sensorPairingCode = "";
    bool   sensorPairingValid = false;
};

struct WebPageSettings {
    String webPageUsername = "admin";
    String webPagePassword = "admin";
    String webPageRealm = "FingerprintDoorbell";
};

class SettingsManager {       
  private:
    WifiSettings wifiSettings;
    WebPageSettings webPageSettings;
    AppSettings appSettings;

    void saveWifiSettings();
    void saveWebPageSettings();
    void saveAppSettings();

  public:
    bool loadWifiSettings();
    bool loadWebPageSettings();
    bool loadAppSettings();

    WifiSettings getWifiSettings();
    void saveWifiSettings(WifiSettings newSettings);

    WebPageSettings getWebPageSettings();
    void saveWebPageSettings(WebPageSettings newSettings);
    
    AppSettings getAppSettings();
    void saveAppSettings(AppSettings newSettings);

    bool isWifiConfigured();

    bool deleteAppSettings();
    bool deleteWebPageSettings();
    bool deleteWifiSettings();

    String generateNewPairingCode();

};

#endif