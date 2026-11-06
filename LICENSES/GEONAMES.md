# GeoNames cities data

`src/location/cities_data.cpp` embeds a compressed table of the world's cities with a population of 15,000 or more
(GeoNames `cities15000`), with their region names (`admin1CodesASCII`) and country names (`countryInfo`). Judas uses it
offline, inside the binary, to turn a position into the nearest city name for the `location_get` tool.

Data: GeoNames, https://www.geonames.org, licensed under the Creative Commons Attribution 4.0 License
(https://creativecommons.org/licenses/by/4.0/). The data was reduced to name, region, country, latitude and longitude
and re-encoded; no endorsement by GeoNames is implied.

Regenerate with `tools/gen-cities.py` (see its header; it needs the three GeoNames files from
https://download.geonames.org/export/dump/ and runs at development time only).
