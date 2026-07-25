/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once
#include <driver/i2c_master.h>
#include <mutex>
#include <mcp_server.h>

class EnvSensorModule {
public:
    struct Reading {
        bool ok = false;
        float temperatureC = 0.0f;
        float humidityPct = 0.0f;
        float pressureHpa = 0.0f;
        uint32_t gasResistanceOhm = 0;
    };

    struct Impl;

    explicit EnvSensorModule(i2c_master_bus_handle_t i2c_bus);
    ~EnvSensorModule();

    Reading getLastReading();
    void registerMcpTools(McpServer& mcp_server);

private:
    Impl* _impl;  // pimpl: keeps bme68x/BME688 types entirely out of this header
};