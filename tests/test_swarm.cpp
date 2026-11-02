#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include "cpp-httplib/httplib.h"
#include "core/cpu_topology.h"
#include "test.h"
#include "tools/swarm_tools.h"

using namespace judas;
namespace fs = std::filesystem;

static void put(const fs::path & p, const std::string & s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

static void test_chunks() {
    CHECK(swarm_chunks("", 10).empty());
    CHECK(swarm_chunks("abc", 10).size() == 1);
    auto c = swarm_chunks("aaaa\nbbbb\ncccc\ndddd\n", 10);
    std::string joined;
    for (auto & p : c) { CHECK(p.size() <= 10); joined += p; }
    CHECK(joined == "aaaa\nbbbb\ncccc\ndddd\n");
    CHECK(c.size() >= 2);
    // never cut inside a UTF-8 sequence
    std::string u;
    for (int i = 0; i < 20; i++) u += "\xc3\xa9";
    auto cu = swarm_chunks(u, 7);
    std::string back;
    for (auto & p : cu) {
        CHECK(((unsigned char) p[0] & 0xC0) != 0x80);
        back += p;
    }
    CHECK(back == u);
}

static void test_glob() {
    CHECK(swarm_glob_match("*.md", "a.md"));
    CHECK(!swarm_glob_match("*.md", "d/a.md"));
    CHECK(swarm_glob_match("**/*.md", "a.md"));
    CHECK(swarm_glob_match("**/*.md", "d/e/a.md"));
    CHECK(swarm_glob_match("src/**/*.{cpp,h}", "src/x/y.h"));
    CHECK(!swarm_glob_match("src/**/*.{cpp,h}", "src/x/y.py"));
    CHECK(swarm_glob_match("a?.txt", "ab.txt"));
    CHECK(!swarm_glob_match("a.txt", "axtxt"));
}

static void test_run() {
    std::atomic<int> cur{0}, peak{0};
    size_t last_done = 0;
    auto res = swarm_run(
        20, 4,
        [&](size_t i) {
            int c = ++cur;
            int p = peak.load();
            while (c > p && !peak.compare_exchange_weak(p, c)) {}
            usleep(5000);
            --cur;
            if (i == 7) throw std::runtime_error("boom");
            return SwarmReply{true, std::to_string(i)};
        },
        [&](size_t done, size_t total) { CHECK(total == 20); CHECK(done == last_done + 1); last_done = done; });
    CHECK(res.size() == 20 && last_done == 20);
    CHECK(peak.load() <= 4 && peak.load() >= 2);
    CHECK(res[3].ok && res[3].text == "3");
    CHECK(!res[7].ok && res[7].text == "boom");
}

static void test_tool() {
    fs::path root = JUDAS_TEST_TMP "/judas-swarm-test-" + std::to_string(getpid());
    fs::remove_all(root);
    put(root / "work/docs/a.md", "alpha\n");
    put(root / "work/docs/b.md", "beta\n");
    put(root / "work/docs/sub/c.md", "gamma\n");
    put(root / "work/docs/bin.dat", std::string("x\0y", 3));
    put(root / "work/long.txt", std::string(3000, 'q') + "\n" + std::string(3000, 'r') + "\n");

    httplib::Server svr;
    std::atomic<int> hits{0};
    svr.Get("/health", [](const httplib::Request &, httplib::Response & r) { r.set_content("{}", "application/json"); });
    svr.Post("/v1/chat/completions", [&](const httplib::Request & req, httplib::Response & r) {
        hits++;
        json b = json::parse(req.body);
        std::string sys = b["messages"][0]["content"], user = b["messages"][1]["content"];
        json out = {{"choices", json::array({{{"message", {{"role", "assistant"}, {"content", sys + "|" + user.substr(0, 5)}}}}})}};
        r.set_content(out.dump(), "application/json");
    });
    int port = svr.bind_to_any_port("127.0.0.1");
    std::thread th([&] { svr.listen_after_bind(); });
    svr.wait_until_ready();

    Config cfg;
    cfg.permissions.allow_paths = {root.string()};
    cfg.permissions.mode = "auto-edit";
    cfg.swarm.enabled = true;
    cfg.swarm.url = "http://127.0.0.1:" + std::to_string(port);
    cfg.swarm.max_parallel = 3;
    cfg.swarm.max_item_chars = 2500;
    ToolRegistry reg;
    register_swarm_tool(reg);
    CHECK(reg.find("swarm_map"));
    ToolContext ctx;
    ctx.cwd = (root / "work").string();
    ctx.cfg = &cfg;
    std::atomic<size_t> last{0}, total{0};
    ctx.progress = [&](const json & d) { last = d["done"].get<size_t>(); total = d["total"].get<size_t>(); };

    json args = {{"instruction", "SUM"},
                 {"items", json::array({"docs/**/*.md", "docs/bin.dat", "long.txt", "text:hello world", "missing.txt"})},
                 {"output", (root / "work/out.jsonl").string()}};
    ToolResult r = reg.call("swarm_map", args, ctx);
    CHECK(r.ok);
    CHECK(r.output.find("swarm_map:") != std::string::npos);
    // 3 md + 3 chunks of long.txt (3000+3000 chars at 2500) + 1 text = 7 sent; bin + missing fail locally
    CHECK(hits.load() >= 7);
    CHECK(total.load() == hits.load() && last.load() == total.load());
    std::ifstream f(root / "work/out.jsonl");
    std::string line;
    int rows = 0, bad = 0;
    bool saw_text = false;
    while (std::getline(f, line)) {
        json j = json::parse(line);
        rows++;
        if (!j["ok"].get<bool>()) bad++;
        else if (j["id"] == "text:1") saw_text = j["text"] == "SUM|hello";
    }
    CHECK(saw_text);
    CHECK(bad == 2);
    CHECK(rows == hits.load() + 2);
    CHECK(r.output.find("binary file skipped") != std::string::npos);

    // default output location
    ToolResult r2 = reg.call("swarm_map", {{"instruction", "x"}, {"items", json::array({"text:one"})}}, ctx);
    CHECK(r2.ok && r2.output.find(".judas/swarm/") != std::string::npos);

    // bad args
    CHECK(!reg.call("swarm_map", {{"instruction", ""}, {"items", json::array({"text:a"})}}, ctx).ok);
    CHECK(!reg.call("swarm_map", {{"instruction", "x"}, {"items", json::array()}}, ctx).ok);

    svr.stop();
    th.join();

    // swarm down: a clear error
    cfg.swarm.url = "http://127.0.0.1:" + std::to_string(port);
    ToolResult r3 = reg.call("swarm_map", {{"instruction", "x"}, {"items", json::array({"text:one"})}}, ctx);
    CHECK(!r3.ok && r3.output.find("judas-swarm") != std::string::npos);
    fs::remove_all(root);
}

static void test_cores() {
    CHECK((parse_cpu_list("0-2,5,12-13") == std::vector<int>{0, 1, 2, 5, 12, 13}));
    CHECK(parse_cpu_list("").empty());

    // fake 2-CCD, 2-way SMT box: cpu0..3 on CCD A (siblings 0/4, 1/5), cpu2,3 on CCD B (siblings 2/6, 3/7)
    fs::path root = JUDAS_TEST_TMP "/judas-topo-test-" + std::to_string(getpid());
    fs::remove_all(root);
    int ccd[8] = {0, 0, 2, 2, 0, 0, 2, 2};
    for (int c = 0; c < 8; c++) {
        fs::path d = root / ("cpu" + std::to_string(c));
        put(d / "topology/thread_siblings_list", std::to_string(c % 4) + "," + std::to_string(c % 4 + 4) + "\n");
        put(d / "cache/index3/shared_cpu_list", ccd[c] == 0 ? "0-1,4-5\n" : "2-3,6-7\n");
    }
    CHECK((cpu_pin_order(root.string()) == std::vector<int>{0, 1, 2, 3, 4, 5, 6, 7}));
    CHECK((cpu_pin_set(3, root.string()) == std::vector<int>{0, 1, 2}));
    CHECK(cpu_pin_set(99, root.string()).size() == 8);
    CHECK(cpu_pin_set(0, root.string()).size() == 1);
    CHECK(!cpu_pin_order("/nonexistent-judas-topo").size());
    fs::remove_all(root);

    // the real machine: the first physical cores come first, no CPU twice
    auto order = cpu_pin_order();
    if (!order.empty()) {
        std::set<int> u(order.begin(), order.end());
        CHECK(u.size() == order.size());
    }

    // swarm_cores_set writes the instance's own config and says it was not live when nothing answers
    fs::path cfgdir = JUDAS_TEST_TMP "/judas-cores-test-" + std::to_string(getpid());
    fs::remove_all(cfgdir);
    put(cfgdir / "swarm.toml", "[engine]\nmodel = \"m\"\n\n[models.m]\npath = \"/x.gguf\"\nthreads = 10   # keep\n\n[models.m.sampling]\ntemp = 0.1\n");
    Config cfg;
    cfg.swarm.config = (cfgdir / "swarm.toml").string();
    cfg.swarm.url = "http://127.0.0.1:1";  // nothing listens
    SwarmCores g = swarm_cores_get(cfg);
    CHECK(g.ok && g.threads == 10 && !g.up && !g.pin);
    SwarmCores r = swarm_cores_set(cfg, 6, 1);
    CHECK(r.ok && !r.live && !r.up && r.threads == 6 && r.pin && r.cpus.size() == 6);
    SwarmCores g2 = swarm_cores_get(cfg);
    CHECK(g2.threads == 6 && g2.pin);
    std::ifstream in(cfg.swarm.config);
    std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(txt.find("threads = 6") != std::string::npos && txt.find("# keep") != std::string::npos);
    CHECK(txt.find("pin_cores = true") != std::string::npos && txt.find("temp = 0.1") != std::string::npos);
    CHECK(!swarm_cores_set(cfg, 0, -1).ok);
    CHECK(!swarm_cores_set(cfg, 100000, -1).ok);
    SwarmCores r2 = swarm_cores_set(cfg, 4, -1);  // keep pin
    CHECK(r2.ok && r2.pin && r2.threads == 4);
    CHECK(!swarm_cores_set(cfg, 4, 0).pin);
    fs::remove_all(cfgdir);
}

static void test_power() {
    CHECK(valid_unit_name("judas-swarm") && valid_unit_name("a@b.service") && valid_unit_name("x_y.1"));
    CHECK(!valid_unit_name("") && !valid_unit_name("a b") && !valid_unit_name("a;rm") && !valid_unit_name("$(x)"));
    fs::path d = fs::temp_directory_path() / ("judas-swarm-power-" + std::to_string(::getpid()));
    fs::create_directories(d);
    std::string f = (d / "config.toml").string();
    put(f, "# top\n[engine]\nmodel = \"m\"  # keep\n\n[swarm]\nenabled = false # note\nurl = \"http://x:1\"\n");
    CHECK(swarm_write_enabled(f, true));
    std::ifstream in(f);
    std::string txt((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK(txt.find("enabled = true") != std::string::npos && txt.find("enabled = false") == std::string::npos);
    CHECK(txt.find("# top") != std::string::npos && txt.find("model = \"m\"  # keep") != std::string::npos);
    CHECK(txt.find("url = \"http://x:1\"") != std::string::npos && txt.find("# note") != std::string::npos);
    // a file with no [swarm] section gets one
    put(f, "[engine]\nmodel = \"m\"\n");
    CHECK(swarm_write_enabled(f, false));
    std::ifstream in2(f);
    std::string t2((std::istreambuf_iterator<char>(in2)), std::istreambuf_iterator<char>());
    CHECK(t2.find("[swarm]") != std::string::npos && t2.find("enabled = false") != std::string::npos);
    // runtime flag follows the switch; an invalid unit is refused before anything is touched
    Config cfg;
    cfg.swarm.enabled = false;
    swarm_set_on(true);
    CHECK(swarm_is_on(cfg));
    swarm_set_on(false);
    CHECK(!swarm_is_on(cfg));
    cfg.swarm.unit = "bad unit;x";
    SwarmPower p = swarm_power(cfg, true);
    CHECK(!p.ok && !p.error.empty() && !p.enabled);
    // swarm_map answers clearly while off
    cfg.swarm.enabled = true;
    ToolRegistry reg;
    register_swarm_tool(reg);
    ToolContext ctx;
    ctx.cfg = &cfg;
    ToolResult r = reg.call("swarm_map", {{"instruction", "x"}, {"items", json::array({"text:a"})}}, ctx);
    CHECK(!r.ok && r.output.find("swarm is off") != std::string::npos);
    fs::remove_all(d);
}

int main() {
    test_chunks();
    test_glob();
    test_run();
    test_tool();
    test_cores();
    test_power();
    TEST_MAIN_END();
}
