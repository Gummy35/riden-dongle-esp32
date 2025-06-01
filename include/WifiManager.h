#pragma once

#include <LittleFS.h>
#include <riden_logging/riden_logging.h>

namespace RidenDongle
{

class WifiManagerClass
{
  public:
    void begin();
    bool saveCredentials();
    void setCredentials(const String &ssid, const String &password);
    void connect();
    void keepAlive();
    bool readCredentials();
    bool clearCredentials();
    void startAPMode();
    String ssid = "";
  private:
    String _password = "";
};

extern WifiManagerClass WifiManager;

} // namespace RidenDongle
