// Location: the offline reverse geocoder, the single in-memory fix (sharing off, stale, expired), the location_get tool
// and the "no memory or skill writes after location_get" rule.
#include "location/location.h"
#include "store/store.h"
#include "test.h"
#include "tools/location_tools.h"
#include "tools/tools.h"

using namespace judas;
using namespace judas::location;

static void test_geocoder() {
    CHECK(city_count() > 30000);
    // Boulder, Colorado (40.0150, -105.2705) is in cities15000: it must resolve to Boulder, Colorado, United States
    auto n = nearest_place(40.0150, -105.2705);
    REQUIRE(n.found);
    CHECK(n.place.city == "Boulder" && n.place.region == "Colorado" && n.place.country_code == "US");
    CHECK(n.km < 3);
    CHECK(place_label(n.place) == "Boulder, Colorado, United States");
    // a spot a few km north-east of Boulder still resolves to a nearby Colorado city, with a distance and a direction
    auto m = nearest_place(40.08, -105.20);
    CHECK(m.found && m.place.region == "Colorado" && m.km > 0 && m.km < 30 && !m.compass.empty());
    // other known places (checked at country level where the nearest listed city may be a district)
    auto austin = nearest_place(30.2672, -97.7431);
    CHECK(austin.found && austin.place.city == "Austin" && austin.place.region == "Texas");
    auto paris = nearest_place(48.8566, 2.3522);
    CHECK(paris.found && paris.place.country_code == "FR" && paris.km < 10);
    auto sydney = nearest_place(-33.8688, 151.2093);
    CHECK(sydney.found && sydney.place.country_code == "AU");
    auto london = nearest_place(51.5074, -0.1278);
    CHECK(london.found && london.place.country_code == "GB" && london.km < 10);
    auto tokyo = nearest_place(35.6762, 139.6503);
    CHECK(tokyo.found && tokyo.place.country_code == "JP" && tokyo.km < 15);
    // the middle of the Pacific: something is returned, far away
    auto sea = nearest_place(0.0, -140.0);
    CHECK(sea.found && sea.km > 1000);
    // the antimeridian wraps
    auto fiji = nearest_place(-16.43, -179.9);   // east of the date line; the closest listed city is Labasa, Fiji (lon 179.4)
    CHECK(fiji.found && fiji.place.country_code == "FJ" && fiji.km < 200);
    CHECK(std::abs(distance_km(0, 0, 0, 1) - 111.2) < 0.5);
}

static void test_state() {
    State s;
    long long t0 = 1'000'000'000'000LL;
    CHECK(!s.sharing() && !s.latest(t0));
    auto d = describe(s, t0);
    CHECK(!d.found && d.text.find("Share my location with Judas") != std::string::npos);

    // validation
    CHECK(!s.report(json({{"lat", 91.0}, {"lon", 0.0}}), t0).empty());
    CHECK(!s.report(json({{"lat", 0.0}, {"lon", 181.0}}), t0).empty());
    CHECK(!s.report(json({{"lat", "x"}, {"lon", 0.0}}), t0).empty());
    CHECK(!s.report(json::array(), t0).empty());
    CHECK(!s.sharing() && !s.latest(t0));   // refused reports changed nothing

    CHECK(s.report(json({{"lat", 40.0150}, {"lon", -105.2705}, {"accuracy", 25.0}, {"timestamp", 123}, {"device", "Phone browser"}}), t0).empty());
    CHECK(s.sharing());
    d = describe(s, t0 + 30'000);
    CHECK(d.found && !d.stale);
    CHECK(d.text.find("Boulder, Colorado, United States") != std::string::npos);
    CHECK(d.text.find("Phone browser") != std::string::npos && d.text.find("30 seconds") != std::string::npos);
    CHECK(d.text.find("STALE") == std::string::npos);

    // a newer report replaces the old one (only the latest is kept)
    CHECK(s.report(json({{"lat", 39.7392}, {"lon", -104.9903}, {"device", "Desktop browser"}}), t0 + 60'000).empty());
    d = describe(s, t0 + 60'000);
    CHECK(d.text.find("Denver") != std::string::npos && d.text.find("Boulder") == std::string::npos);

    // stale: still returned, flagged, with the age
    d = describe(s, t0 + 60'000 + 20 * 60 * 1000);
    CHECK(d.found && d.stale && d.text.find("STALE") != std::string::npos && d.text.find("20 minutes") != std::string::npos);
    auto js = s.status_json(t0 + 60'000 + 20 * 60 * 1000);
    CHECK(js["stale"] == true && js["has_fix"] == true && !js.contains("lat") && !js.contains("lon"));

    // expired: forgotten, as if sharing had never produced a fix
    d = describe(s, t0 + 60'000 + 7LL * 3600 * 1000);
    CHECK(!d.found && d.text.find("not sharing") != std::string::npos);
    CHECK(!s.latest(t0 + 60'000 + 7LL * 3600 * 1000));

    // turning sharing off clears the fix at once
    s.report(json({{"lat", 1.0}, {"lon", 1.0}}), t0);
    s.clear();
    CHECK(!s.sharing() && !describe(s, t0).found);

    // far from any listed city: said so
    s.report(json({{"lat", 0.0}, {"lon", -140.0}}), t0);
    d = describe(s, t0);
    CHECK(d.found && d.text.find("far from any listed city") != std::string::npos);
    // coarse accuracy is called out
    s.report(json({{"lat", 40.0150}, {"lon", -105.2705}, {"accuracy", 20000.0}}), t0);
    CHECK(describe(s, t0).text.find("coarse") != std::string::npos);
}

static void test_tool_and_taint() {
    ToolRegistry reg;
    register_location_tools(reg);
    ToolContext ctx;
    bool noted = false;
    ctx.note_location = [&] { noted = true; };
    state().clear();
    auto r = reg.call("location_get", json::object(), ctx);
    CHECK(r.ok && r.output.find("Settings") != std::string::npos && !noted);   // sharing off: nothing noted
    state().report(json({{"lat", 40.0150}, {"lon", -105.2705}, {"device", "Judas Android app"}}), now_ms());
    r = reg.call("location_get", json::object(), ctx);
    CHECK(r.ok && r.output.find("Boulder") != std::string::npos && r.output.find("Judas Android app") != std::string::npos && noted);
    state().clear();

    // after location_get the chat cannot write memory or skills (enforced in the registry)
    ToolRegistry reg2;
    Tool mem;
    mem.name = "memory_append";
    mem.parameters = {{"type", "object"}, {"properties", json::object()}};
    mem.run = [](const json &, ToolContext &) { return ToolResult{true, "saved", false}; };
    reg2.add(mem);
    mem.name = "skill_manage";
    reg2.add(mem);
    mem.name = "web_search";
    reg2.add(mem);
    ToolContext c2;
    CHECK(reg2.call("memory_append", json::object(), c2).ok);
    c2.location_used = true;
    CHECK(!reg2.call("memory_append", json::object(), c2).ok);
    CHECK(!reg2.call("skill_manage", json::object(), c2).ok);
    CHECK(reg2.call("web_search", json::object(), c2).ok);   // not health data: the web stays available
    CHECK(location_taint_refusal("shell").empty());
}

int main() {
    test_geocoder();
    test_state();
    test_tool_and_taint();
    TEST_MAIN_END();
}
