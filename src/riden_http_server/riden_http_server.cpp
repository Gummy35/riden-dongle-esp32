// SPDX-FileCopyrightText: 2024 Peder Toftegaard Olsen
//
// SPDX-License-Identifier: MIT

#include <ElegantOTA.h>
#include <WebSerial.h>
#include <RidenStatus.h>
#include <Logger.h>
#include <WifiManager.h>
#include "http_static.h"
#include <riden_config/riden_config.h>
#include <riden_http_server/riden_http_server.h>
#include <riden_logging/riden_logging.h>
#include <vxi11_server/vxi_server.h>

#include <ESPmDNS.h>
#include <TinyTemplateEngineMemoryReader.h>
#include <list>

using namespace RidenDongle;

static const String scpi_protocol = "SCPI RAW";
static const String modbustcp_protocol = "Modbus TCP";
static const String vxi11_protocol = "VXI-11";
static const std::list<uint32_t> uart_baudrates = {
    9600,
    19200,
    38400,
    57600,
    115200,
    230400,
    250000,
    460800,
    921600,
    1000000,
};

static String voltage_to_string(double voltage)
{
    if (voltage < 1) {
        return String(voltage * 1000, 0) + " mV";
    } else {
        return String(voltage, 3) + " V";
    }
}

static String current_to_string(double current)
{
    if (current < 1) {
        return String(current * 1000, 0) + " mA";
    } else {
        return String(current, 3) + " A";
    }
}

static String power_to_string(double power)
{
    if (power < 1) {
        return String(power * 1000, 0) + " mW";
    } else {
        return String(power, 3) + " W";
    }
}

static String protection_to_string(Protection protection)
{
    switch (protection) {
    case Protection::OVP:
        return "OVP";
    case Protection::OCP:
        return "OCP";
    default:
        return "None";
    }
}

static String outputmode_to_string(OutputMode output_mode)
{
    switch (output_mode) {
    case OutputMode::CONSTANT_VOLTAGE:
        return "Constant Voltage";
    case OutputMode::CONSTANT_CURRENT:
        return "Constant Current";
    default:
        return "Unknown";
    }
}

static String language_to_string(uint16_t language_id)
{
    String language;
    switch (language_id) {
    case 0:
        language = "English";
        break;
    case 1:
        language = "Chinese";
        break;
    case 2:
        language = "German";
        break;
    case 3:
        language = "French";
        break;
    case 4:
        language = "Russian";
        break;
    default:
        language = "Unknown (" + String(language_id, 10) + ")";
        break;
    }
    return language;
}

RidenHttpServer::RidenHttpServer(RidenModbus* modbus, RidenScpi* scpi, RidenModbusBridge* bridge, VXI_Server* vxi_server)
{
    this->_modbus = modbus;
    this->_scpi = scpi;
    this->_bridge = bridge;
    this->_vxi_server = vxi_server;
    this->_server = new AsyncWebServer(port());
}

void RidenHttpServer::advertiseMDNS()
{
    if (/*MDNS.isRunning() && */ _modbus->is_connected()) {
        auto lxi_service = MDNS.addService("lxi", "tcp", port()); // allows discovery by lxi-tools
        MDNS.addServiceTxt("lxi", "tcp", "path", "/");
        auto http_service = MDNS.addService("http", "tcp", port());
        MDNS.addServiceTxt("http", "tcp",  "path", "/");
    }
}

bool RidenHttpServer::begin()
{
    WifiManager.begin();

    _server->on("/", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->_handlePage(request, "/html/index.html"); });
    _server->on("/wifi", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->_handlePage(request, "/html/wifi.html"); });
    _server->on("/savewifi", HTTP_POST, [this](AsyncWebServerRequest *request)
                { this->_handleSaveWiFi(request); });
    _server->on("/clearwifi", HTTP_POST, [this](AsyncWebServerRequest *request)
                { this->_handleClearWiFi(request); });
    _server->serveStatic("/static/", LittleFS, "/html/");

    ElegantOTA.onEnd([this](bool success)
                    { this->_onOTAEnd(success); });

    WebSerial.begin(_server);
    ElegantOTA.begin(_server);
    RidenStatus.begin(_server, _modbus);

    _server->on("/status", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->handle_root_get(request); });
    _server->on("/psu/", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->handle_psu_get(request); });
    _server->on("/config", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->_handlePsuConfigPage(request); });
    _server->on("/config/", HTTP_POST, [this](AsyncWebServerRequest *request)
                { this->handle_config_post(request); });
    server.on("/control/", HTTPMethod::HTTP_GET, std::bind(&RidenHttpServer::handle_control_get, this));
    server.on("/status", HTTPMethod::HTTP_GET, std::bind(&RidenHttpServer::handle_status_get, this));
    server.on("/set_i", HTTPMethod::HTTP_POST, std::bind(&RidenHttpServer::handle_set_i, this));
    server.on("/set_v", HTTPMethod::HTTP_POST, std::bind(&RidenHttpServer::handle_set_v, this));
    server.on("/toggle_out", HTTPMethod::HTTP_GET, std::bind(&RidenHttpServer::handle_toggle_out, this));

    _server->on("/disconnect_client/", HTTP_POST, [this](AsyncWebServerRequest *request)
                { this->handle_disconnect_client_post(request); });
    _server->on("/reboot/dongle/", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->handle_reboot_dongle_get(request); });
    _server->on("/lxi/identification", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->handle_lxi_identification(request); });
    _server->on("/qps/modbus/", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->handle_modbus_qps(request); });
    _server->onNotFound([this](AsyncWebServerRequest *request)
                { this->handle_not_found(request); });
    _server->begin();

    return true;
}


void RidenHttpServer::_onOTAEnd(bool success)
{
  // Log when OTA has finished
  if (success)
  {
    Logger.Log("OTA update complete");
    WifiManager.saveCredentials();
    LittleFS.end();
  }
  else
  {
    Serial.println("There was an error during OTA update!");
    if (Update.hasError()) {        
          uint8_t otaError = Update.getError();
          const char* otaErrorStr = Update.errorString();
          Serial.printf("OTA error %d : %s\n", otaError, otaErrorStr);
          WebSerial.printf("OTA error %d : %s\n", otaError, otaErrorStr);
    }
  }
}

void RidenHttpServer::_handleSaveWiFi(AsyncWebServerRequest *request)
{
  if (request->hasArg("ssid") && request->hasArg("password"))
  {
    String ssid = request->arg("ssid");
    String password = request->arg("password");

    WifiManager.setCredentials(ssid, password);
    if (WifiManager.saveCredentials())
    {
      request->send(200, "text/plain", "Credentials saved. Please restart the device.");
      LittleFS.end();
      ESP.restart();
    }
    else
    {
      request->send(500, "text/plain", "Failed to save credentials.");
    }
  }
  else
  {
    request->send(400, "text/plain", "Invalid request.");
  }
}

void RidenHttpServer::_handleClearWiFi(AsyncWebServerRequest *request)
{    
  if (WifiManager.clearCredentials())
  {
    request->send(200, "text/plain", "Credentials cleared.");
  }
  else
  {
    request->send(500, "text/plain", "Failed to clear credentials.");
  }
}


String RidenHttpServer::_htmlProcessor(const String &var)
{
  if (var == "WIFI_SSID")
    return WifiManager.ssid;

  return String();
}

void RidenHttpServer::_handlePage(AsyncWebServerRequest *request, const char *pagePath)
{
  request->send(LittleFS, pagePath, String(), false, [this](const String &str)
                { return this->_htmlProcessor(str); });
}

void RidenHttpServer::showIp()
{
  if (WiFi.getMode() == WIFI_MODE_STA)
    Logger.Log(WiFi.localIP().toString().c_str());
  else if (WiFi.getMode() == WIFI_MODE_AP)
    Logger.Log(WiFi.softAPIP().toString().c_str());
}

void RidenHttpServer::loop(void)
{
    //_server->handleClient();
    WebSerial.loop();
    ElegantOTA.loop();
    RidenStatus.loop();
}

uint16_t RidenHttpServer::port()
{
    return HTTP_RAW_PORT;
}

void RidenHttpServer::handle_root_get(AsyncWebServerRequest *request)
{
    AsyncResponseStream *response = request->beginResponseStream("text/html");
    response->print(HTML_HEADER);

    if (_modbus->is_connected()) {
        send_dongle_info(response);
        send_power_supply_info(response);
        send_network_info(response);
        send_services(response);
        send_connected_clients(response);
    } else {        
        response->print(HTML_NO_CONNECTION_BODY);
    }
    response->print(HTML_FOOTER);
    request->send(response);
}

void RidenHttpServer::handle_psu_get(AsyncWebServerRequest *request)
{
    AllValues all_values;
    
    AsyncResponseStream *response = request->beginResponseStream("text/html");

    if (_modbus->is_connected() && _modbus->get_all_values(all_values)) {
        response->print("<div class='box'>");
        response->print("<a style='float:right' href='.'>Refresh</a><h2>Power Supply Details</h2>");
        response->print("<table class='info'>");
        response->print("<tbody>");
        send_info_row(response, "Output", all_values.output_on ? "On" : "Off");
        send_info_row(response, "Set", voltage_to_string(all_values.voltage_set) + " / " + current_to_string(all_values.current_set));
        send_info_row(response, "Out",
                      voltage_to_string(all_values.voltage_out) + " / " + current_to_string(all_values.current_out) + " / " + power_to_string(all_values.power_out));
        send_info_row(response, "Protection", protection_to_string(all_values.protection));
        send_info_row(response, "Output Mode", outputmode_to_string(all_values.output_mode));
        send_info_row(response, "Current Range", String(all_values.current_range, 10));
        send_info_row(response, "Battery Mode", all_values.is_battery_mode ? "Yes" : "No");
        send_info_row(response, "Voltage Battery", voltage_to_string(all_values.voltage_battery));
        send_info_row(response, "Ah", String(all_values.ah, 3) + " Ah");
        send_info_row(response, "Wh", String(all_values.wh, 3) + " Wh");
        response->print("</tbody>");
        response->print("</table>");
        response->print("</div>");

        response->print("<div class='box'>");
        response->print("<h2>Environment</h2>");
        response->print("<table class='info'>");
        response->print("<tbody>");
        send_info_row(response, "Voltage In", voltage_to_string(all_values.voltage_in));
        send_info_row(response, "System Temperature", String(all_values.system_temperature_celsius, 0) + "&deg;C" + " / " + String(all_values.system_temperature_fahrenheit, 0) + "&deg;F");
        send_info_row(response, "Probe Temperature", String(all_values.probe_temperature_celsius, 0) + "&deg;C" + " / " + String(all_values.probe_temperature_fahrenheit, 0) + "&deg;F");
        response->print("</tbody>");
        response->print("</table>");
        response->print("</div>");

        response->print("<div class='box'>");
        response->print("<h2>Settings</h2>");
        response->print("<table class='info'>");
        response->print("<tbody>");
        send_info_row(response, "Keypad Locked", all_values.keypad_locked ? "Yes" : "No");
        char clock_string[20];
        sprintf(clock_string, "%04u-%02u-%02u %02u:%02u:%02u",
                all_values.clock.tm_year + 1900,
                all_values.clock.tm_mon + 1,
                all_values.clock.tm_mday,
                all_values.clock.tm_hour,
                all_values.clock.tm_min,
                all_values.clock.tm_sec);
        send_info_row(response, "Time", clock_string);
        send_info_row(response, "Take OK", all_values.is_take_ok ? "Yes" : "No");
        send_info_row(response, "Take Out", all_values.is_take_out ? "Yes" : "No");
        send_info_row(response, "Power on boot", all_values.is_power_on_boot ? "Yes" : "No");
        send_info_row(response, "Buzzer enabled", all_values.is_buzzer_enabled ? "Yes" : "No");
        send_info_row(response, "Logo", all_values.is_logo ? "Yes" : "No");
        send_info_row(response, "Language", language_to_string(all_values.language));
        send_info_row(response, "Brightness", String(all_values.brightness, 10));
        response->print("</tbody>");
        response->print("</table>");
        response->print("</div>");

        response->print("<div class='box'>");
        response->print("<h2>Calibration</h2>");
        response->print("<table class='info'>");
        response->print("<tbody>");
        send_info_row(response, "V_OUT_ZERO", String(all_values.calibration.V_OUT_ZERO, 10));
        send_info_row(response, "V_OUT_SCALE", String(all_values.calibration.V_OUT_SCALE, 10));
        send_info_row(response, "V_BACK_ZERO", String(all_values.calibration.V_BACK_ZERO, 10));
        send_info_row(response, "V_BACK_SCALE", String(all_values.calibration.V_BACK_SCALE, 10));
        send_info_row(response, "I_OUT_ZERO", String(all_values.calibration.I_OUT_ZERO, 10));
        send_info_row(response, "I_OUT_SCALE", String(all_values.calibration.I_OUT_SCALE, 10));
        send_info_row(response, "I_BACK_ZERO", String(all_values.calibration.I_BACK_ZERO, 10));
        send_info_row(response, "I_BACK_SCALE", String(all_values.calibration.I_BACK_SCALE, 10));
        response->print("</tbody>");
        response->print("</table>");
        response->print("</div>");

        response->print("<div class='box'>");
        response->print("<h2>Presets</h2>");
        response->print("<table class='info'>");
        response->print("<tbody>");
        for (int preset = 0; preset < NUMBER_OF_PRESETS; preset++) {
            response->print("<tr><th colspan='2' style='text-align:left'>Preset " + String(preset + 1, 10) + " (M" + String(preset + 1, 10) + ")" + "</th></tr>");
            send_info_row(response, "Preset Voltage", voltage_to_string(all_values.presets[preset].voltage));
            send_info_row(response, "Preset Current", current_to_string(all_values.presets[preset].current));
            send_info_row(response, "Preset OVP", voltage_to_string(all_values.presets[preset].over_voltage_protection));
            send_info_row(response, "Preset OCP", current_to_string(all_values.presets[preset].over_current_protection));
        }
        response->print("</tbody>");
        response->print("</table>");
        response->print("</div>");
    } else {
        response->print(HTML_NO_CONNECTION_BODY);
    }
    response->print(HTML_FOOTER);
    response->print("");
    request->send(response);
}


String RidenHttpServer::_htmlPsuConfigPageProcessor(const String &var)
{
  if (var == "TIMEZONE") {
    return riden_config.get_timezone_name();
  }
  else if (var == "UARTBAUDRATE") {
    return String(riden_config.get_uart_baudrate());
  }
  return String();
}

void RidenHttpServer::_handlePsuConfigPage(AsyncWebServerRequest *request)
{
  request->send(LittleFS, "/html/config.html", String(), false, [this](const String &str)
                { return this->_htmlPsuConfigPageProcessor(str); });
}

void RidenHttpServer::handle_config_post(AsyncWebServerRequest *request)
{
    String tz = request->arg("timezone");
    String uart_baudrate_string = request->arg("uart_baudrate");
    uint32_t uart_baudrate = std::strtoull(uart_baudrate_string.c_str(), nullptr, 10);
    LOG_F("Selected timezone: %s\r\n", tz.c_str());
    LOG_F("Selected baudrate: %u\r\n", uart_baudrate);
    riden_config.set_timezone_name(tz);
    riden_config.set_uart_baudrate(uart_baudrate);
    riden_config.commit();

    send_redirect_self(request);
}

void RidenHttpServer::handle_disconnect_client_post(AsyncWebServerRequest *request)
{
    String ip_string = request->arg("ip");
    String protocol = request->arg("protocol");
    IPAddress ip;
    if (ip.fromString(ip_string)) {
        if (protocol == scpi_protocol) {
            _scpi->disconnect_client(ip);
        } else if (protocol == modbustcp_protocol) {
            _bridge->disconnect_client(ip);
        } else if (protocol == vxi11_protocol) {
            _vxi_server->disconnect_client(ip);
        }
    }

    send_redirect_root(request);
}

void RidenHttpServer::handle_reboot_dongle_get(AsyncWebServerRequest *request)
{
    String config_arg = request->arg("config_portal");
    AsyncResponseStream *response = request->beginResponseStream("text/html");
    response->print(HTML_HEADER);
    if (config_arg == "true") {
        riden_config.set_config_portal_on_boot();
        riden_config.commit();
        response->print(HTML_REBOOTING_DONGLE_CONFIG_PORTAL_BODY_1);
        response->print(WiFi.getHostname());
        response->print(HTML_REBOOTING_DONGLE_CONFIG_PORTAL_BODY_2);
    } else {
        response->print(HTML_REBOOTING_DONGLE_BODY);
    }
    response->print(HTML_FOOTER);
    response->print("");
    delay(500);
    LittleFS.end();
    ESP.restart();
    delay(1000);
}


void RidenHttpServer::handle_control_get(void)
{
    server.setContentLength(CONTENT_LENGTH_UNKNOWN);
    server.send(200, "text/html", HTML_HEADER);
    server.sendContent_P(HTML_CONTROL_BODY);
    server.sendContent_P(HTML_FOOTER);
    server.sendContent("");
}

void RidenHttpServer::handle_status_get(void)
{
    AllValues all_values;
    // get a subset of the values, reading in bulk to be fast
    // Make sure this is below 800ms, because otherwise the graph will suffer
    if (modbus.is_connected() && modbus.get_all_values(all_values, true)) {
        String s = "{";
        s += "\"out_on\": " + String(all_values.output_on ? "true" : "false");
        s += ",\"set_v\": " + String(all_values.voltage_set, 3);
        s += ",\"set_c\": " + String(all_values.current_set, 3);
        s += ",\"out_v\": " + String(all_values.voltage_out, 3);
        s += ",\"out_c\": " + String(all_values.current_out, 3);
        s += ",\"batt_mode\": " + String(all_values.is_battery_mode ? "true" : "false");
        s += ",\"cvmode\": " + String(all_values.output_mode == OutputMode::CONSTANT_VOLTAGE ? "true" : "false");
        s += ",\"prot\": \"" + protection_to_string(all_values.protection) + "\"";
        s += ",\"batt_v\": " + String(all_values.voltage_battery, 3);
        if (all_values.probe_temperature_celsius < -50.0) {
            s += ",\"ext_t_c\": null";
        } else {
            s += ",\"ext_t_c\": " + String(all_values.probe_temperature_celsius, 2);
        }
        s += ",\"int_t_c\": " + String(all_values.system_temperature_celsius, 2);
        s += ",\"ah\": " + String(all_values.ah, 3);
        s += ",\"wh\": " + String(all_values.wh, 3);
        s += ",\"max_v\": " + String(modbus.get_max_voltage(), 3);
        s += ",\"max_c\": " + String(modbus.get_max_current(), 3);
        s += "}";
        server.send(200, "application/json", s);
    } else {
        server.send(500, "text/plain", "Not connected to power supply");
    }
    server.sendContent("");
}

void RidenHttpServer::handle_set_i() 
{
    String s = server.arg("plain");
    double v = std::strtod(s.c_str(), nullptr);
    if (modbus.is_connected() && modbus.set_current_set(v)) {
        server.send(200, "text/plain", "OK");
    } else {
        server.send(500, "text/plain", "Failed to set");
    }
}

void RidenHttpServer::handle_set_v()
{
    String s = server.arg("plain");
    double v = std::strtod(s.c_str(), nullptr);
    if (modbus.is_connected() && modbus.set_voltage_set(v)) {
        server.send(200, "text/plain", "OK");
    } else {
        server.send(500, "text/plain", "Failed to set");
    }
}

void RidenHttpServer::handle_toggle_out()
{
    if (modbus.is_connected()) {
        bool get_output_on;
        if (!modbus.get_output_on(get_output_on)) {
            get_output_on = false;
        }
        if (modbus.set_output_on(!get_output_on)) {
            // and reply with full data set
            handle_status_get();
        } else {
            server.send(500, "text/plain", "Failed to toggle output");
        }
    } else {
        server.send(500, "text/plain", "Not connected to power supply");
    }
}

void RidenHttpServer::send_redirect_root(AsyncWebServerRequest *request)
{
    AsyncResponseStream *response = request->beginResponseStream("text/html");
    response->print("<body>");
    response->print("<script>");
    response->print("  window.location = '/';");
    response->print("</script>");
    response->print("</body>");
    response->print("</html>");
    response->print("");
    request->send(response);
}

void RidenHttpServer::send_redirect_self(AsyncWebServerRequest *request)
{
    AsyncResponseStream *response = request->beginResponseStream("text/html");
    response->print("<html>");
    response->print("<body>");
    response->print("<script>");
    response->print("  location.replace('");
    response->print(request->url());
    response->print("');");
    response->print("</script>");
    response->print("</body>");
    response->print("</html>");
    response->print("");
    request->send(response);
}

void RidenHttpServer::send_dongle_info(AsyncResponseStream *response)
{
    response->print("        <div class='box'>");
    response->print("            <h2>Riden Dongle</h2>");
    response->print("            <table class='info'>");
    response->print("                <tbody>");
    send_info_row(response, "Version", RidenDongle::version_string);
    if (RidenDongle::build_time != nullptr) {
        send_info_row(response, "Build Time", RidenDongle::build_time);
    }
    response->print("                </tbody>");
    response->print("            </table>");
    response->print("        </div>");
}

void RidenHttpServer::send_power_supply_info(AsyncResponseStream *response)
{
    String type = _modbus->get_type();

    response->print("        <div class='box'>");
    response->print("            <a style='float:right' href='/psu/'>Details</a><h2>Power Supply</h2>");
    response->print("            <table class='info'>");
    response->print("                <tbody>");
    send_info_row(response, "Model", type);
    send_info_row(response, "Firmware", get_firmware_version());
    send_info_row(response, "Serial Number", get_serial_number());
    response->print("                </tbody>");
    response->print("            </table>");
    response->print("        </div>");
}

void RidenHttpServer::send_network_info(AsyncResponseStream *response)
{
    response->print("        <div class='box'>");
    response->print("            <h2>Network Configuration</h2>");
    response->print("            <table class='info'>");
    response->print("                <tbody>");
    send_info_row(response, "Hostname", WiFi.getHostname());
    send_info_row(response, "MDNS", String(WiFi.getHostname()) + ".local");
    send_info_row(response, "WiFi network", WiFi.SSID());
    send_info_row(response, "IP", WiFi.localIP().toString());
    send_info_row(response, "Subnet", WiFi.subnetMask().toString());
    send_info_row(response, "Default Gateway", WiFi.gatewayIP().toString());
    for (int i = 0;; i++) {
        auto dns = WiFi.dnsIP(i);
        if (dns == INADDR_NONE) {
            break;
        }
        send_info_row(response, "DNS", dns.toString());
    }
    response->print("                </tbody>");
    response->print("            </table>");
    response->print("        </div>");    
}

void RidenHttpServer::send_services(AsyncResponseStream *response)
{
    response->print("        <div class='box'>");
    response->print("            <h2>Network Services</h2>");
    response->print("            <table class='info'>");
    response->print("                <tbody>");
    send_info_row(response, "Web Server Port", String(this->port(), 10));
    send_info_row(response, "Modbus TCP Port", String(_bridge->port(), 10));
    send_info_row(response, "VXI-11 Port", String(_vxi_server->port(), 10));
    send_info_row(response, "SCPI RAW Port", String(_scpi->port(), 10));
    send_info_row(response, "VISA Resource Address VXI-11", _vxi_server->get_visa_resource());
    send_info_row(response, "VISA Resource Address RAW", _scpi->get_visa_resource());
    response->print("                </tbody>");
    response->print("            </table>");
    response->print("        </div>");
}

void RidenHttpServer::send_connected_clients(AsyncResponseStream *response)
{
    response->print("        <div class='box'>");
    response->print("            <h2>Connected Clients</h2>");
    response->print("            <table class='clients'>");
    response->print("                <thead><tr>");
    response->print("                <th>IP address</th>");
    response->print("                <th>Protocol</th>");
    response->print("                <th></th>");
    response->print("                </tr></thead>");
    response->print("                <tbody>");
    for (auto const &ip : _vxi_server->get_connected_clients()) {
        send_client_row(response, ip, vxi11_protocol);
    }    
    for (auto const &ip : _scpi->get_connected_clients()) {
        send_client_row(response, ip, scpi_protocol);
    }
    for (auto const &ip : _bridge->get_connected_clients()) {
        send_client_row(response, ip, modbustcp_protocol);
    }
    response->print("                </tbody>");
    response->print("            </table>");
    response->print("        </div>");
}

void RidenHttpServer::send_client_row(AsyncResponseStream *response, const IPAddress &ip, const String protocol)
{
    response->print("<tr>");
    response->print("<td>");
    response->print(ip.toString());
    response->print("</td>");
    response->print("<td>");
    response->print(protocol);
    response->print("</td>");
    response->print("<td><form method='post' action='/disconnect_client/'>");
    response->print("<input type='hidden' name='ip' value='" + ip.toString() + "'>");
    response->print("<input type='hidden' name='protocol' value='" + protocol + "'>");
    response->print("<input type='submit' value='Disconnect'>");
    response->print("</form></td>");
    response->print("</tr>");
}

void RidenHttpServer::send_info_row(AsyncResponseStream *response, const String key, const String value)
{
    response->print("                    <tr>");
    response->print("                        <th>");
    response->print(key);
    response->print("</th>");
    response->print("                        <td>");
    response->print(value);
    response->print("</td>");
    response->print("                    </tr>");
}

void RidenHttpServer::handle_not_found(AsyncWebServerRequest *request)
{
    request->send(404, "text/plain", "404: Not found");
}

void RidenHttpServer::handle_modbus_qps(AsyncWebServerRequest *request)
{
    AsyncResponseStream *response = request->beginResponseStream("text/html");
    response->print(HTML_HEADER);
    unsigned long start = millis();
    double voltage;
    for (int i = 0; i < 200; i++) {
        _modbus->get_voltage_set(voltage);
        yield();
    }
    unsigned long end = millis();
    double qps = 1000.0 * double(100) / double(end - start);
    LOG_F("qps = %f\r\n", qps);
    response->print("<p>Result = ");
    response->print(String(qps, 1));
    response->print(" queries/second</p>");
    response->print(HTML_FOOTER);
    response->print("");
    request->send(response);
}

void RidenHttpServer::handle_lxi_identification(AsyncWebServerRequest *request)
{
    String model = _modbus->get_type();
    String ip = WiFi.localIP().toString();
    String subnet_mask = WiFi.subnetMask().toString();
    String mac_address = WiFi.macAddress();
    String gateway = WiFi.gatewayIP().toString();
    // The values to be substituted
    const char *values[] = {
        model.c_str(),
        get_serial_number(),
        get_firmware_version(),
        WiFi.getHostname(),
        ip.c_str(),
        subnet_mask.c_str(),
        mac_address.c_str(),
        gateway.c_str(),
        _vxi_server->get_visa_resource(),
        _scpi->get_visa_resource(),
        0 // Guard against wrong parameters, such as ${9999}
    };
    TinyTemplateEngineMemoryReader reader(LXI_IDENTIFICATION_TEMPLATE);
    reader.keepLineEnds(true);

    AsyncResponseStream *response = request->beginResponseStream("text/html");
    response->setContentType("text/xml");
    TinyTemplateEngine engine(reader);
    engine.start(values);
    while (const char *line = engine.nextLine()) {
        response->print(line);
    }
    engine.end();
    request->send(response);
}

const char *RidenHttpServer::get_firmware_version()
{
    static char firmware_version_string[10];

    uint16_t firmware_version;
    _modbus->get_firmware_version(firmware_version);
    sprintf(firmware_version_string, "%u.%u", firmware_version / 100u, firmware_version % 100u);
    return firmware_version_string;
}

const char *RidenHttpServer::get_serial_number()
{
    static char serial_number_string[10];
    uint32_t serial_number;
    _modbus->get_serial_number(serial_number);
    sprintf(serial_number_string, "%08u", serial_number);
    return serial_number_string;
}

