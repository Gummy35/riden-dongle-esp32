#include "RidenStatus.h"
#include <assert.h>
#include <LittleFS.h>
#include <riden_modbus/riden_modbus_registers.h>

using namespace RidenDongle;

RidenStatusClass RidenStatus;

void RidenStatusClass::begin(AsyncWebServer *server, RidenModbus *modbus, const char *url)
{
    _server = server;
    _modbus = modbus;

    _ws = new AsyncWebSocket("/ws_ridenstatus");

    _server->on(url, HTTP_GET, [this](AsyncWebServerRequest *request) {
      request->send(LittleFS, "/html/riden_status.html", String());
    });

    _ws->onEvent([this](AsyncWebSocket *server, AsyncWebSocketClient *client, AwsEventType type, void *arg, uint8_t *data, size_t len) {
        switch (type) {
        case WS_EVT_CONNECT:
            Serial.printf("WebSocket client #%u connected from %s\n", client->id(), client->remoteIP().toString().c_str());
            client->setCloseClientOnQueueFull(false);
            break;
        case WS_EVT_DISCONNECT:
            Serial.printf("WebSocket client #%u disconnected\n", client->id());
            break;
        case WS_EVT_DATA:
            // AwsFrameInfo *info = (AwsFrameInfo*)arg;
            // if (info->final && info->index == 0 && info->len == len && info->opcode == WS_TEXT) {
            //   data[len] = 0;
            // }
            break;
        }
    });

    _server->addHandler(_ws);
}

size_t RidenStatusClass::getConnectionCount()
{
    return _ws->count();
}

static unsigned long lasttime = 0;
void RidenStatusClass::loop()
{
  unsigned long now = millis();
  if (now - lasttime > 200) {
      if (_started && getConnectionCount())
        if (updateStatus(false))
          sendStatus();
      lasttime = now;
  }
}

String RidenStatusClass::_serializeAllValues(const RidenDongle::AllValues& v) {
    String json = "{";

    // Scalars
    json += "\"system_temperature_celsius\":" + String(v.system_temperature_celsius) + ",";
    json += "\"system_temperature_fahrenheit\":" + String(v.system_temperature_fahrenheit) + ",";
    json += "\"voltage_set\":" + String(v.voltage_set) + ",";
    json += "\"current_set\":" + String(v.current_set) + ",";
    json += "\"voltage_out\":" + String(v.voltage_out) + ",";
    json += "\"current_out\":" + String(v.current_out) + ",";
    json += "\"power_out\":" + String(v.power_out) + ",";
    json += "\"voltage_in\":" + String(v.voltage_in) + ",";
    json += "\"keypad_locked\":" + String(v.keypad_locked ? "true" : "false") + ",";
    json += "\"protection\":" + String(static_cast<int>(v.protection)) + ",";
    json += "\"output_mode\":" + String(static_cast<int>(v.output_mode)) + ",";
    json += "\"output_on\":" + String(v.output_on ? "true" : "false") + ",";
    json += "\"current_range\":" + String(v.current_range) + ",";
    json += "\"is_battery_mode\":" + String(v.is_battery_mode ? "true" : "false") + ",";
    json += "\"voltage_battery\":" + String(v.voltage_battery) + ",";
    json += "\"probe_temperature_celsius\":" + String(v.probe_temperature_celsius) + ",";
    json += "\"probe_temperature_fahrenheit\":" + String(v.probe_temperature_fahrenheit) + ",";
    json += "\"ah\":" + String(v.ah) + ",";
    json += "\"wh\":" + String(v.wh) + ",";
    json += "\"voltage_max\":" + String(_modbus->get_max_voltage()) + ",";
    json += "\"current_max\":" + String(_modbus->get_max_current()) + ",";
    json += "\"is_take_ok\":" + String(v.is_take_ok ? "true" : "false") + ",";
    json += "\"is_take_out\":" + String(v.is_take_out ? "true" : "false") + ",";
    json += "\"is_power_on_boot\":" + String(v.is_power_on_boot ? "true" : "false") + ",";
    json += "\"is_buzzer_enabled\":" + String(v.is_buzzer_enabled ? "true" : "false") + ",";
    json += "\"is_logo\":" + String(v.is_logo ? "true" : "false") + ",";
    json += "\"language\":" + String(v.language) + ",";
    json += "\"brightness\":" + String(v.brightness) + ",";

    // Presets as array of objects
    json += "\"presets\":[";
    for (int i = 0; i < NUMBER_OF_PRESETS; ++i) {
        if (i > 0) json += ",";
        json += "{";
        json += "\"voltage\":" + String(v.presets[i].voltage) + ",";
        json += "\"current\":" + String(v.presets[i].current) + ",";
        json += "\"over_voltage_protection\":" + String(v.presets[i].over_voltage_protection) + ",";
        json += "\"over_current_protection\":" + String(v.presets[i].over_current_protection);
        json += "}";
    }
    json += "]";

    json += "}";

    return json;
}


void RidenStatusClass::sendStatus()
{
    _ws->textAll(_serializeAllValues(_allValues));
}

bool RidenStatusClass::updateStatus(bool full)
{
    return _modbus->get_all_values(_allValues, !full);
}

void RidenStatusClass::end()
{
  _started = false;
}

void RidenStatusClass::start()
{
  _started = true;
  updateStatus(true);
}
