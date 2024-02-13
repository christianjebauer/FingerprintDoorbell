#ifndef SETTINGSMANAGER_H
#define SETTINGSMANAGER_H

#include <Preferences.h>
#include "global.h"

struct WifiSettings {    
    String ssid = "";
    String password = "";
    String hostname = "";
    bool dhcp_setting = true;
    IPAddress localIP;
    IPAddress gatewayIP;
    IPAddress subnetMask;
    IPAddress dnsIP0;
    IPAddress dnsIP1;
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
    // Red - 1
    // Blue - 2
    // Purple - 3
    // Green - 4
    // Yellow - 5
    // Cyan - 6
    // White - 7
    uint8_t activeColor = 2;
    uint8_t scanColor = 1;
    uint8_t matchColor = 3;
    uint8_t enrollColor = 3;
    u_int8_t connectColor = 2;
    uint8_t wifiColor = 1;
    uint8_t errorColor = 1;

    // Breath - 1
    // Blink - 2
    // On - 3
    // Off - 4
    uint8_t activeSequence = 1;
    uint8_t scanSequence = 2;
    uint8_t matchSequence = 3;
    uint8_t enrollSequence = 2;
    uint8_t connectSequence = 2;
    uint8_t wifiSequence = 1;
    uint8_t errorSequence = 3;
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
