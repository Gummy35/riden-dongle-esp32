// SPDX-FileCopyrightText: 2024 Peder Toftegaard Olsen
//
// SPDX-License-Identifier: MIT

#include <Arduino.h>
#include <EEPROM.h>
#include <ESPmDNS.h>
#include <LittleFS.h>
#include <Logger.h>
#include <RidenStatus.h>
#include <Ticker.h>
#include <WebSerial.h>
#include <WiFi.h>
#include <esp_sntp.h>
#include <riden_config/riden_config.h>
#include <riden_http_server/riden_http_server.h>
#include <riden_logging/riden_logging.h>
#include <riden_modbus/riden_modbus.h>
#include <riden_modbus_bridge/riden_modbus_bridge.h>
#include <riden_scpi/riden_scpi.h>
#include <scpi_bridge/scpi_bridge.h>
#include <time.h>
#include <vxi11_server/rpc_bind_server.h>
#include <vxi11_server/vxi_server.h>

#include "soc/rtc_cntl_reg.h"
#include "soc/soc.h"

#define NTP_SERVER "pool.ntp.org"
// #define LED_BUILTIN 2

#define FAILSAFE_FILE "/failsafe.tmp"

#ifdef MOCK_RIDEN
#define MODBUS_USE_SOFWARE_SERIAL
#endif

using namespace RidenDongle;

static Ticker led_ticker;
static char hostname[100];

static bool has_time = false;
static bool did_update_time = false;

static bool connected = false;
volatile bool isSafemode = false;
static bool useEnPin = false;
static int enPin = 5;
volatile byte enPinState = LOW;
volatile bool enPinStateChanged = false;

RidenModbus *riden_modbus = new RidenModbus();                                                           ///< The modbus server
RidenScpi *riden_scpi = new RidenScpi(riden_modbus);                                                     ///< The raw socket server + the SCPI command handler
RidenModbusBridge *modbus_bridge = new RidenModbusBridge(riden_modbus);                                  ///< The modbus TCP server
SCPI_handler *scpi_handler = new SCPI_handler(riden_scpi);                                               ///< The bridge from the vxi server to the SCPI command handler
VXI_Server *vxi_server = new VXI_Server(scpi_handler);                                                   ///< The vxi server
RPC_Bind_Server *rpc_bind_server = new RPC_Bind_Server(vxi_server);                                      ///< The RPC_Bind_Server for the vxi server
RidenHttpServer *http_server = new RidenHttpServer(riden_modbus, riden_scpi, modbus_bridge, vxi_server); ///< The web server

/**
 * Invoked by led_ticker to flash the LED.
 */
static void tick();

/**
 * Invoked when time has been received from an NTP server.
 */
static void on_time_received(struct timeval *tv);

#define DEBUG

#ifdef DEBUG
#define debug(MyCode) MyCode
#else
#endif

static bool SetupWifi(const char *hostname)
{
    LOG_LN("set hostname");
    // Serial.println(hostname);
    WiFi.setHostname(hostname);
    bool wifi_connected = (WiFi.status() == WL_CONNECTED);

    if (wifi_connected) {
        LOG_LN("Wifi connected");

        LOG_F("WiFi SSID: %s\r\n", WiFi.SSID().c_str());
        LOG_F("IP: %s\r\n", WiFi.localIP().toString().c_str());

        // experimental::ESP8266WiFiGratuitous::stationKeepAliveSetIntervalMs();
        if (hostname != nullptr) {
            LOG_LN("starting MDNS");
            if (!MDNS.begin(hostname)) {
                LOG_LN("MDNS failed");
                while (true) {
                    delay(100);
                }
            }
            String tz = riden_config.get_timezone_spec();
            if (tz.length() > 0) {
                // Get time via NTP
                // settimeofday_cb(on_time_received);
                // configTime(tz.c_str(), NTP_SERVER);
                configTzTime(tz.c_str(), NTP_SERVER);
                sntp_set_time_sync_notification_cb(on_time_received);
            }
        }

        LOG_LN("WiFi initialized");
    } else {
        LOG_LN("WiFi failed to initialize");
    }

    return wifi_connected;
}

bool StartServices()
{
    // Wait for power supply firmware to boot
    unsigned long boot_delay_start = millis();
    while (!riden_modbus->begin()) {
        if (millis() - boot_delay_start >= 5000L)
            break;
        delay(100);
    }

    // We need modbus initialised to read type and serial number
    if (riden_modbus->is_connected()) {
        uint32_t serial_number;
        riden_modbus->get_serial_number(serial_number);
        sprintf(hostname, "%s-%08u", riden_modbus->get_type().c_str(), serial_number);
        LOG("Hostname = ");
        LOG_LN(hostname);

        LOG("Setup Wifi...");
        bool res = SetupWifi(hostname);
        LOG_LN("Start SCPI");
        riden_scpi->begin();
        LOG_LN("Start Modbus bridge");
        modbus_bridge->begin();
        LOG_LN("VXI server");
        vxi_server->begin();
        LOG_LN("RPC Bind server");
        rpc_bind_server->begin();
        LOG_LN("Status update worker");
        RidenStatus.start();
        LOG_LN("Service initialization complete");
        // turn off led
        led_ticker.detach();
        digitalWrite(LED_BUILTIN, HIGH);

        LOG_LN("MDNS: Add services");
        auto arduino_service = MDNS.addService("arduino", "tcp", 80);
        MDNS.addServiceTxt("arduino", "tcp", "app_version", RidenDongle::version_string);
        if (RidenDongle::build_time != nullptr) {
            MDNS.addServiceTxt("arduino", "tcp", "build_date", RidenDongle::build_time);
        }
        MDNS.addServiceTxt("arduino", "tcp", "mac", WiFi.macAddress());

        http_server->advertiseMDNS();
        modbus_bridge->advertiseMDNS();
        riden_scpi->advertiseMDNS();
        vxi_server->advertiseMDNS();

        connected = true;
    } else {
        bool res = SetupWifi(nullptr);

        led_ticker.attach(0.1, tick);
        connected = false;
    }
    return connected;
}

/// @brief Init
bool InitServices()
{
    byte devId = 0;

    // set default logger callback
    Logger.SetLogger([](const char *logString) {
        WebSerial.println(logString);
    });

    // Wait for serial
    // Serial.begin(115200);
    Serial.begin(9600);
    while (!Serial)
        delay(10);

    LOG_LN(ESP.getSdkVersion());

    riden_config.begin();
    return StartServices();
}

void PrintFreeRam()
{
    multi_heap_info_t info;
    heap_caps_get_info(&info, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT); // internal RAM, memory capable to store data or to create new task
    WebSerial.printf("Total free : %d, minimum free : %d, largest block : %d\n",
                     info.total_free_bytes,    // total currently free in all non-continues blocks
                     info.minimum_free_bytes,  // minimum free ever
                     info.largest_free_block); // largest continues block to allocate big array
}

void DisplayServicesStatus()
{
    String status = "modbus : ";
    if (riden_modbus == nullptr) {
        status.concat("stopped");
    } else {
        status.concat("started, ");
        if (!riden_modbus->is_connected())
            status.concat("device unreachable");
        else
            status.concat("device connected");
    }

    status.concat("\nSCPI : ");
    if (riden_scpi == nullptr)
        status.concat("stopped");
    else
        status.concat("started");

    status.concat("\nVXI : ");
    if (vxi_server == nullptr) {
        status.concat("stopped");
    } else {
        status.concat("started, ");
        if (vxi_server->available())
            status.concat("available");
        else
            status.concat("busy (client connected)");
    }

    status.concat("\nRPC bind server : ");
    if (rpc_bind_server == nullptr)
        status.concat("stopped");
    else
        status.concat("started");

    status.concat("\nHttp Server : ");
    if (http_server == nullptr)
        status.concat("stopped");
    else
        status.concat("started");
    WebSerial.println(status);
}

/// @brief setup Web serial commands handling
void SetupWebSerialCommands()
{
    WebSerial.onMessage([&](uint8_t *data, size_t len) {
        unsigned long ts = millis();
        debug(Serial.printf("Received %u bytes from WebSerial: ", len));
        debug(Serial.write(data, len));
        // debug(Serial.println());
        String d(data, len);
        WebSerial.println(d);
        if (d.equals("help")) {
            WebSerial.println("freeram : display free ram");
            if (isSafemode)
                WebSerial.println("boot : exit safemode and continue boot");
            else {
                WebSerial.println("safemode : enter safemode at next boot");
                WebSerial.println("scpi : scpi commands");
                WebSerial.println("svc : manage services");
            }
            WebSerial.println("reboot : reboot dongle");
        } else if (d.equals("freeram")) {
            PrintFreeRam();
        } else if (d.equals("reboot")) {
            LittleFS.end();
            ESP.restart();
        } else if (!isSafemode) {
            if (d.equals("safemode")) {
                File failsafeFile = LittleFS.open(FAILSAFE_FILE, "w");
                if (failsafeFile) {
                    failsafeFile.close();
                    WebSerial.println("Safemode flag set, reboot to enter safemode");
                } else {
                    WebSerial.println("Could not create flag file. Consider reflashing littlefs partition");
                }
            } else if (d.startsWith("scpi")) {
                String subcommand = d.substring(4);
                subcommand.trim();
                if ((subcommand.equals("")) || subcommand.equals("help")) {
                    WebSerial.println("scpi help : display this help");
                    WebSerial.println("scpi list : list all available commands");
                    WebSerial.println("scpi [command] : execute command (see scpi list for available commands)");
                    WebSerial.println("** Note : Using scpi [command] will force close external connections **");
                } else if (subcommand.equals("list")) {
                    File file = LittleFS.open("/SCPI_COMMANDS.md", FILE_READ);
                    if (!file) {
                        WebSerial.println("Failed to scpi commands");
                    } else {
                        String tmp;
                        String command = "";
                        int i = 0;
                        while (file.available()) {
                            tmp = file.readStringUntil('\n');
                            tmp.trim();
                            if (tmp.startsWith("##")) {
                                if (!command.equals("")) {
                                    command.trim();
                                    WebSerial.println(command);
                                    i++;
                                    delay(1);
                                }
                                command = tmp + " :";
                            } else if (!tmp.equals("")) {
                                if (!command.equals("")) {
                                    command.concat(" ");
                                    command.concat(tmp);
                                    command.concat("\n");
                                }
                            }
                            yield();
                        }
                        command.trim();
                        if (!command.equals(""))
                            WebSerial.println(command);
                        file.close();
                    }
                } else {
                    if (scpi_handler->claim_control()) {
                        scpi_handler->write(subcommand.c_str(), subcommand.length() - 1);
                        char outbuffer[256];
                        size_t len = 0;
                        scpi_result_t rv = scpi_handler->read(outbuffer, &len, sizeof(outbuffer));
                        if (rv == SCPI_RES_OK) {
                            WebSerial.println(outbuffer);
                        } else {
                            WebSerial.println("SCPI : Error while processing command");
                        }
                        scpi_handler->release_control();
                    } else {
                        WebSerial.println("SCPI : could not process command");
                    }
                }
            } else if (d.startsWith("svc")) {
                String subcommand = d.substring(3);
                subcommand.trim();
                if ((subcommand.equals("")) || subcommand.equals("help")) {
                    WebSerial.println("svc help : display this help");
                    WebSerial.println("svc status : display services status");
                    WebSerial.println("svc start : start services");
                    WebSerial.println("svc stop : stop services");
                } else if (subcommand.equals("status")) {
                    DisplayServicesStatus();
                } else if (subcommand.equals("stop")) {
//                    free(http_server);
                //     free(rpc_bind_server);
                //     free(scpi_handler);
                //     free(modbus_bridge);
                //     free(riden_scpi);                    
                //     free(riden_modbus);
                } else if (subcommand.equals("start"))
                {
                    bool started = StartServices();
                    DisplayServicesStatus();
                }

                // RidenScpi *riden_scpi = new RidenScpi(riden_modbus);                                                     ///< The raw socket server + the SCPI command handler
                // RidenModbusBridge *modbus_bridge = new RidenModbusBridge(riden_modbus);                                  ///< The modbus TCP server
                // SCPI_handler *scpi_handler = new SCPI_handler(riden_scpi);                                               ///< The bridge from the vxi server to the SCPI command handler
                // VXI_Server *vxi_server = new VXI_Server(scpi_handler);                                                   ///< The vxi server
                // RPC_Bind_Server *rpc_bind_server = new RPC_Bind_Server(vxi_server);                                      ///< The RPC_Bind_Server for the vxi server
                // RidenHttpServer *http_server = new RidenHttpServer(riden_modbus, riden_scpi, modbus_bridge, vxi_server); ///< The web server
            }
        } else if (isSafemode && d.equals("boot")) {
        isSafemode = false;
        }

        debug(WebSerial.printf("%d ms\n", millis() - ts));
});
}

void failsafeMode()
{
    log_printf("Oooops, something went wrong. Entering safemode\n");
    isSafemode = true;
    while (isSafemode) {
        http_server->loop();
        delay(5);
    }
}

void enPinStateChangeIntrHandler()
{
    enPinState = digitalRead(enPin);
    enPinStateChanged = true;
}

void setup()
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // disable brownout
    pinMode(LED_BUILTIN, OUTPUT);
    led_ticker.attach(0.6, tick);

#ifdef MODBUS_USE_SOFWARE_SERIAL
    Serial.begin(74880);
    delay(1000);
#endif

    // start filesystem
    bool littleFSstatus = LittleFS.begin();
    // web server (ota + serial)
    http_server->begin();
    delay(500);
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 1); // reenable brownout

    // configure web serial commands
    SetupWebSerialCommands();

    // Set failsafe flag
    if (!littleFSstatus)
        log_printf("LittleFS NOT initialized\n");
    if (littleFSstatus) {
        log_printf("Checking for safemode flag file\n");
        // check failsafe status file
        if (LittleFS.exists(FAILSAFE_FILE)) {
            // enter failsafe mode
            log_printf("Safemode flag file found, entering safemode\n");
            failsafeMode();
        }

        File failsafeFile = LittleFS.open(FAILSAFE_FILE, "w");
        if (failsafeFile) {
            log_printf("Creating safemode flag file\n");
            failsafeFile.close();
        } else {
            // can't create failsafe flag => enter failsafe mode
            log_printf("Can't create file, entering safemode\n");
            failsafeMode();
        }
    }

#ifdef ENABLE_RD_EN_DETECT
    useEnPin = true;
#endif

    if (useEnPin) {
#ifdef RD_EN_PIN
        enPin = RD_EN_PIN;
#endif

        attachInterrupt(digitalPinToInterrupt(enPin), enPinStateChangeIntrHandler, CHANGE);
    }
    // init devices
    bool initRes = InitServices();
    // Logger.Log("v1.4");

    // create FreeRTOS tasks
    // xTaskCreatePinnedToCore(TaskKeypadCode, "TaskKeypad", 10000, NULL, 1, &TaskKeypad, 0);
    // xTaskCreatePinnedToCore(TaskDisplayCode, "TaskDisplay", 10000, NULL, 1, &TaskDisplayController, 0);
    // xTaskCreatePinnedToCore(TaskCommsCode, "TaskComms", 10000, NULL, 1, &TaskComms, 1);
    // xTaskCreatePinnedToCore(TaskUpdateLedsCode, "TaskLedcontroller", 10000, NULL, 1, &TaskLedController, 1);
    // wait for everyone to be ready

    delay(1000);
    // initialization is ok, enter normal mode
    if (LittleFS.exists(FAILSAFE_FILE)) {
        log_printf("Init complete, Removing safemode flag\n");
        LittleFS.remove(FAILSAFE_FILE);
        isSafemode = false;
    }
}

void loop()
{
    if (connected) {
        if (has_time && !did_update_time) {
            LOG_LN("Setting PSU clock");
            // Read time and convert to local timezone
            time_t now;
            tm tm;
            time(&now);
            localtime_r(&now, &tm);

            riden_modbus->set_clock(tm);
            did_update_time = true;
        }

        // //MDNS.update();
        riden_modbus->loop();
        riden_scpi->loop();
        modbus_bridge->loop();
        rpc_bind_server->loop();
        vxi_server->loop();
    }
    http_server->loop();
    delay(5);
    if (enPinStateChanged) {
        delay(100);
        enPinStateChanged = false;
        if (enPinState) {
            WebSerial.println("RD EN Pin activated");
            if (!connected)
                StartServices();
        } else {
            WebSerial.println("RD EN Pin deactivated");
        }
    }
}

void tick()
{
    // Toggle led state
    int state = digitalRead(LED_BUILTIN);
    digitalWrite(LED_BUILTIN, !state);
}

// void wifi_manager_config_mode_callback(WiFiManager *myWiFiManager)
// {
//     // entered config mode, make led toggle faster
//     led_ticker.attach(0.2, tick);
// }

void on_time_received(struct timeval *tv)
{
    LOG_LN("Time has been received");
    has_time = true;
}
