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
        float iaq = 0.0f;
        uint8_t iaqAccuracy = 0;       // 0=stabilizing, 1=low, 2=medium, 3=high
        float co2EquivalentPpm = 0.0f;
    };

    struct Impl;

    explicit EnvSensorModule(i2c_master_bus_handle_t i2c_bus);
    ~EnvSensorModule();

    Reading getLastReading();
    void registerMcpTools(McpServer& mcp_server);

private:
    Impl* _impl;
};