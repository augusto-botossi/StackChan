// gps_module.h
#pragma once
#include <driver/uart.h>
#include <mutex>
#include <mcp_server.h>

class GpsModule {
public:
    struct Fix {
        bool valid = false;
        double latitude = 0.0;
        double longitude = 0.0;
        double altitudeM = 0.0;
        float speedKmh = 0.0f;
        int satellites = 0;
    };

    struct Impl;

    GpsModule(uart_port_t uart_num, int baud_rate, int tx_pin, int rx_pin);
    ~GpsModule();

    Fix getLastFix();
    void registerMcpTools(McpServer& mcp_server);

private:
    Impl* _impl;
};