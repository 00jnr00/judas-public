// Location: where the user is right now, as reported by the client he is on (browser Geolocation in the desktop and
// phone pages, the WebView in the Android app). The server keeps ONLY the latest fix, in memory, with the time it was
// reported; nothing is written to disk and nothing is logged. A sharing toggle in Settings turns reporting on; turning it
// off clears the fix. The lead agent reads it with location_get, together with the nearest place name from an offline
// reverse geocoder (GeoNames cities15000 compiled into the binary, CC BY 4.0, see LICENSES/GEONAMES.md).
#pragma once

#include <mutex>
#include <optional>
#include <string>

#include "core/json.h"

namespace judas::location {

// ---- offline reverse geocoder -----------------------------------------------------------------------------------
struct Place {
    std::string city, region, country, country_code;
    double lat = 0, lon = 0;
};
struct Nearest {
    bool found = false;
    Place place;
    double km = 0;            // great-circle distance from the query point to the city
    std::string compass;      // direction from the city to the query point: N, NE, E ...
};
// Nearest listed city by great-circle distance (a full scan of the table; microseconds). found is false only if the
// table failed to load.
Nearest nearest_place(double lat, double lon);
size_t city_count();
double distance_km(double lat1, double lon1, double lat2, double lon2);   // haversine, mean Earth radius
// "Boulder, Colorado, United States" (empty parts dropped).
std::string place_label(const Place & p);

// ---- the latest fix ---------------------------------------------------------------------------------------------
struct Fix {
    double lat = 0, lon = 0;
    double accuracy_m = 0;       // 0 = unknown
    long long fix_ms = 0;        // when the device measured it (client clock), 0 = unknown
    long long reported_ms = 0;   // when this server received it
    std::string device;          // short label from the client: "Desktop browser", "Phone browser", "Judas Android app"
};

constexpr long long kFreshMs = 10LL * 60 * 1000;          // up to 10 minutes: current
constexpr long long kExpireMs = 6LL * 60 * 60 * 1000;     // past 6 hours: dropped, as if never shared

class State {
public:
    // "" on success, else why the report was refused (bad range, not a number). Marks sharing on.
    std::string report(const json & body, long long now_ms);
    void clear();                                          // sharing off: the fix is forgotten
    bool sharing() const;
    std::optional<Fix> latest(long long now_ms);           // drops (and forgets) an expired fix
    json status_json(long long now_ms);                    // {sharing, has_fix, age_s, stale, device} (no coordinates)
private:
    mutable std::mutex mu_;
    std::optional<Fix> fix_;
    bool sharing_ = false;
};
State & state();   // the process-wide instance the server and the tool share

// What location_get returns. found = false when there is nothing usable (the text then tells the model to ask the user
// to turn sharing on).
struct Description {
    bool found = false;
    bool stale = false;
    std::string text;
};
Description describe(State & s, long long now_ms);

}  // namespace judas::location
