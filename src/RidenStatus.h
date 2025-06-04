#ifndef RidenStatus_h
#define RidenStatus_h

#include "Arduino.h"
#include "stdlib_noniso.h"

#include "AsyncTCP.h"
#include "ESPAsyncWebServer.h"
#include "WiFi.h"
#include <riden_modbus/riden_modbus.h>

// typedef std::function<void(uint8_t *data, size_t len)> WSLMessageHandler;
// typedef std::function<void(const String& msg)> WSLStringMessageHandler;
namespace RidenDongle
{
class RidenStatusClass
{
  public:
    void begin(AsyncWebServer *server, RidenModbus *modbus, const char *url = "/ridenstatus");
    void start();
    void end();
    size_t getConnectionCount();
    void sendStatus();
    bool updateStatus(bool full=false);
    void loop();

  private:
    AsyncWebServer *_server;
    RidenModbus *_modbus;
    AsyncWebSocket *_ws;
    TaskHandle_t _xHandle;
    RidenDongle::AllValues _allValues;
    bool _started = false;
    String _serializeAllValues(const RidenDongle::AllValues& values);
};

} // namespace RidenDongle

extern RidenDongle::RidenStatusClass RidenStatus;

#endif