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

struct ColorSettings {
    int8_t activeColor = 2;
    int8_t activeSequence = 1;
    int8_t scanColor = 1;
    int8_t matchColor = 3;
};

struct WebPageSettings {
    String webPageUsername = "admin";
    String webPagePassword = "admin";
    String webPageRealm = "FingerprintDoorbell";
};

class SettingsManager {       
  private:
    WifiSettings wifiSettings;
    AppSettings appSettings;
    ColorSettings colorSettings;
    WebPageSettings webPageSettings;

    void saveWifiSettings();
    void saveAppSettings();
    void saveColorSettings();
    void saveWebPageSettings();

  public:
    bool loadWifiSettings();
    bool loadAppSettings();
    bool loadColorSettings();
    bool loadWebPageSettings();

    WifiSettings getWifiSettings();
    void saveWifiSettings(WifiSettings newSettings);
    
    AppSettings getAppSettings();
    void saveAppSettings(AppSettings newSettings);

    ColorSettings getColorSettings();
    void saveColorSettings(ColorSettings newSettings);

    WebPageSettings getWebPageSettings();
    void saveWebPageSettings(WebPageSettings newSettings);

    bool isWifiConfigured();

    bool deleteWifiSettings();
    bool deleteAppSettings();
    bool deleteColorSettings();
    bool deleteWebPageSettings();

    String generateNewPairingCode();

};

#endif
