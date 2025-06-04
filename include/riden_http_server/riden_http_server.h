#pragma once

#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <AsyncJson.h>
#include <ArduinoJson.h>

#include <riden_modbus/riden_modbus.h>
#include <riden_modbus_bridge/riden_modbus_bridge.h>
#include <riden_scpi/riden_scpi.h>
#include <vxi11_server/vxi_server.h>

#define HTTP_RAW_PORT 80

namespace RidenDongle
{

class RidenHttpServer
{
  public:
    RidenHttpServer(RidenModbus* modbus, RidenScpi* scpi, RidenModbusBridge* bridge, VXI_Server* vxi_server);
    void advertiseMDNS();
    bool begin();
    void loop(void);
    uint16_t port();
    void showIp();

  private:
    RidenModbus* _modbus;
    RidenScpi* _scpi;
    RidenModbusBridge* _bridge;
    VXI_Server* _vxi_server;

    void handle_root_get(AsyncWebServerRequest *request);
    void handle_psu_get(AsyncWebServerRequest *request);
    void handle_config_get(AsyncWebServerRequest *request);
    void handle_config_post(AsyncWebServerRequest *request);
    void handle_disconnect_client_post(AsyncWebServerRequest *request);
    void handle_reboot_dongle_get(AsyncWebServerRequest *request);
    void handle_firmware_update_post(AsyncWebServerRequest *request);
    void finish_firmware_update_post(AsyncWebServerRequest *request);
    void handle_lxi_identification(AsyncWebServerRequest *request);
    void handle_not_found(AsyncWebServerRequest *request);

    void handle_modbus_qps(AsyncWebServerRequest *request);
    void send_redirect_root(AsyncWebServerRequest *request);
    void send_redirect_self(AsyncWebServerRequest *request);

    void send_dongle_info(AsyncResponseStream *response);
    void send_network_info(AsyncResponseStream *response);
    void send_services(AsyncResponseStream *response);
    void send_power_supply_info(AsyncResponseStream *response);
    void send_connected_clients(AsyncResponseStream *response);

    //void send_as_chunks(AsyncResponseStream *response, const char *str);
    void send_info_row(AsyncResponseStream *response, const String key, const String value);
    void send_client_row(AsyncResponseStream *response, const IPAddress &ip, const String protocol);

    const char *get_firmware_version();
    const char *get_serial_number();

    bool _readWiFiCredentials(String &ssid, String &password);
    void _startAPMode();
    void _connectToWiFi(const String &ssid, const String &password);
    bool _writeWiFiCredentials(const String &ssid, const String &password);
    void _onOTAEnd(bool success);
    void _handleGetJsonFile(AsyncWebServerRequest *request, const char *filename);
    void _saveJsonToFile(AsyncWebServerRequest *request, JsonVariant &json, const char *filename);
    void _handleSaveWiFi(AsyncWebServerRequest *request);
    void _handleClearWiFi(AsyncWebServerRequest *request);
    void _handlePage(AsyncWebServerRequest *request, const char *pagePath = "/html/index.html");
    String _htmlProcessor(const String &var);
    void _handlePsuConfigPage(AsyncWebServerRequest *request);
    String _htmlPsuConfigPageProcessor(const String &var);

    AsyncWebServer *_server;    
};

} // namespace RidenDongle

