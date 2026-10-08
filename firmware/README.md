## Data sources

Place names for the offline "which town are we in" lookup come from
[GeoNames](https://www.geonames.org) (`cities15000` and `countryInfo`),
licensed under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/).
The data is converted to a compact binary file (`places.bin`) with
`make_places_bin.py`: names are reduced to ASCII and only name, country and
coordinates are kept. The file is stored on the device's microSD card and is
not part of this repository.

## Build

### Fetch Dependencies

```bash
python3 ./fetch_repos.py
```

### Tool Chains

[ESP-IDF v5.5.4](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/index.html)

### Build

```bash
idf.py build
```

### Host-side tests

The motion coordinate helpers can be tested without ESP-IDF hardware:

```bash
cmake -S tests -B build-host-tests
cmake --build build-host-tests
ctest --test-dir build-host-tests --output-on-failure
```

### Flash

```bash
idf.py flash
```
