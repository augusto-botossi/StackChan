// gps_module.cc
#include "gps_module.h"
#include <mooncake_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <fmt/format.h>
#include <sstream>
#include <vector>

static const std::string_view _tag = "GpsModule";

struct GpsModule::Impl {
    uart_port_t uart_num;
    std::mutex mutex;
    Fix last_fix;
    std::string line_buffer;
};

static double _nmea_to_decimal(const std::string& raw, char hemisphere)
{
    if (raw.empty()) return 0.0;
    double val = std::stod(raw);
    int degrees = (int)(val / 100);
    double minutes = val - (degrees * 100);
    double decimal = degrees + minutes / 60.0;
    if (hemisphere == 'S' || hemisphere == 'W') decimal = -decimal;
    return decimal;
}

static bool _parse_gga(const std::string& line, GpsModule::Fix& fix)
{
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, ',')) f.push_back(tok);
    if (f.size() < 10) return false;

    if (f[6].empty() || f[6] == "0") {
        fix.valid = false;
        return false;
    }
    fix.valid = true;
    fix.latitude = _nmea_to_decimal(f[2], f[3].empty() ? 'N' : f[3][0]);
    fix.longitude = _nmea_to_decimal(f[4], f[5].empty() ? 'E' : f[5][0]);
    fix.satellites = f[7].empty() ? 0 : std::stoi(f[7]);
    fix.altitudeM = f[9].empty() ? 0.0 : std::stod(f[9]);
    return true;
}

static bool _parse_rmc(const std::string& line, GpsModule::Fix& fix)
{
    std::vector<std::string> f;
    std::stringstream ss(line);
    std::string tok;
    while (std::getline(ss, tok, ',')) f.push_back(tok);
    if (f.size() < 8) return false;
    if (!f[7].empty()) {
        fix.speedKmh = std::stof(f[7]) * 1.852f;
    }
    return true;
}

static void _update_task(void* param)
{

    auto* impl = static_cast<GpsModule::Impl*>(param);
    mclog::tagInfo(_tag, "start update task");

    uint8_t buf[256];
    while (1) {
        int len = uart_read_bytes(impl->uart_num, buf, sizeof(buf), pdMS_TO_TICKS(20));
        for (int i = 0; i < len; i++) {
            char c = (char)buf[i];
            if (c == '\n') {
                // mclog::tagInfo(_tag, "NMEA: {}", impl->line_buffer);   // <-- add this temporarily
                GpsModule::Fix fix;
                {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    fix = impl->last_fix;
                }
                bool updated = false;
                if (impl->line_buffer.rfind("$GPGGA", 0) == 0 || impl->line_buffer.rfind("$GNGGA", 0) == 0) {
                    updated = _parse_gga(impl->line_buffer, fix);
                } else if (impl->line_buffer.rfind("$GPRMC", 0) == 0 || impl->line_buffer.rfind("$GNRMC", 0) == 0) {
                    updated = _parse_rmc(impl->line_buffer, fix) || fix.valid;
                }
                if (updated) {
                    std::lock_guard<std::mutex> lock(impl->mutex);
                    impl->last_fix = fix;
                }
                impl->line_buffer.clear();
            } else if (c != '\r') {
                impl->line_buffer += c;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

GpsModule::GpsModule(uart_port_t uart_num, int baud_rate, int tx_pin, int rx_pin)
{
    _impl = new Impl();
    _impl->uart_num = uart_num;

    uart_config_t cfg = {
        .baud_rate = baud_rate,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_APB,
    };

    esp_err_t err = uart_driver_install(uart_num, 1024, 0, 0, NULL, 0);
    if (err != ESP_OK) {
        mclog::tagWarn(_tag, "uart_driver_install failed: {}", esp_err_to_name(err));
        return;
    }
    uart_param_config(uart_num, &cfg);
    uart_set_pin(uart_num, tx_pin, rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    mclog::tagInfo(_tag, "GPS UART ready");
    xTaskCreatePinnedToCoreWithCaps(_update_task, "gpsmodule", 1024 * 4, _impl, 2, NULL, 1, MALLOC_CAP_SPIRAM);
}

GpsModule::~GpsModule()
{
    uart_driver_delete(_impl->uart_num);
    delete _impl;
}

GpsModule::Fix GpsModule::getLastFix()
{
    std::lock_guard<std::mutex> lock(_impl->mutex);
    return _impl->last_fix;
}

void GpsModule::registerMcpTools(McpServer& mcp_server)
{
    mclog::tagInfo(_tag, "add sensors.get_location tool");
    mcp_server.AddTool("self.sensors.get_location",
                       "Returns the current GPS location: latitude, longitude, altitude (meters), speed (km/h), "
                       "and number of satellites in view. Returns available=false if no GPS fix yet.",
                       std::vector<Property>{}, [this](const PropertyList&) -> ReturnValue {
                           auto fix = getLastFix();
                           if (!fix.valid) {
                               return std::string(R"({"available": false})");
                           }
                           return fmt::format(
                               R"({{"available": true, "latitude": {:.6f}, "longitude": {:.6f}, )"
                               R"("altitude_m": {:.1f}, "speed_kmh": {:.1f}, "satellites": {}}})",
                               fix.latitude, fix.longitude, fix.altitudeM, fix.speedKmh, fix.satellites);
                       });
}