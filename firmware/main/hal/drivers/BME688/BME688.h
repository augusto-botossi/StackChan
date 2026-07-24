#ifndef __BME688_H__
#define __BME688_H__

#include <stdint.h>
#include <stdbool.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BME688_VERSION "0.0.1"

/* ENV Pro ships with SDO tied high */
#define BME688_I2C_ADDR_HIGH 0x77
#define BME688_I2C_ADDR_LOW  0x76

typedef struct {
    i2c_master_bus_handle_t i2c_bus; /*!< I2C bus handle */
    uint8_t dev_addr;                /*!< Device address, default BME688_I2C_ADDR_HIGH */
} bme688_config_t;

typedef struct {
    bool ok;
    bool gas_valid;
    float temperature_c;
    float humidity_pct;
    float pressure_hpa;
    uint32_t gas_resistance_ohm; /*!< Raw, uncalibrated. Not a BSEC IAQ score. */
} bme688_reading_t;

typedef struct bme688_dev_t *bme688_handle_t;

/**
 * @brief Initialize BME688 device
 */
esp_err_t bme688_init(const bme688_config_t *config, bme688_handle_t *handle);

/**
 * @brief Delete BME688 device
 */
esp_err_t bme688_delete(bme688_handle_t handle);

/**
 * @brief Trigger a forced-mode measurement and read the result
 */
esp_err_t bme688_read(bme688_handle_t handle, bme688_reading_t *out);

#ifdef __cplusplus
}
#endif

#endif /* __BME688_H__ */