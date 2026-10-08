#pragma once

#include <cstdint>
#include "esp_err.h"

struct PlaceResult {
    const char* name;      // ASCII town name, valid until the next LoadFromFile()
    const char* country;   // English country name
    float distance_km;     // distance from the query point to the town
};

// Offline "nearest town" lookup. The whole places.bin file is read once into
// PSRAM, so the SD card (and its shared SPI bus) is not needed afterwards.
class PlaceLookup {
public:
    static esp_err_t LoadFromFile(const char* path);
    static bool Loaded();
    // lat/lon in degrees. Returns false if no data is loaded.
    static bool Nearest(double lat, double lon, PlaceResult& out);
};
