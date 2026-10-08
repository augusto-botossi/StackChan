#include "place_lookup.h"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char* TAG = "PlaceLookup";

namespace {

// places.bin layout (little endian), written by make_places_bin.py:
//   Header (16 bytes)
//   countries[header.countries] of char[28], NUL padded
//   places[header.places] of Place (40 bytes), sorted by nothing in particular
constexpr char kMagic[4]          = {'P', 'L', 'C', '1'};
constexpr size_t kCountryNameLen  = 28;

struct Header {
    char magic[4];
    uint32_t places;
    uint32_t countries;
    uint32_t reserved;
};

struct Place {
    int32_t lat_e5;    // degrees * 1e5
    int32_t lon_e5;
    uint8_t country;   // index into the country table
    char name[31];     // ASCII, NUL terminated
};

static_assert(sizeof(Header) == 16, "Header layout");
static_assert(sizeof(Place) == 40, "Place layout");

uint8_t* g_blob            = nullptr;
const char* g_countries    = nullptr;
const Place* g_places      = nullptr;
uint32_t g_place_count     = 0;
uint32_t g_country_count   = 0;

}  // namespace

esp_err_t PlaceLookup::LoadFromFile(const char* path)
{
    FILE* f = fopen(path, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }

    Header h;
    if (fread(&h, sizeof(h), 1, f) != 1 || memcmp(h.magic, kMagic, 4) != 0 || h.places == 0 ||
        h.places > 200000 || h.countries == 0 || h.countries > 256) {
        ESP_LOGW(TAG, "%s is not a valid places file", path);
        fclose(f);
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t country_bytes = (size_t)h.countries * kCountryNameLen;
    const size_t place_bytes   = (size_t)h.places * sizeof(Place);
    const size_t total         = country_bytes + place_bytes;

    uint8_t* blob = (uint8_t*)heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!blob) {
        ESP_LOGW(TAG, "No PSRAM for %u bytes", (unsigned)total);
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    if (fread(blob, 1, total, f) != total) {
        ESP_LOGW(TAG, "Short read from %s", path);
        heap_caps_free(blob);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    fclose(f);

    if (g_blob) {
        heap_caps_free(g_blob);
    }
    g_blob          = blob;
    g_countries     = (const char*)blob;
    g_places        = (const Place*)(blob + country_bytes);
    g_place_count   = h.places;
    g_country_count = h.countries;
    ESP_LOGI(TAG, "Loaded %u places, %u countries", (unsigned)g_place_count, (unsigned)g_country_count);
    return ESP_OK;
}

bool PlaceLookup::Loaded()
{
    return g_places != nullptr;
}

bool PlaceLookup::Nearest(double lat, double lon, PlaceResult& out)
{
    if (!g_places || g_place_count == 0) {
        return false;
    }

    const int32_t lat_e5 = (int32_t)lround(lat * 1e5);
    const int32_t lon_e5 = (int32_t)lround(lon * 1e5);
    // Longitude degrees shrink with latitude; a flat approximation is plenty
    // for "which town is closest".
    const float cos_lat = cosf((float)(lat * M_PI / 180.0));

    float best_d2    = 3.0e38f;
    uint32_t best_i  = 0;
    for (uint32_t i = 0; i < g_place_count; i++) {
        const int32_t dlat = g_places[i].lat_e5 - lat_e5;
        int32_t dlon       = g_places[i].lon_e5 - lon_e5;
        if (dlon > 18000000) {
            dlon -= 36000000;  // wrap across the date line
        } else if (dlon < -18000000) {
            dlon += 36000000;
        }
        const float dx = (float)dlon * cos_lat;
        const float dy = (float)dlat;
        const float d2 = dx * dx + dy * dy;
        if (d2 < best_d2) {
            best_d2 = d2;
            best_i  = i;
        }
    }

    const Place& p = g_places[best_i];
    out.name        = p.name;
    out.country     = (p.country < g_country_count) ? g_countries + (size_t)p.country * kCountryNameLen : "";
    out.distance_km = sqrtf(best_d2) / 1e5f * 111.195f;
    return true;
}
