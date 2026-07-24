/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "hal.h"
#include "drivers/BME688/BME688.h"
#include "board/hal_bridge.h"
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <mutex>

static const std::string_view _tag = "HAL-EnvSensor";

static std::mutex _env_mutex;
static Hal::EnvReading_t _last_reading;

static void _env_sensor_update_task(void* param)
{
    mclog::tagInfo(_tag, "start update task");

    bme688_handle_t bme = (bme688_handle_t)param;

    vTaskDelay(pdMS_TO_TICKS(500));

    while (1) {
        bme688_reading_t r;
        esp_err_t err = bme688_read(bme, &r);

        if (err == ESP_OK && r.ok) {
            Hal::EnvReading_t reading;
            reading.ok = true;
            reading.temperatureC = r.temperature_c;
            reading.humidityPct = r.humidity_pct;
            reading.pressureHpa = r.pressure_hpa;
            reading.gasResistanceOhm = r.gas_valid ? r.gas_resistance_ohm : 0;

            {
                std::lock_guard<std::mutex> lock(_env_mutex);
                _last_reading = reading;
            }

            GetHAL().onEnvReading.emit(reading);
        } else {
            mclog::tagWarn(_tag, "read failed");
        }

        // BME688 gas heater cycling means this shouldn't be polled too fast
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

void Hal::env_sensor_init()
{
    mclog::tagInfo(_tag, "init");

    auto i2c_bus = hal_bridge::board_get_i2c_bus();

    bme688_config_t cfg = {
        .i2c_bus = i2c_bus,
        .dev_addr = BME688_I2C_ADDR_HIGH,
    };

    static bme688_handle_t bme = nullptr;
    esp_err_t err = bme688_init(&cfg, &bme);
    if (err != ESP_OK) {
        mclog::tagWarn(_tag, "BME688 not detected; environment sensing disabled");
        return;
    }

    xTaskCreatePinnedToCoreWithCaps(_env_sensor_update_task, "envsensor", 1024 * 6, bme, 2, NULL, 1,
                                    MALLOC_CAP_SPIRAM);
}

Hal::EnvReading_t Hal::getLastEnvReading()
{
    std::lock_guard<std::mutex> lock(_env_mutex);
    return _last_reading;
}