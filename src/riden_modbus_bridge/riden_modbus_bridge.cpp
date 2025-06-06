// SPDX-FileCopyrightText: 2024 Peder Toftegaard Olsen
//
// SPDX-License-Identifier: MIT

#include <riden_logging/riden_logging.h>
#include <riden_modbus_bridge/riden_modbus_bridge.h>

#include <ESPmDNS.h>

using namespace RidenDongle;

// Callbacks within the esp8266-modbus library do
// not allow for instance methods to be used, so for
// the time being we stick to only allowing a single
// instance of RidenModbusBridge.
static RidenModbusBridge *one_and_only = nullptr;
static Modbus::ResultCode modbus_tcp_raw_callback(uint8_t *data, uint8_t len, void *custom_data);
static Modbus::ResultCode modbus_rtu_raw_callback(uint8_t *data, uint8_t len, void *custom);

bool RidenModbusBridge::begin()
{
    if (initialized) {
        return true;
    }
    if (one_and_only != nullptr) {
        return false;
    }

    LOG_LN("RidenModbusBridge initializing");

    _modbus_tcp = new RidenModbusTCP();
    _modbus_tcp->onRaw(::modbus_tcp_raw_callback);
    _modbus_tcp->server();

    // See https://github.com/espressif/esp-idf/blob/master/examples/protocols/modbus/tcp/mb_tcp_master/main/tcp_master.c#L266

    LOG_LN("RidenModbusBridge initialized");

    one_and_only = this;
    initialized = true;
    return true;
}

void RidenModbusBridge::advertiseMDNS()
{
    auto modbus_service = MDNS.addService("modbus", "tcp", MODBUSTCP_PORT);
    MDNS.addServiceTxt("modbus", "tcp", "unitid", String(MODBUS_ADDRESS));
}

bool RidenModbusBridge::loop()
{
    _modbus_tcp->task();
    return true;
}

uint16_t RidenModbusBridge::port()
{
    return MODBUSTCP_PORT;
}

std::list<IPAddress> RidenModbusBridge::get_connected_clients()
{
    return _modbus_tcp->get_connected_clients();
}

void RidenModbusBridge::disconnect_client(const IPAddress &ip)
{
    LOG_LN("RidenModbusBridge::disconnect_client");
    _modbus_tcp->disconnect_client(ip);
}

/**
 * Data received from the TCP-end is forwarded to ModbusRTU,
 * which in turn forwards it to the power supply.
 */
Modbus::ResultCode RidenModbusBridge::modbus_tcp_raw_callback(uint8_t *data, uint8_t len, void *custom_data)
{
    if (!initialized) {
        return Modbus::EX_GENERAL_FAILURE;
    }
    // Wait until no transaction is active
#ifdef MOCK_RIDEN
    return Modbus::EX_SUCCESS;
#else
    while (_riden_modbus->modbus.server()) {
        delay(1);
        _riden_modbus->modbus.task();
    }

    Modbus::frame_arg_t *source = (Modbus::frame_arg_t *)custom_data;
    if (!_riden_modbus->modbus.rawRequest(source->slaveId, data, len)) {
        // Inform TCP-end that processing failed
        _modbus_tcp->errorResponce(source->slaveId, (Modbus::FunctionCode)data[0], Modbus::EX_DEVICE_FAILED_TO_RESPOND);
        return Modbus::EX_DEVICE_FAILED_TO_RESPOND; // Stop ModbusTCP from processing the data
    }

    // Set up ourself for forwarding the response to our ModbusTCP instance.
    transaction_id = source->transactionId;
    slave_id = source->slaveId;
    ip = source->ipaddr;
    _riden_modbus->modbus.onRaw(::modbus_rtu_raw_callback);
    return Modbus::EX_SUCCESS; // Stops ModbusTCP from processing the data
#endif
}

/**
 * Data received from the RTU-end must be forwarded to the TCP-end. Anything
 * else is passed through unaltered to ModbusRTU.
 */
Modbus::ResultCode RidenModbusBridge::modbus_rtu_raw_callback(uint8_t *data, uint8_t len, void *custom)
{
    if (!initialized) {
        return Modbus::EX_GENERAL_FAILURE;
    }
#ifdef MOCK_RIDEN
    return Modbus::EX_SUCCESS;
#else

    // Stop intercepting raw data
    _riden_modbus->modbus.onRaw(nullptr);

    const Modbus::frame_arg_t *source = static_cast<Modbus::frame_arg_t *>(custom);
    if (!source->to_server) {
        _modbus_tcp->setTransactionId(transaction_id);
        _modbus_tcp->rawResponce(ip, data, len, slave_id);
    } else {
        return Modbus::EX_PASSTHROUGH;
    }

    // Clear state
    transaction_id = 0;
    slave_id = 0;
    ip = 0;
    return Modbus::EX_SUCCESS; // Stops ModbusRTU from processing the data
#endif
}

Modbus::ResultCode modbus_tcp_raw_callback(uint8_t *data, uint8_t len, void *custom_data)
{
    return one_and_only->modbus_tcp_raw_callback(data, len, custom_data);
}

Modbus::ResultCode modbus_rtu_raw_callback(uint8_t *data, uint8_t len, void *custom)
{
    return one_and_only->modbus_rtu_raw_callback(data, len, custom);
}

std::list<IPAddress> RidenModbusTCP::get_connected_clients()
{
    std::list<IPAddress> connected_clients;
    for (int i = 0; i < MODBUSIP_MAX_CLIENTS; i++) {
        if (tcpclient[i] != nullptr && tcpclient[i]->connected()) {
            connected_clients.push_back(tcpclient[i]->remoteIP());
        }
    }
    return connected_clients;
}

void RidenModbusTCP::disconnect_client(const IPAddress &ip)
{
    int8_t n = getMaster(ip);
    if (n != -1) {
        tcpclient[n]->flush();
        delete tcpclient[n];
        tcpclient[n] = nullptr;
    }
}
