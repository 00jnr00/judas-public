// Heartbeat collectors, text rendering and severity logic.
#include "heartbeat/heartbeat.h"
#include "test.h"

using namespace judas;

static Collector col(const char * n, const char * k, const char * a = "") { return Collector{n, k, a}; }

int main() {
    HeartbeatConfig cfg;
    cfg.collectors = {col("cpu", "cpu"), col("ram", "ram"), col("disk", "disk", "/"),
                      col("web", "http", "http://127.0.0.1:1/health")};
    json f;
    try { f = collect_facts(cfg); } catch (...) { CHECK(false); return 1; }
    for (auto n : {"cpu", "ram", "disk", "web"}) CHECK(f.contains(n));
    CHECK(f["cpu"]["text"].get<std::string>().find("load") != std::string::npos);
    CHECK(f["ram"]["text"].get<std::string>().find("GB") != std::string::npos);
    CHECK(f["disk"]["text"].get<std::string>().find("% used") != std::string::npos);
    CHECK(f["web"].contains("error") || f["web"]["text"].get<std::string>().find("DOWN") != std::string::npos);
    CHECK(f["web"].value("down", false));
    CHECK(f["cpu"].contains("ms"));

    // command collector
    HeartbeatConfig c2;
    c2.collectors = {col("hi", "command", "echo hello"), col("bad", "command", "exit 1"),
                     col("odd", "unknown"), col("nopath", "disk", "/nonexistent-xyz")};
    json g = collect_facts(c2);
    CHECK(g["hi"]["text"].get<std::string>().find("hello") != std::string::npos);
    CHECK(!g["hi"].contains("error"));
    CHECK(g["bad"].contains("error"));
    CHECK(g["odd"].contains("error"));
    CHECK(g["odd"]["error"].get<std::string>().find("unknown collector kind") != std::string::npos);
    CHECK(g["nopath"].contains("error"));

    // text rendering
    std::string t = facts_to_text(g);
    for (auto n : {"## hi", "## bad", "## odd", "## nopath"}) CHECK(t.find(n) != std::string::npos);
    CHECK(t.find("ERROR:") != std::string::npos);

    // dirgrowth and backups on a temp dir
    system("mkdir -p " JUDAS_TEST_TMP "/hb_test && echo x > " JUDAS_TEST_TMP "/hb_test/a.tar");
    HeartbeatConfig c3;
    c3.collectors = {col("dg", "dirgrowth", JUDAS_TEST_TMP "/hb_test"), col("bk", "backups", JUDAS_TEST_TMP "/hb_test/*.tar")};
    json h = collect_facts(c3);
    CHECK(!h["dg"].contains("error"));
    CHECK(!h["bk"].contains("error"));
    CHECK(h["bk"]["text"].get<std::string>().find("a.tar") != std::string::npos);

    // run_heartbeat: model severity wins when valid
    Summarizer good = [](const std::string &) { return std::make_pair(std::string("all good"), std::string("ok")); };
    HeartbeatConfig c4;
    c4.collectors = {col("cpu", "cpu")};
    HeartbeatReport r = run_heartbeat(c4, good);
    CHECK(r.summary == "all good");
    CHECK(r.severity == "ok");
    CHECK(r.ran_ms > 0);
    CHECK(!r.pushed);

    // invalid severity plus an errored collector: inferred critical
    Summarizer bogus = [](const std::string &) { return std::make_pair(std::string(), std::string("bogus")); };
    HeartbeatConfig c5;
    c5.collectors = {col("odd", "unknown")};
    CHECK(run_heartbeat(c5, bogus).severity == "critical");
    // invalid severity, healthy facts: ok
    HeartbeatConfig c6;
    c6.collectors = {col("cpu", "cpu")};
    CHECK(run_heartbeat(c6, bogus).severity == "ok");

    TEST_MAIN_END();
}
