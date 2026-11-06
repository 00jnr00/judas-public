#include "location/location.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <vector>

#include "location/cities.h"

namespace judas::location {

namespace {

constexpr double kPi = 3.14159265358979323846;
double rad(double d) { return d * kPi / 180.0; }

struct Table {
    std::vector<std::string> country_code, country_name, region;
    std::vector<std::string> name;
    std::vector<uint16_t> cc, reg;
    std::vector<float> lat, lon;
    bool ok = false;
};

// LZ4 block decoder (no frame): out_size is the known decoded size. false on corrupt input.
bool lz4_decode(const unsigned char * in, size_t in_size, std::string & out, size_t out_size) {
    out.clear();
    out.reserve(out_size);
    size_t ip = 0;
    while (ip < in_size) {
        unsigned token = in[ip++];
        size_t lit = token >> 4;
        if (lit == 15) {
            unsigned b;
            do {
                if (ip >= in_size) return false;
                b = in[ip++];
                lit += b;
            } while (b == 255);
        }
        if (ip + lit > in_size) return false;
        out.append(reinterpret_cast<const char *>(in + ip), lit);
        ip += lit;
        if (ip >= in_size) break;   // the last sequence has literals only
        if (ip + 2 > in_size) return false;
        size_t off = in[ip] | (in[ip + 1] << 8);
        ip += 2;
        size_t ml = token & 15;
        if (ml == 15) {
            unsigned b;
            do {
                if (ip >= in_size) return false;
                b = in[ip++];
                ml += b;
            } while (b == 255);
        }
        ml += 4;
        if (off == 0 || off > out.size()) return false;
        size_t from = out.size() - off;
        for (size_t i = 0; i < ml; i++) out.push_back(out[from + i]);   // byte by byte: matches may overlap
    }
    return out.size() == out_size;
}

uint32_t rd32(const std::string & s, size_t & p) { uint32_t v; std::memcpy(&v, s.data() + p, 4); p += 4; return v; }
uint16_t rd16(const std::string & s, size_t & p) { uint16_t v; std::memcpy(&v, s.data() + p, 2); p += 2; return v; }
std::string rdz(const std::string & s, size_t & p) {
    size_t e = s.find('\0', p);
    if (e == std::string::npos) e = s.size();
    std::string r = s.substr(p, e - p);
    p = e + 1;
    return r;
}

const Table & table() {
    static const Table t = [] {
        Table t;
        std::string raw;
        if (!lz4_decode(kCitiesPacked, kCitiesPackedSize, raw, kCitiesRawSize) || raw.size() < 8) return t;
        size_t p = 0;
        uint32_t n = rd32(raw, p);
        uint16_t nc = rd16(raw, p), nr = rd16(raw, p);
        for (int i = 0; i < nc; i++) {
            t.country_code.push_back(raw.substr(p, 2));
            p += 2;
            t.country_name.push_back(rdz(raw, p));
        }
        for (int i = 0; i < nr; i++) t.region.push_back(rdz(raw, p));
        t.name.reserve(n);
        for (uint32_t i = 0; i < n; i++) t.name.push_back(rdz(raw, p));
        if (p + (size_t) n * 10 > raw.size()) return t;
        t.cc.resize(n); t.reg.resize(n); t.lat.resize(n); t.lon.resize(n);
        for (uint32_t i = 0; i < n; i++) t.cc[i] = rd16(raw, p);
        for (uint32_t i = 0; i < n; i++) t.reg[i] = rd16(raw, p);
        auto r24 = [&](size_t q) {
            return (uint32_t) (unsigned char) raw[q] | ((uint32_t) (unsigned char) raw[q + 1] << 8) |
                   ((uint32_t) (unsigned char) raw[q + 2] << 16);
        };
        for (uint32_t i = 0; i < n; i++) t.lat[i] = (float) (r24(p + (size_t) i * 3) / 1000.0 - 90.0);
        p += (size_t) n * 3;
        for (uint32_t i = 0; i < n; i++) t.lon[i] = (float) (r24(p + (size_t) i * 3) / 1000.0 - 180.0);
        t.ok = true;
        return t;
    }();
    return t;
}

const char * compass8(double bearing) {
    static const char * const n[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return n[(int) std::floor(std::fmod(bearing + 22.5, 360.0) / 45.0) % 8];
}

std::string fmt(const char * f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

}  // namespace

double distance_km(double lat1, double lon1, double lat2, double lon2) {
    double dl = rad(lat2 - lat1), dn = rad(lon2 - lon1);
    double a = std::sin(dl / 2) * std::sin(dl / 2) + std::cos(rad(lat1)) * std::cos(rad(lat2)) * std::sin(dn / 2) * std::sin(dn / 2);
    return 6371.0088 * 2 * std::atan2(std::sqrt(a), std::sqrt(std::max(0.0, 1 - a)));
}

size_t city_count() { return table().name.size(); }

std::string place_label(const Place & p) {
    std::string s = p.city;
    if (!p.region.empty() && p.region != p.city) s += ", " + p.region;
    if (!p.country.empty()) s += ", " + p.country;
    return s;
}

Nearest nearest_place(double lat, double lon) {
    Nearest r;
    const Table & t = table();
    if (!t.ok || t.name.empty()) return r;
    size_t best = 0;
    double bd = 1e18;
    // the haversine "a" term is monotonic in distance, so rank by it and take the real distance once
    double cl = std::cos(rad(lat)), sl = rad(lat), sn = rad(lon);
    for (size_t i = 0; i < t.name.size(); i++) {
        double dl = std::sin((rad(t.lat[i]) - sl) / 2), dn = std::sin((rad(t.lon[i]) - sn) / 2);
        double a = dl * dl + cl * std::cos(rad(t.lat[i])) * dn * dn;
        if (a < bd) { bd = a; best = i; }
    }
    r.found = true;
    r.place.city = t.name[best];
    r.place.region = t.region[t.reg[best]];
    r.place.country = t.country_name[t.cc[best]];
    r.place.country_code = t.country_code[t.cc[best]];
    r.place.lat = t.lat[best];
    r.place.lon = t.lon[best];
    r.km = distance_km(r.place.lat, r.place.lon, lat, lon);
    // bearing from the city to the point
    double y = std::sin(rad(lon - r.place.lon)) * std::cos(rad(lat));
    double x = std::cos(rad(r.place.lat)) * std::sin(rad(lat)) - std::sin(rad(r.place.lat)) * std::cos(rad(lat)) * std::cos(rad(lon - r.place.lon));
    double b = std::atan2(y, x) * 180.0 / kPi;
    r.compass = compass8(b < 0 ? b + 360 : b);
    return r;
}

// ---- State ------------------------------------------------------------------------------------------------------

static bool num(const json & b, const char * k, double & out) {
    if (!b.contains(k) || !b[k].is_number()) return false;
    out = b[k].get<double>();
    return std::isfinite(out);
}

std::string State::report(const json & body, long long now_ms) {
    if (!body.is_object()) return "body must be a JSON object";
    Fix f;
    if (!num(body, "lat", f.lat) || !num(body, "lon", f.lon)) return "lat and lon are required numbers";
    if (f.lat < -90 || f.lat > 90 || f.lon < -180 || f.lon > 180) return "lat must be -90..90 and lon -180..180";
    double acc = 0;
    if (num(body, "accuracy", acc)) f.accuracy_m = std::clamp(acc, 0.0, 1e7);
    double ts = 0;
    if (num(body, "timestamp", ts) && ts > 0) f.fix_ms = (long long) ts;
    if (body.contains("device") && body["device"].is_string()) {
        f.device = body["device"].get<std::string>().substr(0, 40);
        for (auto & c : f.device) if ((unsigned char) c < 32) c = ' ';
    }
    f.reported_ms = now_ms;
    std::lock_guard lk(mu_);
    fix_ = f;
    sharing_ = true;
    return "";
}

void State::clear() {
    std::lock_guard lk(mu_);
    fix_.reset();
    sharing_ = false;
}

bool State::sharing() const {
    std::lock_guard lk(mu_);
    return sharing_;
}

std::optional<Fix> State::latest(long long now_ms) {
    std::lock_guard lk(mu_);
    if (fix_ && now_ms - fix_->reported_ms > kExpireMs) fix_.reset();
    return fix_;
}

json State::status_json(long long now_ms) {
    auto f = latest(now_ms);
    std::lock_guard lk(mu_);
    json j = {{"sharing", sharing_}, {"has_fix", f.has_value()}};
    if (f) {
        long long age = std::max(0LL, now_ms - f->reported_ms);
        j["age_s"] = age / 1000;
        j["stale"] = age > kFreshMs;
        j["device"] = f->device;
    }
    return j;
}

State & state() {
    static State s;
    return s;
}

static std::string age_text(long long ms) {
    long long s = std::max(0LL, ms) / 1000;
    if (s < 90) return std::to_string(s) + " seconds";
    if (s < 5400) return std::to_string((s + 30) / 60) + " minutes";
    return fmt("%.1f", s / 3600.0) + " hours";
}

Description describe(State & s, long long now_ms) {
    Description d;
    auto f = s.latest(now_ms);
    if (!f) {
        d.text = "No location available: the user is not sharing it. Ask him to turn on Settings > \"Share my location "
                 "with Judas\" (the toggle asks the browser or the app for permission), keep Judas open on the device he "
                 "is carrying, then try again. Do not guess a location.";
        return d;
    }
    long long age = now_ms - f->reported_ms;
    d.found = true;
    d.stale = age > kFreshMs;
    Nearest n = nearest_place(f->lat, f->lon);
    std::string t = "Latest fix: " + fmt("%.4f", f->lat) + ", " + fmt("%.4f", f->lon);
    if (f->accuracy_m > 0) t += " (accuracy about " + fmt("%.0f", f->accuracy_m) + " m)";
    t += "\nReported " + age_text(age) + " ago" + (d.stale ? " (STALE)" : "") + " from " + (f->device.empty() ? "an unknown device" : f->device) + ".";
    if (n.found) {
        if (n.km < 1.5) t += "\nNearest place: " + place_label(n.place) + " (in or right at it).";
        else t += "\nNearest listed city: " + place_label(n.place) + ", " + fmt("%.0f", n.km) + " km " + n.compass + " of it.";
        if (n.km > 80) t += " That is far from any listed city, so the user may be in a rural area or at sea.";
    }
    if (d.stale)
        t += "\nThis fix is old: he may have moved. If it matters, say so and ask him to open Judas on the device he is "
             "carrying so it refreshes.";
    if (f->accuracy_m > 5000) t += "\nAccuracy is coarse (a network or IP based guess); treat it as the city, not the street.";
    t += "\nThis is private: use it for the user's request only, never write it into memory or skills, and do not put it "
         "in web searches unless he asked for something local.";
    d.text = t;
    return d;
}

}  // namespace judas::location
