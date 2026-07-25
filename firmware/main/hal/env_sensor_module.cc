/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "env_sensor_module.h"
#include "drivers/BME688/BME688.h"
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <fmt/format.h>

static const std::string_view _tag = "EnvSensorModule";

struct EnvSensorModule::Impl {
    bme688_handle_t bme = nullptr;
    bool ok = false;
    std::mutex mutex;
    Reading last_reading;
};

static void _update_task(void* param)
{
    auto* impl = static_cast<EnvSensorModule::Impl*>(param);
    mclog::tagInfo(_tag, "start update task");
    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        bme688_reading_t r;
        if (bme688_read(impl->bme, &r) == ESP_OK && r.ok) {
            EnvSensorModule::Reading reading;
            reading.ok = true;
            reading.temperatureC = r.temperature_c;
            reading.humidityPct = r.humidity_pct;
            reading.pressureHpa = r.pressure_hpa;
            reading.gasResistanceOhm = r.gas_valid ? r.gas_resistance_ohm : 0;

            std::lock_guard<std::mutex> lock(impl->mutex);
            impl->last_reading = reading;
        } else {
            mclog::tagWarn(_tag, "read failed");
        }
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

EnvSensorModule::EnvSensorModule(i2c_master_bus_handle_t i2c_bus)
{
    _impl = new Impl();

    bme688_config_t cfg = {
        .i2c_bus = i2c_bus,
        .dev_addr = BME688_I2C_ADDR_HIGH,
    };

    esp_err_t err = bme688_init(&cfg, &_impl->bme);
    if (err != ESP_OK) {
        mclog::tagWarn(_tag, "BME688 not detected; environment sensing disabled");
        return;
    }

    _impl->ok = true;
    xTaskCreatePinnedToCoreWithCaps(_update_task, "envsensor", 1024 * 6, _impl, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}

EnvSensorModule::~EnvSensorModule()
{
    if (_impl->bme) {
        bme688_delete(_impl->bme);
    }
    delete _impl;
}

EnvSensorModule::Reading EnvSensorModule::getLastReading()
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->last_reading;
}

void EnvSensorModule::registerMcpTools(McpServer& mcp_server)
{
    if (!_impl->ok) {
        return;
    }

    mclog::tagInfo(_tag, "add sensors.get_environment tool");
    mcp_server.AddTool("self.sensors.get_environment",
                       "Returns current temperature (Celsius), humidity (%), pressure (hPa), and raw gas "
                       "resistance (Ohms, uncalibrated) from the onboard environmental sensor.",
                       std::vector<Property>{}, [this](const PropertyList&) -> ReturnValue {
                           auto reading = getLastReading();
                           if (!reading.ok) {
                               return std::string(R"({"available": false})");
                           }
                           return fmt::format(
                               R"({{"available": true, "temperature_c": {:.1f}, "humidity_pct": {:.1f}, )"
                               R"("pressure_hpa": {:.1f}, "gas_resistance_ohm": {}}})",
                               reading.temperatureC, reading.humidityPct, reading.pressureHpa,
                               reading.gasResistanceOhm);
                       });
}