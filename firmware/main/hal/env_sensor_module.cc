#include "env_sensor_module.h"
#include "hal/drivers/BSEC/inc/bsec_interface.h"
#include "hal/drivers/BSEC/inc/bsec_datatypes.h"
#include "hal/drivers/BSEC/config/bsec_iaq.h"
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <fmt/format.h>
#include <esp_timer.h>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "hal/drivers/BME688/bme68x.h"
}

static const std::string_view _tag = "EnvSensorModule";

struct EnvSensorModule::Impl {
    i2c_master_dev_handle_t i2c_dev;
    struct bme68x_dev bme_dev;
    void* bsec_instance = nullptr;
    bool ok = false;
    std::mutex mutex;
    Reading last_reading;
};

static BME68X_INTF_RET_TYPE _i2c_read(uint8_t reg_addr, uint8_t* reg_data, uint32_t len, void* intf_ptr)
{
    auto* impl = static_cast<EnvSensorModule::Impl*>(intf_ptr);
    esp_err_t err = i2c_master_transmit_receive(impl->i2c_dev, &reg_addr, 1, reg_data, len, 100);
    return (err == ESP_OK) ? 0 : -1;
}

static BME68X_INTF_RET_TYPE _i2c_write(uint8_t reg_addr, const uint8_t* reg_data, uint32_t len, void* intf_ptr)
{
    auto* impl = static_cast<EnvSensorModule::Impl*>(intf_ptr);
    uint8_t buf[16];
    if (len + 1 > sizeof(buf)) return -1;
    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, len);
    esp_err_t err = i2c_master_transmit(impl->i2c_dev, buf, len + 1, 100);
    return (err == ESP_OK) ? 0 : -1;
}

static void _delay_us(uint32_t period, void* intf_ptr)
{
    (void)intf_ptr;
    esp_rom_delay_us(period);
}

static int64_t _now_ns()
{
    return (int64_t)esp_timer_get_time() * 1000;
}

static void _apply_bme68x_settings(EnvSensorModule::Impl* impl, const bsec_bme_settings_t& s)
{
    struct bme68x_conf conf;
    bme68x_get_conf(&conf, &impl->bme_dev);
    conf.os_temp = s.temperature_oversampling;
    conf.os_pres = s.pressure_oversampling;
    conf.os_hum = s.humidity_oversampling;
    conf.filter = BME68X_FILTER_OFF;
    conf.odr = BME68X_ODR_NONE;
    bme68x_set_conf(&conf, &impl->bme_dev);

    if (s.run_gas) {
        struct bme68x_heatr_conf heatr_conf;
        heatr_conf.enable = BME68X_ENABLE;
        heatr_conf.heatr_temp = s.heater_temperature;
        heatr_conf.heatr_dur = s.heater_duration;
        bme68x_set_heatr_conf(BME68X_FORCED_MODE, &heatr_conf, &impl->bme_dev);
    }

    bme68x_set_op_mode(BME68X_FORCED_MODE, &impl->bme_dev);
}

static void _update_task(void* param)
{
    auto* impl = static_cast<EnvSensorModule::Impl*>(param);
    mclog::tagInfo(_tag, "start update task");

    while (1) {
        bsec_bme_settings_t settings = {};
        bsec_library_return_t rslt = bsec_sensor_control(impl->bsec_instance, _now_ns(), &settings);
        if (rslt != BSEC_OK) {
            mclog::tagWarn(_tag, "bsec_sensor_control failed: {}", (int)rslt);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        if (settings.trigger_measurement && settings.op_mode == BME68X_FORCED_MODE) {
            _apply_bme68x_settings(impl, settings);

            uint32_t meas_dur_us = bme68x_get_meas_dur(BME68X_FORCED_MODE, nullptr, &impl->bme_dev)
                                  + ((uint32_t)settings.heater_duration * 1000);
            vTaskDelay(pdMS_TO_TICKS(meas_dur_us / 1000 + 10));

            struct bme68x_data data;
            uint8_t n_fields = 0;
            if (bme68x_get_data(BME68X_FORCED_MODE, &data, &n_fields, &impl->bme_dev) == BME68X_OK && n_fields > 0) {
                int64_t ts = _now_ns();
                bsec_input_t inputs[4];
                uint8_t n_inputs = 0;

                if (settings.process_data & BSEC_PROCESS_TEMPERATURE) {
                    inputs[n_inputs] = {ts, data.temperature, 1, BSEC_INPUT_TEMPERATURE};
                    n_inputs++;
                }
                if (settings.process_data & BSEC_PROCESS_HUMIDITY) {
                    inputs[n_inputs] = {ts, data.humidity, 1, BSEC_INPUT_HUMIDITY};
                    n_inputs++;
                }
                if (settings.process_data & BSEC_PROCESS_PRESSURE) {
                    inputs[n_inputs] = {ts, data.pressure, 1, BSEC_INPUT_PRESSURE};
                    n_inputs++;
                }
                if (settings.process_data & BSEC_PROCESS_GAS
                    && (data.status & (BME68X_HEAT_STAB_MSK | BME68X_GASM_VALID_MSK))) {
                    inputs[n_inputs] = {ts, data.gas_resistance, 1, BSEC_INPUT_GASRESISTOR};
                    n_inputs++;
                }

                if (n_inputs > 0) {
                    bsec_output_t outputs[8];
                    uint8_t n_outputs = 8;
                    rslt = bsec_do_steps(impl->bsec_instance, inputs, n_inputs, outputs, &n_outputs);

                    if (rslt == BSEC_OK) {
                        EnvSensorModule::Reading reading = impl->last_reading;
                        reading.ok = true;
                        for (uint8_t i = 0; i < n_outputs; i++) {
                            switch (outputs[i].sensor_id) {
                                case BSEC_OUTPUT_IAQ:
                                    reading.iaq = outputs[i].signal;
                                    reading.iaqAccuracy = outputs[i].accuracy;
                                    break;
                                case BSEC_OUTPUT_CO2_EQUIVALENT:
                                    reading.co2EquivalentPpm = outputs[i].signal;
                                    break;
                                case BSEC_OUTPUT_RAW_TEMPERATURE:
                                    reading.temperatureC = outputs[i].signal;
                                    break;
                                case BSEC_OUTPUT_RAW_HUMIDITY:
                                    reading.humidityPct = outputs[i].signal;
                                    break;
                                case BSEC_OUTPUT_RAW_PRESSURE:
                                    reading.pressureHpa = outputs[i].signal / 100.0f;
                                    break;
                                case BSEC_OUTPUT_RAW_GAS:
                                    reading.gasResistanceOhm = (uint32_t)outputs[i].signal;
                                    break;
                            }
                        }
                        std::lock_guard<std::mutex> lock(impl->mutex);
                        impl->last_reading = reading;
                    } else {
                        mclog::tagWarn(_tag, "bsec_do_steps failed: {}", (int)rslt);
                    }
                }
            }
        }

        int64_t sleep_ns = settings.next_call - _now_ns();
        if (sleep_ns > 0) {
            vTaskDelay(pdMS_TO_TICKS(sleep_ns / 1000000));
        } else {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
}

EnvSensorModule::EnvSensorModule(i2c_master_bus_handle_t i2c_bus)
{
    _impl = new Impl();

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = 0x77,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(i2c_bus, &dev_cfg, &_impl->i2c_dev) != ESP_OK) {
        mclog::tagWarn(_tag, "failed to add I2C device");
        return;
    }

    _impl->bme_dev.intf = BME68X_I2C_INTF;
    _impl->bme_dev.intf_ptr = _impl;
    _impl->bme_dev.read = _i2c_read;
    _impl->bme_dev.write = _i2c_write;
    _impl->bme_dev.delay_us = _delay_us;
    _impl->bme_dev.amb_temp = 25;

    if (bme68x_init(&_impl->bme_dev) != BME68X_OK) {
        mclog::tagWarn(_tag, "bme68x_init failed; environment sensing disabled");
        return;
    }

    size_t inst_size = bsec_get_instance_size();
    _impl->bsec_instance = malloc(inst_size);
    if (!_impl->bsec_instance) {
        mclog::tagError(_tag, "failed to allocate {} bytes for BSEC instance", inst_size);
        return;
    }

    if (bsec_init(_impl->bsec_instance) != BSEC_OK) {
        mclog::tagWarn(_tag, "bsec_init failed");
        return;
    }

    uint8_t work_buffer[BSEC_MAX_PROPERTY_BLOB_SIZE];
    bsec_library_return_t cfg_rslt = bsec_set_configuration(_impl->bsec_instance, bsec_config_iaq,
                                                             sizeof(bsec_config_iaq), work_buffer, sizeof(work_buffer));
    if (cfg_rslt != BSEC_OK) {
        mclog::tagWarn(_tag, "bsec_set_configuration failed: {}", (int)cfg_rslt);
    }

    bsec_sensor_configuration_t requested[7] = {
        {BSEC_SAMPLE_RATE_LP, BSEC_OUTPUT_IAQ},
        {BSEC_SAMPLE_RATE_LP, BSEC_OUTPUT_CO2_EQUIVALENT},
        {BSEC_SAMPLE_RATE_LP, BSEC_OUTPUT_RAW_TEMPERATURE},
        {BSEC_SAMPLE_RATE_LP, BSEC_OUTPUT_RAW_HUMIDITY},
        {BSEC_SAMPLE_RATE_LP, BSEC_OUTPUT_RAW_PRESSURE},
        {BSEC_SAMPLE_RATE_LP, BSEC_OUTPUT_RAW_GAS},
    };
    bsec_sensor_configuration_t required[8];
    uint8_t n_required = 8;
    bsec_library_return_t sub_rslt = bsec_update_subscription(_impl->bsec_instance, requested, 6, required, &n_required);
    if (sub_rslt != BSEC_OK) {
        mclog::tagWarn(_tag, "bsec_update_subscription failed: {}", (int)sub_rslt);
        return;
    }

    _impl->ok = true;
    mclog::tagInfo(_tag, "BSEC + BME688 init OK");
    xTaskCreatePinnedToCoreWithCaps(_update_task, "envsensor", 1024 * 8, _impl, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}

EnvSensorModule::~EnvSensorModule()
{
    if (_impl->bsec_instance) free(_impl->bsec_instance);
    delete _impl;
}

EnvSensorModule::Reading EnvSensorModule::getLastReading()
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->last_reading;
}

void EnvSensorModule::registerMcpTools(McpServer& mcp_server)
{
    if (!_impl->ok) return;

    mclog::tagInfo(_tag, "add sensors.get_environment tool");
    mcp_server.AddTool("self.sensors.get_environment",
        "Returns temperature (C), humidity (%), pressure (hPa), raw gas resistance (Ohms), "
        "Index for Air Quality (IAQ, 0-500 scale, lower is better), IAQ accuracy (0=stabilizing to 3=high), "
        "and estimated CO2 equivalent (ppm).",
        std::vector<Property>{}, [this](const PropertyList&) -> ReturnValue {
            auto r = getLastReading();
            if (!r.ok) return std::string(R"({"available": false})");
            return fmt::format(
                R"({{"available": true, "temperature_c": {:.1f}, "humidity_pct": {:.1f}, )"
                R"("pressure_hpa": {:.1f}, "gas_resistance_ohm": {}, "iaq": {:.0f}, )"
                R"("iaq_accuracy": {}, "co2_equivalent_ppm": {:.0f}}})",
                r.temperatureC, r.humidityPct, r.pressureHpa, r.gasResistanceOhm,
                r.iaq, r.iaqAccuracy, r.co2EquivalentPpm);
        });
}