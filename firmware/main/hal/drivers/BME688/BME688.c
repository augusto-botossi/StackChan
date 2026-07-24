#include "BME688.h"
#include "bme68x.h"
#include <stdlib.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "BME688";

struct bme688_dev_t {
    i2c_master_dev_handle_t i2c_dev;
    struct bme68x_dev bme_dev;
    struct bme68x_conf conf;
    struct bme68x_heatr_conf heatr_conf;
};

static BME68X_INTF_RET_TYPE _i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t len, void *intf_ptr)
{
    bme688_handle_t dev = (bme688_handle_t)intf_ptr;
    esp_err_t err = i2c_master_transmit_receive(dev->i2c_dev, &reg_addr, 1, reg_data, len, 100);
    return (err == ESP_OK) ? 0 : -1;
}

static BME68X_INTF_RET_TYPE _i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t len, void *intf_ptr)
{
    bme688_handle_t dev = (bme688_handle_t)intf_ptr;
    uint8_t buf[16];
    if (len + 1 > sizeof(buf)) {
        return -1;
    }
    buf[0] = reg_addr;
    for (uint32_t i = 0; i < len; i++) {
        buf[1 + i] = reg_data[i];
    }
    esp_err_t err = i2c_master_transmit(dev->i2c_dev, buf, len + 1, 100);
    return (err == ESP_OK) ? 0 : -1;
}

static void _delay_us(uint32_t period, void *intf_ptr)
{
    (void)intf_ptr;
    esp_rom_delay_us(period);
}

esp_err_t bme688_init(const bme688_config_t *config, bme688_handle_t *handle)
{
    if (!config || !handle) {
        return ESP_ERR_INVALID_ARG;
    }

    bme688_handle_t dev = calloc(1, sizeof(struct bme688_dev_t));
    if (!dev) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = config->dev_addr,
        .scl_speed_hz    = 100000,
    };
    esp_err_t err = i2c_master_bus_add_device(config->i2c_bus, &dev_cfg, &dev->i2c_dev);
    if (err != ESP_OK) {
        free(dev);
        return err;
    }

    dev->bme_dev.intf = BME68X_I2C_INTF;
    dev->bme_dev.intf_ptr = dev;
    dev->bme_dev.read = _i2c_read;
    dev->bme_dev.write = _i2c_write;
    dev->bme_dev.delay_us = _delay_us;
    dev->bme_dev.amb_temp = 25;

    int8_t rslt = bme68x_init(&dev->bme_dev);
    if (rslt != BME68X_OK) {
        ESP_LOGE(TAG, "bme68x_init failed: %d", rslt);
        i2c_master_bus_rm_device(dev->i2c_dev);
        free(dev);
        return ESP_FAIL;
    }

    dev->conf.os_hum = BME68X_OS_2X;
    dev->conf.os_pres = BME68X_OS_4X;
    dev->conf.os_temp = BME68X_OS_8X;
    dev->conf.filter = BME68X_FILTER_OFF;
    dev->conf.odr = BME68X_ODR_NONE;
    bme68x_set_conf(&dev->conf, &dev->bme_dev);

    dev->heatr_conf.enable = BME68X_ENABLE;
    dev->heatr_conf.heatr_temp = 320;
    dev->heatr_conf.heatr_dur = 150;
    bme68x_set_heatr_conf(BME68X_FORCED_MODE, &dev->heatr_conf, &dev->bme_dev);

    ESP_LOGI(TAG, "init OK (chip_id=0x%02X, variant=%lu)", dev->bme_dev.chip_id,
             (unsigned long)dev->bme_dev.variant_id);

    *handle = dev;
    return ESP_OK;
}

esp_err_t bme688_delete(bme688_handle_t handle)
{
    if (!handle) {
        return ESP_ERR_INVALID_ARG;
    }
    i2c_master_bus_rm_device(handle->i2c_dev);
    free(handle);
    return ESP_OK;
}

esp_err_t bme688_read(bme688_handle_t handle, bme688_reading_t *out)
{
    if (!handle || !out) {
        return ESP_ERR_INVALID_ARG;
    }
    *out = (bme688_reading_t){0};

    int8_t rslt = bme68x_set_op_mode(BME68X_FORCED_MODE, &handle->bme_dev);
    if (rslt != BME68X_OK) {
        return ESP_FAIL;
    }

    uint32_t period_us = bme68x_get_meas_dur(BME68X_FORCED_MODE, &handle->conf, &handle->bme_dev)
                        + ((uint32_t)handle->heatr_conf.heatr_dur * 1000);
    vTaskDelay(pdMS_TO_TICKS(period_us / 1000 + 10));

    struct bme68x_data data;
    uint8_t n_fields = 0;
    rslt = bme68x_get_data(BME68X_FORCED_MODE, &data, &n_fields, &handle->bme_dev);
    if (rslt != BME68X_OK || n_fields == 0) {
        ESP_LOGW(TAG, "read failed or no data (rslt=%d, n_fields=%d)", rslt, n_fields);
        return ESP_FAIL;
    }

    out->ok = true;
    out->temperature_c = data.temperature;
    out->humidity_pct = data.humidity;
    out->pressure_hpa = data.pressure / 100.0f;
    out->gas_valid = (data.status & (BME68X_HEAT_STAB_MSK | BME68X_GASM_VALID_MSK)) != 0;
    if (out->gas_valid) {
        out->gas_resistance_ohm = data.gas_resistance;
    }

    return ESP_OK;
}