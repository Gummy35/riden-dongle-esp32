// SPDX-FileCopyrightText: 2024 Peder Toftegaard Olsen
//
// SPDX-License-Identifier: MIT

#include <WiFi.h>
#include <ElegantOTA.h>
#include <WebSerial.h>
#include <Logger.h>
#define WIFI_CREDENTIALS_FILE "/wifi_credentials.txt"
#define AP_SSID "ESP32_AP"
#define WIFI_TIMEOUT_MS 20000 // 20 second WiFi connection timeout
#define WIFI_RECOVER_TIME_MS 30000 // Wait 30 seconds after a failed connection attempt

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
    if (_readWiFiCredentials(wifi_ssid, wifi_password))
    {
        Serial.println("WiFi credentials read successfully.");
        Serial.printf("SSID: %s, Password: %s\n", wifi_ssid.c_str(), wifi_password.c_str());
        _connectToWiFi(wifi_ssid, wifi_password);
    }
    else
    {
        Serial.println("WiFi credentials not found or invalid. Starting AP mode.");
        _startAPMode();
    }
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
    ElegantOTA.begin(_server); // Start ElegantOTA

    _server->on("/status", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->handle_root_get(request); });
    _server->on("/psu/", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->handle_psu_get(request); });
    _server->on("/config/", HTTP_GET, [this](AsyncWebServerRequest *request)
                { this->handle_config_get(request); });
    _server->on("/config/", HTTP_POST, [this](AsyncWebServerRequest *request)
                { this->handle_config_post(request); });
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

bool RidenHttpServer::_readWiFiCredentials(String &ssid, String &password)
{
  File file = LittleFS.open(WIFI_CREDENTIALS_FILE, FILE_READ);
  if (!file)
  {
    Serial.println("Failed to open wifi credentials file");
    return false;
  }

  ssid = file.readStringUntil('\n');
  password = file.readStringUntil('\n');

  // Remove newline characters
  ssid.trim();
  password.trim();

  file.close();

  return !ssid.isEmpty() && !password.isEmpty();
}

void RidenHttpServer::_startAPMode()
{
  WiFi.softAP(AP_SSID);

  Serial.println("Access Point started:");
  Serial.print("SSID: ");
  Serial.println(AP_SSID);
  Serial.print("IP Address: ");
  Serial.println(WiFi.softAPIP());
}

void keepWiFiAlive(void * parameter){
    for(;;){
        if(WiFi.status() == WL_CONNECTED){
            vTaskDelay(10000 / portTICK_PERIOD_MS);
            continue;
        }

        LOG_LN("[WIFI] Connecting");
        WiFi.mode(WIFI_STA);
        WiFi.begin();

        unsigned long startAttemptTime = millis();

        // Keep looping while we're not connected and haven't reached the timeout
        while (WiFi.status() != WL_CONNECTED && 
                millis() - startAttemptTime < WIFI_TIMEOUT_MS){}

        // When we couldn't make a WiFi connection (or the timeout expired)
		  // sleep for a while and then retry.
        if(WiFi.status() != WL_CONNECTED){
            LOG_LN("[WIFI] FAILED");
            vTaskDelay(WIFI_RECOVER_TIME_MS / portTICK_PERIOD_MS);
			  continue;
        }

        LOG_LN("[WIFI] Connected: " + WiFi.localIP().toString());
    }
}

void RidenHttpServer::_connectToWiFi(const String &ssid, const String &password)
{
  WiFi.begin(ssid.c_str(), password.c_str());

  Serial.print("Connecting to WiFi");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 10)
  {
    delay(1000);
    Serial.print(".");
    attempts++;
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED)
  {
    Serial.println("Connected to WiFi successfully.");
    Serial.print("IP Address: ");
    Serial.println(WiFi.localIP());

    xTaskCreatePinnedToCore(
	    keepWiFiAlive,
	    "keepWiFiAlive",  // Task name
	    5000,             // Stack size (bytes)
	    NULL,             // Parameter
	    1,                // Task priority
	    NULL,             // Task handle
	    ARDUINO_RUNNING_CORE
    );     
  }
  else
  {
    Serial.println("Failed to connect to WiFi. Starting AP mode.");
    _startAPMode();
  }
}

bool RidenHttpServer::_writeWiFiCredentials(const String &ssid, const String &password)
{
  File file = LittleFS.open(WIFI_CREDENTIALS_FILE, FILE_WRITE);
  if (!file)
  {
    Serial.println("Failed to open wifi credentials file for writing");
    return false;
  }

  file.println(ssid);
  file.println(password);

  file.close();
  return true;
}

void RidenHttpServer::_onOTAEnd(bool success)
{
  // Log when OTA has finished
  if (success)
  {
    Logger.Log("OTA update complete");
    _writeWiFiCredentials(wifi_ssid, wifi_password);
    LittleFS.end();
  }
  else
  {
    Serial.println("There was an error during OTA update!");
  }
  // <Add your own code here>
}

void RidenHttpServer::_handleGetJsonFile(AsyncWebServerRequest *request, const char *filename)
{
  if (LittleFS.exists(filename))
    request->send(LittleFS, filename, "application/json");
  else
    request->send(200, "application/json", "{}");
}

void RidenHttpServer::_saveJsonToFile(AsyncWebServerRequest *request, JsonVariant &json, const char *filename)
{
  JsonDocument data;
  if (json.is<JsonArray>())
  {
    data = json.as<JsonArray>();
  }
  else if (json.is<JsonObject>())
  {
    data = json.as<JsonObject>();
  }

  File file = LittleFS.open(filename, "w");
  if (!file)
  {
    Logger.Log("Failed to open file for writing");
    request->send(500, "application/json", "{error:\"Failed to open file for writing\"}");
    return;
  }

  if (serializeJson(data, file) == 0)
  {
    Logger.Log("Failed to write to file");
    request->send(500, "application/json", "{error:\"Failed to write to file\"}");
    file.close();
    return;
  }

  file.close();
  // String response;
  // serializeJson(data, response);
  // request->send(200, "application/json", response);
  // Serial.println(response);
  Logger.Log("Config saved. Restarting...");
  request->send(200, "text/plain", "file saved");
  LittleFS.end();
  ESP.restart();
}

void RidenHttpServer::_handleSaveWiFi(AsyncWebServerRequest *request)
{
  if (request->hasArg("ssid") && request->hasArg("password"))
  {
    String ssid = request->arg("ssid");
    String password = request->arg("password");

    if (_writeWiFiCredentials(ssid, password))
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
  if (LittleFS.remove(WIFI_CREDENTIALS_FILE))
  {
    WiFi.eraseAP();
    LittleFS.end();
    ESP.restart();
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
    return wifi_ssid;

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

// void RidenHttpServer::handle_config_get(AsyncWebServerRequest *request)
// {
//     AsyncResponseStream *response = request->beginResponseStream("text/html");
//     response->print(HTML_HEADER);
//     response->print(HTML_CONFIG_BODY_1);
//     //send_as_chunks(HTML_CONFIG_BODY_1);
//     String configured_tz = riden_config.get_timezone_name();
//     LOG_LN(configured_tz);
//     for (int i = 0; i < /*riden_config.get_number_of_timezones()*/50; i++) {
//         const Timezone &timezone = riden_config.get_timezone(i);
//         String name = timezone.name;
//         if (name == configured_tz) {
//             response->print("<option value='" + name + "' selected>" + name + "</option>");
//         } else {
//             response->print("<option value='" + name + "'>" + name + "</option>");
//         }
//     }
//     // send_as_chunks(HTML_CONFIG_BODY_2);
//     response->print(HTML_CONFIG_BODY_2);
//     uint32_t uart_baudrate = riden_config.get_uart_baudrate();
//     for (uint32_t option : uart_baudrates) {
//         String option_string(option, 10);
//         if (option == uart_baudrate) {
//             response->print("<option value='" + option_string + "' selected>" + option_string + "</option>");
//         } else {
//             response->print("<option value='" + option_string + "'>" + option_string + "</option>");
//         }
//     }
//     //send_as_chunks(HTML_CONFIG_BODY_3);
//     response->print(HTML_CONFIG_BODY_3);
//     response->print(HTML_FOOTER);
//     response->print("");
//     request->send(response);
// }

void RidenHttpServer::handle_config_get(AsyncWebServerRequest *request)
{
    AsyncChunkedResponse *response = new AsyncChunkedResponse("text/html", [](uint8_t *buffer, size_t maxLen, size_t index) -> size_t {
        static int phase = 0;
        static size_t tz_index = 0;
        static std::list<uint32_t>::const_iterator baud_it;
        static String configured_tz;
        static bool initialized = false;

        if (!initialized) {
            configured_tz = riden_config.get_timezone_name();
            baud_it = uart_baudrates.begin(); // init de l'itérateur
            tz_index = 0;
            phase = 0;
            initialized = true;
        }

        String chunk;

        switch (phase) {
            case 0: // Header + BODY_1
                chunk += HTML_HEADER;
                chunk += HTML_CONFIG_BODY_1;
                phase++;
                break;

            case 1: // Timezones
                while (tz_index < riden_config.get_number_of_timezones()) {
                    const Timezone &tz = riden_config.get_timezone(tz_index);
                    String name = tz.name;
                    chunk += "<option value='" + name + "'";
                    if (name == configured_tz) chunk += " selected";
                    chunk += ">" + name + "</option>\n";
                    tz_index++;

                    if (chunk.length() > maxLen - 100) break;
                }

                if (tz_index >= riden_config.get_number_of_timezones()) {
                    phase++;
                }
                break;

            case 2: // BODY_2
                chunk += HTML_CONFIG_BODY_2;
                phase++;
                break;

            case 3: // Baudrates
                while (baud_it != uart_baudrates.end()) {
                    String rate_str = String(*baud_it, 10);
                    chunk += "<option value='" + rate_str + "'";
                    if (*baud_it == riden_config.get_uart_baudrate()) chunk += " selected";
                    chunk += ">" + rate_str + "</option>\n";
                    ++baud_it;

                    if (chunk.length() > maxLen - 100) break;
                }

                if (baud_it == uart_baudrates.end()) {
                    phase++;
                }
                break;

            case 4: // BODY_3 + FOOTER
                chunk += HTML_CONFIG_BODY_3;
                chunk += HTML_FOOTER;
                phase++;
                break;

            case 5: // Fin de transmission
                initialized = false; // reset pour prochain appel
                return 0;
        }

        // Copie le chunk dans le buffer
        size_t len = chunk.length();
        len = len > maxLen ? maxLen : len;
        memcpy(buffer, chunk.c_str(), len);
        return len;
    });

    request->send(response);
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

// void RidenHttpServer::send_as_chunks(AsyncResponseStream* response, const char *str)
// {
//     const size_t chunk_length = 1000;
//     size_t length = strlen(str);
//     for (size_t start_pos = 0; start_pos < length; start_pos += chunk_length) {
//         size_t end_pos = min(start_pos + chunk_length, length);
//         response->print(&(str[start_pos]), end_pos - start_pos);
//         yield();
//     }
// }

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

