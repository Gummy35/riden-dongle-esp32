#include "RidenStatus.h"
#include <assert.h>
#include <ArduinoJson.h>
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

String RidenStatusClass::_serializeAllValues(const RidenDongle::AllValues& values) {
    JsonDocument doc;

    doc["system_temperature_celsius"] = values.system_temperature_celsius;
    doc["system_temperature_fahrenheit"] = values.system_temperature_fahrenheit;
    doc["voltage_set"] = values.voltage_set;
    doc["current_set"] = values.current_set;
    doc["voltage_out"] = values.voltage_out;
    doc["current_out"] = values.current_out;
    doc["power_out"] = values.power_out;
    doc["voltage_in"] = values.voltage_in;
    doc["keypad_locked"] = values.keypad_locked;
    doc["protection"] = static_cast<int>(values.protection);
    doc["output_mode"] = static_cast<int>(values.output_mode);
    doc["output_on"] = values.output_on;
    doc["current_range"] = values.current_range;
    doc["is_battery_mode"] = values.is_battery_mode;
    doc["voltage_battery"] = values.voltage_battery;
    doc["probe_temperature_celsius"] = values.probe_temperature_celsius;
    doc["probe_temperature_fahrenheit"] = values.probe_temperature_fahrenheit;
    doc["ah"] = values.ah;
    doc["wh"] = values.wh;
    //Note: La structure `tm` n'est pas directement sérialisable, vous devrez la convertir en un format approprié
    doc["clock"] = "clock_data"; // Remplacez par une conversion appropriée de `tm` en chaîne

    // Sérialiser la structure Calibration
    JsonObject calibration = doc.createNestedObject("calibration");
    calibration["V_OUT_ZERO"] = values.calibration.V_OUT_ZERO;
    calibration["V_OUT_SCALE"] = values.calibration.V_OUT_SCALE;
    calibration["V_BACK_ZERO"] = values.calibration.V_BACK_ZERO;
    calibration["V_BACK_SCALE"] = values.calibration.V_BACK_SCALE;
    calibration["I_OUT_ZERO"] = values.calibration.I_OUT_ZERO;
    calibration["I_OUT_SCALE"] = values.calibration.I_OUT_SCALE;
    calibration["I_BACK_ZERO"] = values.calibration.I_BACK_ZERO;
    calibration["I_BACK_SCALE"] = values.calibration.I_BACK_SCALE;

    doc["is_take_ok"] = values.is_take_ok;
    doc["is_take_out"] = values.is_take_out;
    doc["is_power_on_boot"] = values.is_power_on_boot;
    doc["is_buzzer_enabled"] = values.is_buzzer_enabled;
    doc["is_logo"] = values.is_logo;
    doc["language"] = values.language;
    doc["brightness"] = values.brightness;

    for (int i = 0; i < NUMBER_OF_PRESETS; ++i) {
        String presetKey = "preset_" + String(i);
        JsonObject preset = doc.createNestedObject(presetKey);
        preset["voltage"] = values.presets[i].voltage;
        preset["current"] = values.presets[i].current;
        preset["over_voltage_protection"] = values.presets[i].over_voltage_protection;
        preset["over_current_protection"] = values.presets[i].over_current_protection;
    }

    String jsonString;
    serializeJson(doc, jsonString);
    return jsonString;
}

void RidenStatusClass::sendStatus()
{
    _ws->textAll(_serializeAllValues(_allValues));
}

bool RidenStatusClass::updateStatus(bool full)
{
  if (full)
    return _modbus->get_all_values(_allValues);
  else {
    return _modbus->get_live_values(_allValues);
  }
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
