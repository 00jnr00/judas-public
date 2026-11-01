// Pure mapping logic of the Strata backend: request lines, output lines, command line, config keys.
#include <stdexcept>

#include "core/config.h"
#include "engine/strata_proto.h"
#include "test.h"

using namespace judas;

static ModelConfig strata_model() {
    ModelConfig m;
    m.backend = "strata";
    m.path = "/m/Qwen-IQ2_XS-00001-of-00002.gguf";
    m.strata_pack = "/p/iq2_xs";
    m.strata_mtp = "/p/mtp/rt";
    return m;
}

static bool has_pair(const std::vector<std::string> & a, const std::string & k, const std::string & v) {
    for (size_t i = 0; i + 1 < a.size(); i++)
        if (a[i] == k && a[i + 1] == v) return true;
    return false;
}

int main() {
    // ---- GEN line
    {
        StrataSampling s;   // greedy defaults
        s.top_k = -1;
        CHECK(strata_gen_line(100, s, {1, 2, 3}) == "GEN 100 1,2,3");
        s.temp = 0.7f; s.top_p = 0.95f; s.min_p = 0.05f; s.top_k = 20; s.seed = 42;
        CHECK(strata_gen_line(8, s, {9}) == "GEN 8 temperature=0.699999988 top_p=0.949999988 top_k=20 min_p=0.0500000007 seed=42 9");
        s = StrataSampling{}; s.temp = 1; s.top_k = 0;
        CHECK(strata_gen_line(5, s, {7}).find("top_k=64") != std::string::npos);   // 0 (off) becomes the 64 cap
        s.top_k = 500;
        CHECK(strata_gen_line(5, s, {7}).find("top_k=64") != std::string::npos);
        CHECK(strata_gen_line(0, s, {}).rfind("GEN 1 ", 0) == 0);                  // max_new is at least 1
    }
    // ---- sampling defaults and overrides
    {
        ModelConfig m;
        m.temp = 0.6f; m.top_p = 0.9f; m.min_p = 0; m.top_k = 40;
        auto s = strata_sampling(-1, -1, -1, -1, 0xFFFFFFFFu, m);
        CHECK(s.temp == 0.6f && s.top_p == 0.9f && s.top_k == 40 && s.seed == 0);
        s = strata_sampling(0.0f, 0.5f, 0.1f, 5, 7, m);
        CHECK(s.temp == 0.0f && s.top_p == 0.5f && s.min_p == 0.1f && s.top_k == 5 && s.seed == 7);
    }
    // ---- shards
    {
        CHECK(strata_second_shard("/m/Qwen-IQ2_XS-00001-of-00002.gguf") == "/m/Qwen-IQ2_XS-00002-of-00002.gguf");
        CHECK(strata_second_shard("/m/single.gguf").empty());
        CHECK(strata_second_shard("/m/x-00001-of-00003.gguf").empty());
        CHECK(strata_second_shard("/m/x-00002-of-00002.gguf").empty());
        CHECK(strata_second_shard("").empty());
    }
    // ---- command line
    {
        auto a = strata_argv(strata_model(), 65536, "/s/data/expert-profile.bin");
        CHECK(a[0] == "--serve");
        CHECK(has_pair(a, "--pack", "/p/iq2_xs"));
        CHECK(has_pair(a, "--native", "/m/Qwen-IQ2_XS-00001-of-00002.gguf"));
        CHECK(has_pair(a, "--ple-gguf", "/m/Qwen-IQ2_XS-00002-of-00002.gguf"));
        CHECK(has_pair(a, "--max-context", "65536"));
        CHECK(has_pair(a, "--spec", "4") && has_pair(a, "--spec-min-p", "0.5"));
        CHECK(has_pair(a, "--kv", "int8") && has_pair(a, "--kv-resident", "32768"));
        CHECK(has_pair(a, "--vram-reserve-mib", "3072"));
        CHECK(has_pair(a, "--mtp", "/p/mtp/rt") && has_pair(a, "--expert-profile", "/s/data/expert-profile.bin"));
        CHECK(has_pair(a, "--expert-cache", "auto") && has_pair(a, "--prefill", "auto"));
        bool budget = false;
        for (auto & x : a) budget |= x == "--resident-budget-gib";
        CHECK(!budget);
        auto m = strata_model();
        m.strata_resident_budget_gib = 16;
        m.strata_extra_args = {"--foo", "bar"};
        a = strata_argv(m, 8192, "/x");
        CHECK(has_pair(a, "--resident-budget-gib", "16"));
        CHECK(a[a.size() - 2] == "--foo" && a.back() == "bar");
        m = strata_model();
        m.strata_mtp.clear();
        bool threw = false;
        try { strata_argv(m, 1, "/x"); } catch (const std::runtime_error & e) { threw = std::string(e.what()).find("strata_mtp") != std::string::npos; }
        CHECK(threw);
    }
    // ---- output lines
    {
        auto t = parse_strata_line("T 12345\n");
        CHECK(t.kind == StrataLine::Token && t.token == 12345);
        auto pp = parse_strata_line("PP 4096 30000 1500.5 2730.1");
        CHECK(pp.kind == StrataLine::Prompt && pp.pos == 4096 && pp.total == 30000 && pp.tok_s > 2730);
        CHECK(parse_strata_line("RESUME 2920").kind == StrataLine::Resume);
        CHECK(parse_strata_line("REUSED 2920").reused == 2920);
        auto d = parse_strata_line("DONE 69 3015 893 1274 stop 46 59 2920 100 200 1 2 3.5 95");
        CHECK(d.kind == StrataLine::Done && d.generated == 69 && d.prompt_tokens == 3015 && d.finish == "stop");
        CHECK(d.drafts_accepted == 46 && d.drafts_offered == 59 && d.prompt_read == 95 && d.decode_ms == 1274);
        auto d2 = parse_strata_line("DONE 3 10 5 6 cancel");
        CHECK(d2.kind == StrataLine::Done && d2.drafts_accepted == -1 && d2.prompt_read == -1);
        auto e = parse_strata_line("ERR bad request: max_new");
        CHECK(e.kind == StrataLine::Err && e.text == "bad request: max_new");
        auto r = parse_strata_line("READY 65536 stop");
        CHECK(r.kind == StrataLine::Ready && r.max_context == 65536 && r.can_stop);
        CHECK(!parse_strata_line("READY 4096").can_stop);
        CHECK(parse_strata_line("INFO engine=0.1.38").kind == StrataLine::Info);
        CHECK(parse_strata_line("T x").kind == StrataLine::Other);
        CHECK(parse_strata_line("").kind == StrataLine::Other);
        CHECK(parse_strata_line("garbage line").kind == StrataLine::Other);
    }
    // ---- finish mapping
    CHECK(strata_finish_reason("stop") == "stop");
    CHECK(strata_finish_reason("length") == "length");
    CHECK(strata_finish_reason("cancel") == "cancelled");
    // ---- utf8 holdback
    {
        CHECK(utf8_complete_prefix("") == 0);
        CHECK(utf8_complete_prefix("abc") == 3);
        CHECK(utf8_complete_prefix("caf\xC3\xA9") == 5);        // complete e-acute
        CHECK(utf8_complete_prefix("caf\xC3") == 3);            // lead byte alone
        CHECK(utf8_complete_prefix("\xE6\x97") == 0);           // 2 of 3 bytes
        CHECK(utf8_complete_prefix("x\xE6\x97\xA5") == 4);      // complete 3-byte
        CHECK(utf8_complete_prefix("x\xF0\x9F\x98") == 1);      // 3 of 4 bytes
        CHECK(utf8_complete_prefix("x\xF0\x9F\x98\x80") == 5);
    }
    // ---- config keys
    {
        const char * toml = R"(
[engine]
model = "titan"
[models.titan]
path = "/m/Qwen-IQ2_XS-00001-of-00002.gguf"
backend = "strata"
strata_pack = "/p/iq2_xs"
strata_resident_budget_gib = 16
strata_spec_min_p = 0.4
strata_extra_args = ["--a", "b"]
)";
        std::string path = std::string(JUDAS_TEST_TMP) + "/strata-config.toml";
        std::system(("mkdir -p " + std::string(JUDAS_TEST_TMP)).c_str());
        FILE * f = std::fopen(path.c_str(), "w");
        REQUIRE(f);
        std::fputs(toml, f);
        std::fclose(f);
        setenv("JUDAS_CONFIG", path.c_str(), 1);
        Config c = load_config();
        const auto & m = c.models.at("titan");
        CHECK(m.backend == "strata" && m.strata_pack == "/p/iq2_xs" && m.strata_resident_budget_gib == 16);
        CHECK(m.strata_spec_min_p == 0.4 && m.strata_extra_args.size() == 2 && m.strata_extra_args[1] == "b");
        CHECK(c.models.count("main") == 0 || c.models.at("main").backend == "llama");
    }
    TEST_MAIN_END();
}
