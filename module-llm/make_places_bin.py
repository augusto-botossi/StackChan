#!/usr/bin/env python3
"""Convert GeoNames cities15000.txt + countryInfo.txt into places.bin.

Usage: make_places_bin.py cities15000.txt countryInfo.txt places.bin

Data: GeoNames (https://www.geonames.org), licensed CC BY 4.0 - credit them
in the README / about screen.
"""
import struct
import sys

COUNTRY_LEN = 28   # bytes per country name, NUL padded
NAME_LEN = 31      # bytes per town name incl. NUL


def ascii_bytes(text, size):
    raw = text.encode("ascii", "replace").replace(b"?", b"")
    raw = raw[: size - 1]
    return raw.ljust(size, b"\0")


def main(cities_path, countries_path, out_path):
    country_names = {}
    with open(countries_path, encoding="utf-8") as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            cols = line.rstrip("\n").split("\t")
            country_names[cols[0]] = cols[4]

    codes = []          # country index -> ISO code
    code_index = {}
    places = []
    with open(cities_path, encoding="utf-8") as f:
        for line in f:
            cols = line.rstrip("\n").split("\t")
            if len(cols) < 9:
                continue
            ascii_name, lat, lon, cc = cols[2] or cols[1], float(cols[4]), float(cols[5]), cols[8]
            if cc not in code_index:
                code_index[cc] = len(codes)
                codes.append(cc)
            places.append((round(lat * 1e5), round(lon * 1e5), code_index[cc], ascii_name))

    if len(codes) > 256:
        sys.exit("more than 256 countries")

    with open(out_path, "wb") as out:
        out.write(b"PLC1" + struct.pack("<III", len(places), len(codes), 0))
        for cc in codes:
            out.write(ascii_bytes(country_names.get(cc, cc), COUNTRY_LEN))
        for lat_e5, lon_e5, idx, name in places:
            out.write(struct.pack("<iiB", lat_e5, lon_e5, idx) + ascii_bytes(name, NAME_LEN))

    print(f"{len(places)} places, {len(codes)} countries -> {out_path}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
