#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "core/config.h"
#include "test.h"

using namespace judas;
namespace fs = std::filesystem;

static fs::path root;

static void write(const fs::path & p, const std::string & text) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << text;
}

static void clear_env() {
    unsetenv("JUDAS_CONFIG");
    unsetenv("JUDAS_PORT");
    unsetenv("JUDAS_DATA_DIR");
}

static std::string load_error(const std::string & project) {
    try {
        load_config(project);
    } catch (const std::exception & e) {
        return e.what();
    }
    return "";
}

static bool same_model(const ModelConfig & a, const ModelConfig & b) {
    return a.name == b.name && a.path == b.path && a.gpu_layers == b.gpu_layers && a.cpu_moe == b.cpu_moe &&
           a.kv_type == b.kv_type && a.kv_type_k == b.kv_type_k && a.kv_type_v == b.kv_type_v &&
           a.ctx_total == b.ctx_total && a.ctx_per_agent == b.ctx_per_agent &&
           a.ctx_per_agent_max == b.ctx_per_agent_max &&
           a.threads == b.threads && a.ubatch == b.ubatch && a.batch == b.batch && a.flash_attn == b.flash_attn &&
           a.temp == b.temp && a.top_p == b.top_p && a.min_p == b.min_p && a.top_k == b.top_k &&
           a.logit_bias == b.logit_bias && a.bias_file == b.bias_file && a.template_kwargs == b.template_kwargs &&
           a.chat_template_file == b.chat_template_file;
}

static bool same_hooks(const std::vector<Hook> & a, const std::vector<Hook> & b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (a[i].event != b[i].event || a[i].command != b[i].command || a[i].timeout_s != b[i].timeout_s ||
            a[i].blocking != b[i].blocking || a[i].match != b[i].match)
            return false;
    return true;
}

static bool same_heartbeat(const Config & x, const Config & y) {
    const auto & a = x.heartbeat;
    const auto & b = y.heartbeat;
    if (a.enabled != b.enabled || a.interval_s != b.interval_s || a.endpoint != b.endpoint || a.model != b.model ||
        a.checklist != b.checklist || a.quiet_hours != b.quiet_hours || a.channel != b.channel ||
        a.collectors.size() != b.collectors.size())
        return false;
    for (size_t i = 0; i < a.collectors.size(); i++)
        if (a.collectors[i].name != b.collectors[i].name || a.collectors[i].kind != b.collectors[i].kind ||
            a.collectors[i].arg != b.collectors[i].arg)
            return false;
    return true;
}

static bool same_channels(const Config & x, const Config & y) {
    const auto & a = x.channels;
    const auto & b = y.channels;
    if (a.def != b.def || a.channels.size() != b.channels.size()) return false;
    for (size_t i = 0; i < a.channels.size(); i++)
        if (a.channels[i].name != b.channels[i].name || a.channels[i].kind != b.channels[i].kind ||
            a.channels[i].target != b.channels[i].target || a.channels[i].enabled != b.channels[i].enabled)
            return false;
    return true;
}

static bool same(const Config & a, const Config & b) {
    if (!same_heartbeat(a, b) || !same_channels(a, b)) return false;
    if (a.models.size() != b.models.size()) return false;
    for (auto & [n, m] : a.models) {
        auto it = b.models.find(n);
        if (it == b.models.end() || !same_model(m, it->second)) return false;
    }
    return a.engine.model == b.engine.model && a.engine.max_agents == b.engine.max_agents &&
           a.engine.prefill_chunk == b.engine.prefill_chunk && a.engine.max_tokens == b.engine.max_tokens &&
           a.engine.park_dir == b.engine.park_dir && a.engine.prefix_cache == b.engine.prefix_cache && a.engine.kv_reserve == b.engine.kv_reserve &&
           a.server.bind == b.server.bind && a.server.port == b.server.port &&
           a.server.extra_binds == b.server.extra_binds && a.permissions.mode == b.permissions.mode &&
           a.permissions.allow_paths == b.permissions.allow_paths &&
           a.permissions.deny_paths == b.permissions.deny_paths &&
           a.permissions.deny_commands == b.permissions.deny_commands &&
           a.permissions.shell_timeout_s == b.permissions.shell_timeout_s && a.search.searxng == b.search.searxng &&
           a.search.fetch_command == b.search.fetch_command && a.agents.max_depth == b.agents.max_depth &&
           a.agents.max_steps == b.agents.max_steps && a.agents.role_dirs == b.agents.role_dirs &&
           a.agents.read_streak_nudge == b.agents.read_streak_nudge && a.agents.read_streak_force == b.agents.read_streak_force &&
           a.agents.auto_resume == b.agents.auto_resume && a.agents.auto_resume_max == b.agents.auto_resume_max &&
           a.agents.auto_resume_delay_s == b.agents.auto_resume_delay_s && a.memory.path == b.memory.path &&
           a.memory.inject_sections == b.memory.inject_sections &&
           a.data_dir == b.data_dir && a.permissions.sandbox == b.permissions.sandbox &&
           a.permissions.sandbox_net == b.permissions.sandbox_net &&
           a.git.commit_per_edit == b.git.commit_per_edit && a.git.branch_prefix == b.git.branch_prefix &&
           a.skills.enabled == b.skills.enabled && a.skills.paths == b.skills.paths &&
           a.skills.autoload == b.skills.autoload && a.prompt.max_memory_tokens == b.prompt.max_memory_tokens &&
           a.prompt.tool_schema == b.prompt.tool_schema && a.hooks.enabled == b.hooks.enabled &&
           same_hooks(a.hooks.hooks, b.hooks.hooks) && a.context.mode == b.context.mode &&
           a.context.trigger_pct == b.context.trigger_pct && a.context.target_pct == b.context.target_pct &&
           a.context.keep_turns == b.context.keep_turns && a.context.recall == b.context.recall &&
           a.context.clm_max_edits == b.context.clm_max_edits;
}

static void test_defaults() {
    clear_env();
    fs::path home = root / "home-defaults";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    Config c = load_config((root / "empty-project").string());
    CHECK(c.sources.empty());
    CHECK(c.engine.model == "main");
    CHECK(c.server.port == 8079);
    CHECK(c.permissions.mode == "auto-edit");
    REQUIRE(c.models.count("main") == 1);
    CHECK(c.models.size() == 1);
    auto & f = c.active_model();
    CHECK(f.name == "main");
    CHECK(f.kv_type == "q8_0" && f.ctx_total == 49152 && f.ctx_per_agent == 24576 && f.threads == 4 && f.ubatch == 256);
    CHECK(f.ctx_per_agent_max == 0);  // borrowing off by default
    CHECK(c.context.mode == "compact" && c.context.trigger_pct == 0.75 && c.context.target_pct == 0.45 &&
          c.context.keep_turns == 3 && c.context.recall && c.context.clm_max_edits == 4);
    CHECK(c.agents.read_streak_nudge == 8 && c.agents.read_streak_force == 14);
    CHECK(!c.agents.auto_resume && c.agents.auto_resume_max == 20 && c.agents.auto_resume_delay_s == 30);
    CHECK(c.prompt.max_memory_tokens == 3000);
    CHECK(c.memory.path == "~/.local/share/judas/memory.md");
    CHECK((c.memory.inject_sections == std::vector<std::string>{"About me", "Rules"}));
    CHECK(!c.memory.reject_dashes);
    CHECK((c.permissions.secret_paths == std::vector<std::string>{"~/.ssh", "~/.gnupg"}));
    CHECK(f.top_k == 20 && f.top_p == 0.95f && f.temp == 0.7f && f.min_p == 0.0f);
    CHECK(f.path == "~/models/model.gguf");
    CHECK(f.bias_file.empty() && f.logit_bias.empty());
    CHECK(c.permissions.sandbox == "bwrap" && c.permissions.sandbox_net);
    CHECK(!c.git.commit_per_edit && c.git.branch_prefix == "judas/");
    CHECK(c.skills.enabled && c.skills.autoload == "trigger");
    REQUIRE(c.skills.paths.size() == 1);
    CHECK(c.skills.paths[0] == "~/.config/judas/skills");
    CHECK(c.prompt.max_memory_tokens == 3000 && c.prompt.tool_schema == "full");
    CHECK(!c.hooks.enabled && c.hooks.hooks.empty());
}

static void test_phase3() {
    clear_env();
    fs::path home = root / "home-p3";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    fs::path f = root / "p3.toml";
    setenv("JUDAS_CONFIG", f.c_str(), 1);
    write(f,
          "[permissions]\nsandbox = \"none\"\nsandbox_net = false\n"
          "[git]\ncommit_per_edit = true\nbranch_prefix = \"j/\"\n"
          "[skills]\nenabled = false\npaths = [\"/a\", \"/b\"]\nautoload = \"on\"\n"
          "[prompt]\nmax_memory_tokens = 500\ntool_schema = \"lazy\"\n"
          "[agents]\nread_streak_nudge = 5\nread_streak_force = 9\nauto_resume = true\nauto_resume_max = 5\nauto_resume_delay_s = 7\n"
          "[memory]\npath = \"/m/x.md\"\ninject_sections = [\"Rules\", \"Projects\"]\n"
          "[hooks]\nenabled = true\n"
          "[[hooks.hooks]]\nevent = \"pre-edit\"\ncommand = \"echo hi\"\ntimeout_s = 5\nblocking = true\nmatch = \"src/\"\n"
          "[[hooks.hooks]]\nevent = \"turn-end\"\ncommand = \"true\"\n");
    Config c = load_config(root / "none");
    CHECK(c.permissions.sandbox == "none" && !c.permissions.sandbox_net);
    CHECK(c.git.commit_per_edit && c.git.branch_prefix == "j/");
    CHECK(!c.skills.enabled && c.skills.paths.size() == 2 && c.skills.autoload == "on");
    CHECK(c.prompt.max_memory_tokens == 500 && c.prompt.tool_schema == "lazy");
    CHECK(c.agents.read_streak_nudge == 5 && c.agents.read_streak_force == 9);
    CHECK(c.agents.auto_resume && c.agents.auto_resume_max == 5 && c.agents.auto_resume_delay_s == 7);
    CHECK(c.memory.path == "/m/x.md" && c.memory.inject_sections.size() == 2 && c.memory.inject_sections[1] == "Projects");
    CHECK(c.hooks.enabled);
    REQUIRE(c.hooks.hooks.size() == 2);
    CHECK(c.hooks.hooks[0].event == "pre-edit" && c.hooks.hooks[0].command == "echo hi");
    CHECK(c.hooks.hooks[0].timeout_s == 5 && c.hooks.hooks[0].blocking && c.hooks.hooks[0].match == "src/");
    CHECK(c.hooks.hooks[1].event == "turn-end" && c.hooks.hooks[1].timeout_s == 30);
    CHECK(!c.hooks.hooks[1].blocking && c.hooks.hooks[1].match.empty());

    // dump -> reload round trip
    write(root / "p3-rt.toml", dump_config(c));
    setenv("JUDAS_CONFIG", (root / "p3-rt.toml").c_str(), 1);
    Config d = load_config(root / "none");
    CHECK(same(c, d));
    CHECK(d.hooks.hooks.size() == 2);

    // unknown keys in the new sections are rejected
    setenv("JUDAS_CONFIG", f.c_str(), 1);
    write(f, "[git]\nbogus = 1\n");
    CHECK(load_error(root / "none").find("unknown key git.bogus") != std::string::npos);
    write(f, "[context]\nbogus = 1\n");
    CHECK(load_error(root / "none").find("unknown key context.bogus") != std::string::npos);
    write(f, "[[hooks.hooks]]\nevent = \"pre-tool\"\ncommand = \"x\"\nwhen = 1\n");
    CHECK(load_error(root / "none").find("unknown key hooks.hooks.when") != std::string::npos);

    // validation
    clear_env();
    write(home / "m.gguf", "x");
    Config v = load_config(root / "none");
    v.models["main"].path = "~/m.gguf";
    CHECK(validate_config(v).empty());
    Config b = v; b.permissions.sandbox = "docker";
    CHECK(validate_config(b).size() == 1);
    b = v; b.skills.autoload = "maybe";
    CHECK(validate_config(b).size() == 1);
    b = v; b.prompt.tool_schema = "tiny";
    CHECK(validate_config(b).size() == 1);
    b = v; b.hooks.hooks.push_back(Hook{"pre-tool", "x"});
    CHECK(validate_config(b).empty());
    b.hooks.hooks.push_back(Hook{"sometime", "x"});
    CHECK(validate_config(b).size() == 1);
}

static void test_phase4() {
    clear_env();
    fs::path home = root / "home-p4";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);

    // defaults
    Config d0 = load_config(root / "none");
    CHECK(!d0.heartbeat.enabled && d0.heartbeat.interval_s == 900 && d0.heartbeat.endpoint == "local");
    CHECK(d0.heartbeat.model.empty() && d0.heartbeat.checklist.empty() && d0.heartbeat.quiet_hours.empty());
    CHECK(d0.heartbeat.channel.empty() && d0.heartbeat.collectors.empty());
    CHECK(d0.channels.def.empty() && d0.channels.channels.empty());

    fs::path f = root / "p4.toml";
    setenv("JUDAS_CONFIG", f.c_str(), 1);
    write(f,
          "[heartbeat]\nenabled = true\ninterval_s = 60\nendpoint = \"http://100.1.2.3:8079/v1\"\n"
          "model = \"main\"\nchecklist = \"~/watch.md\"\nquiet_hours = \"23:00-07:00\"\nchannel = \"phone\"\n"
          "[[heartbeat.collectors]]\nname = \"root\"\nkind = \"disk\"\narg = \"/\"\n"
          "[[heartbeat.collectors]]\nname = \"cpu\"\nkind = \"cpu\"\n"
          "[channels]\ndef = \"phone\"\n"
          "[[channels.channels]]\nname = \"phone\"\ntarget = \"https://ntfy.sh/x\"\n"
          "[[channels.channels]]\nname = \"cmd\"\nkind = \"command\"\ntarget = \"cat\"\nenabled = false\n");
    Config c = load_config(root / "none");
    CHECK(c.heartbeat.enabled && c.heartbeat.interval_s == 60);
    CHECK(c.heartbeat.endpoint == "http://100.1.2.3:8079/v1" && c.heartbeat.model == "main");
    CHECK(c.heartbeat.checklist == "~/watch.md" && c.heartbeat.quiet_hours == "23:00-07:00");
    CHECK(c.heartbeat.channel == "phone");
    REQUIRE(c.heartbeat.collectors.size() == 2);
    CHECK(c.heartbeat.collectors[0].name == "root" && c.heartbeat.collectors[0].kind == "disk" &&
          c.heartbeat.collectors[0].arg == "/");
    CHECK(c.heartbeat.collectors[1].kind == "cpu" && c.heartbeat.collectors[1].arg.empty());
    CHECK(c.channels.def == "phone");
    REQUIRE(c.channels.channels.size() == 2);
    CHECK(c.channels.channels[0].name == "phone" && c.channels.channels[0].kind == "ntfy" &&
          c.channels.channels[0].target == "https://ntfy.sh/x" && c.channels.channels[0].enabled);
    CHECK(c.channels.channels[1].kind == "command" && !c.channels.channels[1].enabled);

    // round trip
    write(root / "p4-rt.toml", dump_config(c));
    setenv("JUDAS_CONFIG", (root / "p4-rt.toml").c_str(), 1);
    Config d = load_config(root / "none");
    CHECK(same(c, d));
    CHECK(d.heartbeat.collectors.size() == 2 && d.channels.channels.size() == 2);

    // unknown keys
    setenv("JUDAS_CONFIG", f.c_str(), 1);
    write(f, "[heartbeat]\nbogus = 1\n");
    CHECK(load_error(root / "none").find("unknown key heartbeat.bogus") != std::string::npos);
    write(f, "[[heartbeat.collectors]]\nname = \"a\"\nkind = \"cpu\"\nwhen = 1\n");
    CHECK(load_error(root / "none").find("unknown key heartbeat.collectors.when") != std::string::npos);
    write(f, "[channels]\nbogus = 1\n");
    CHECK(load_error(root / "none").find("unknown key channels.bogus") != std::string::npos);
    write(f, "[[channels.channels]]\nname = \"a\"\nwhen = 1\n");
    CHECK(load_error(root / "none").find("unknown key channels.channels.when") != std::string::npos);

    // validation
    clear_env();
    write(home / "m.gguf", "x");
    Config v = load_config(root / "none");
    v.models["main"].path = "~/m.gguf";
    CHECK(validate_config(v).empty());
    Config b = v;
    b.heartbeat.collectors.push_back(Collector{"u", "units", "x*"});
    b.channels.channels.push_back(Channel{"n", "ntfy", "https://ntfy.sh/x", true});
    CHECK(validate_config(b).empty());
    b = v; b.heartbeat.interval_s = 29;
    CHECK(validate_config(b).size() == 1);
    b.heartbeat.interval_s = 30;
    CHECK(validate_config(b).empty());
    b = v; b.heartbeat.collectors.push_back(Collector{"u", "bogus", ""});
    CHECK(validate_config(b).size() == 1);
    b = v; b.channels.channels.push_back(Channel{"n", "sms", "", true});
    CHECK(validate_config(b).size() == 1);
}

static void test_expand_home() {
    setenv("HOME", JUDAS_TEST_TMP "/fakehome", 1);
    CHECK(expand_home("~/x") == JUDAS_TEST_TMP "/fakehome/x");
    CHECK(expand_home("~") == JUDAS_TEST_TMP "/fakehome");
    CHECK(expand_home("/a/~b") == "/a/~b");
    CHECK(expand_home("rel") == "rel");
}

static void test_placeholder_dropped() {
    clear_env();
    fs::path home = root / "home-placeholder";
    setenv("HOME", home.c_str(), 1);
    write(home / ".config/judas/config.toml", "[engine]\nmodel = \"qwen\"\n[models.qwen]\npath = \"~/q.gguf\"\n");
    Config c = load_config((root / "empty-project").string());
    CHECK(c.models.count("main") == 0 && c.models.count("qwen") == 1 && c.active_model().name == "qwen");
    // a "main" the user actually configured stays
    write(home / ".config/judas/config.toml",
          "[engine]\nmodel = \"qwen\"\n[models.qwen]\npath = \"~/q.gguf\"\n[models.main]\npath = \"~/m.gguf\"\n");
    c = load_config((root / "empty-project").string());
    CHECK(c.models.count("main") == 1);
}

static void test_project_untrusted() {
    clear_env();
    fs::path home = root / "home-untrusted";
    setenv("HOME", home.c_str(), 1);
    write(home / ".config/judas/config.toml", "[permissions]\nmode = \"auto-edit\"\n");
    fs::path proj = root / "proj-evil";
    write(proj / ".judas/config.toml",
          "data_dir = \"/evil\"\n[permissions]\nmode = \"full-auto\"\nsandbox = \"none\"\nallow_paths = [\"/\"]\n"
          "[hooks]\nenabled = true\n[skills]\npaths = [\".judas/skills\"]\n[context]\nkeep_turns = 5\n");
    Config c = load_config(proj.string());
    CHECK(c.permissions.mode == "auto-edit" && c.permissions.sandbox == "bwrap");
    CHECK((c.permissions.allow_paths == std::vector<std::string>{"~"}));
    CHECK(!c.hooks.enabled && c.skills.paths[0] == "~/.config/judas/skills" && c.data_dir == "~/.local/share/judas");
    CHECK(c.context.keep_turns == 5);   // working style is fine
    setenv("JUDAS_PROJECT_TRUST", "1", 1);
    c = load_config(proj.string());
    CHECK(c.permissions.mode == "full-auto" && c.data_dir == "/evil");
    unsetenv("JUDAS_PROJECT_TRUST");
}

static void test_layers() {
    clear_env();
    fs::path home = root / "home-layers";
    setenv("HOME", home.c_str(), 1);
    write(home / ".config/judas/config.toml",
          "data_dir = \"/user/data\"\n[engine]\nmax_agents = 3\n[server]\nport = 9000\nextra_binds = [\"100.64.0.1\"]\n"
          "[models.main.sampling]\ntemp = 0.5\n");
    fs::path proj = root / "proj-layers";
    write(proj / ".judas/config.toml", "[server]\nport = 9100\n[permissions]\nmode = \"suggest\"\ndeny_commands = [\"rm -rf\"]\n");
    Config c = load_config(proj.string());
    CHECK(c.sources.size() == 2);
    CHECK(c.data_dir == "/user/data");
    CHECK(c.engine.max_agents == 3);
    CHECK(c.server.port == 9000);              // a project file cannot move the server
    CHECK(c.server.extra_binds.size() == 1);
    CHECK(c.permissions.mode == "suggest");    // but it may make permissions stricter
    CHECK(c.permissions.deny_commands.size() == 9);   // seeded 8 + "rm -rf" from the project
    CHECK(c.models["main"].temp == 0.5f);
    CHECK(c.models["main"].top_k == 20);  // untouched default

    setenv("JUDAS_PORT", "7777", 1);
    setenv("JUDAS_DATA_DIR", "/env/data", 1);
    c = load_config(proj.string());
    CHECK(c.server.port == 7777);
    CHECK(c.data_dir == "/env/data");

    // JUDAS_CONFIG replaces the user file
    clear_env();
    write(root / "alt.toml", "[engine]\nmax_agents = 5\n");
    setenv("JUDAS_CONFIG", (root / "alt.toml").c_str(), 1);
    c = load_config((root / "nope").string());
    CHECK(c.engine.max_agents == 5);
    CHECK(c.data_dir == "~/.local/share/judas");
    REQUIRE(c.sources.size() == 1);
    CHECK(c.sources[0] == (root / "alt.toml").string());
    clear_env();

    setenv("JUDAS_PORT", "abc", 1);
    CHECK(!load_error(proj.string()).empty());
    clear_env();
}

static void test_new_model_and_bias() {
    clear_env();
    fs::path home = root / "home-bias";
    setenv("HOME", home.c_str(), 1);
    write(home / "bias.txt", "1892-inf, 2218-inf, 5-1.5 ;comment\n 77-inf ,\n  9+2.5 ; trailing\n");
    write(home / ".config/judas/config.toml",
          "[engine]\nmodel = \"tiny\"\n"
          "[models.tiny]\npath = \"~/m/tiny.gguf\"\nkv_type = \"f16\"\nbias_file = \"~/bias.txt\"\n"
          "logit_bias = { 100 = \"-inf\", 101 = -2 }\n"
          "[models.tiny.sampling]\ntemp = 0.2\ntop_k = 40\n"
          "[models.tiny.template_kwargs]\nfoo = \"bar\"\n"
          "[models.pairs]\npath = \"/x.gguf\"\nlogit_bias = [[1, \"-inf\"], [2, 0.5]]\n");
    Config c = load_config(root / "none");
    auto & t = c.active_model();
    CHECK(t.name == "tiny" && t.path == "~/m/tiny.gguf" && t.kv_type == "f16");
    CHECK(t.top_k == 40 && t.temp == 0.2f);
    CHECK(t.template_kwargs.at("foo") == "bar");
    CHECK(t.bias_file == (home / "bias.txt").string());
    REQUIRE(t.logit_bias.size() == 7);
    CHECK(t.logit_bias[0].first == 100 && std::isinf(t.logit_bias[0].second) && t.logit_bias[0].second < 0);
    CHECK(t.logit_bias[1] == std::make_pair(101, -2.0f));
    CHECK(t.logit_bias[2].first == 1892 && std::isinf(t.logit_bias[2].second));
    CHECK(t.logit_bias[4] == std::make_pair(5, -1.5f));
    CHECK(t.logit_bias[6] == std::make_pair(9, 2.5f));
    auto & p = c.models["pairs"];
    REQUIRE(p.logit_bias.size() == 2);
    CHECK(p.logit_bias[1] == std::make_pair(2, 0.5f));

    // dump -> load round trip gives equal values (including the expanded bias list)
    std::string text = dump_config(c);
    write(root / "rt.toml", text);
    setenv("JUDAS_CONFIG", (root / "rt.toml").c_str(), 1);
    Config d = load_config(root / "none");
    CHECK(same(c, d));
    clear_env();

    // malformed bias file
    write(home / "badbias.txt", "12abc,3-inf");
    write(home / ".config/judas/config.toml", "[models.main]\nbias_file = \"~/badbias.txt\"\n");
    CHECK(load_error(root / "none").find("badbias.txt") != std::string::npos);
}

static void test_ctx_borrow_config() {
    clear_env();
    fs::path home = root / "home-borrow";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    fs::path f = root / "borrow.toml";
    setenv("JUDAS_CONFIG", f.c_str(), 1);
    write(f, "[models.main]\nctx_per_agent_max = 40000\n");
    Config c = load_config(root / "none");
    CHECK(c.models["main"].ctx_per_agent_max == 40000);
    CHECK(c.models["main"].ctx_per_agent == 24576);  // soft cap untouched

    // dump -> load round trip keeps the hard cap
    write(root / "borrow-rt.toml", dump_config(c));
    setenv("JUDAS_CONFIG", (root / "borrow-rt.toml").c_str(), 1);
    CHECK(same(c, load_config(root / "none")));
    CHECK(dump_config(c).find("ctx_per_agent_max = 40000") != std::string::npos);

    // validation: 0 (off) or at least ctx_per_agent, and never above the pool
    clear_env();
    write(home / "m.gguf", "x");
    Config v = load_config(root / "none");
    v.models["main"].path = "~/m.gguf";
    CHECK(validate_config(v).empty());
    Config b = v;
    b.models["main"].ctx_per_agent_max = 30000;
    CHECK(validate_config(b).empty());
    b.models["main"].ctx_per_agent_max = 3000;  // below the soft cap
    CHECK(validate_config(b).size() == 1);
    b.models["main"].ctx_per_agent_max = 60000;  // above the whole pool
    CHECK(validate_config(b).size() == 1);
    clear_env();
}

static void test_roundtrip_defaults() {
    clear_env();
    fs::path home = root / "home-rt";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    Config base = load_config(root / "none");
    write(root / "rt-dump.toml", dump_config(base));
    setenv("JUDAS_CONFIG", (root / "rt-dump.toml").c_str(), 1);
    CHECK(same(base, load_config(root / "none")));
    // the commented default file equals the built-in defaults
    std::string txt = default_config_toml();
    CHECK(txt.find("\xe2\x80\x94") == std::string::npos);  // no em dash
    write(root / "rt-default.toml", txt);
    setenv("JUDAS_CONFIG", (root / "rt-default.toml").c_str(), 1);
    Config e = load_config(root / "none");
    CHECK(same(base, e));
    clear_env();
    CHECK(dump_config(base).find("\xe2\x80\x94") == std::string::npos);
}

static void test_errors() {
    clear_env();
    fs::path home = root / "home-err";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    fs::path f = root / "err.toml";
    setenv("JUDAS_CONFIG", f.c_str(), 1);

    write(f, "[engine]\nmax_agent = 3\n");
    std::string e = load_error(root / "none");
    CHECK(e.find("unknown key engine.max_agent in " + f.string()) != std::string::npos);

    write(f, "[models.main.sampling]\ntemperature = 1\n");
    e = load_error(root / "none");
    CHECK(e.find("unknown key models.main.sampling.temperature") != std::string::npos);

    write(f, "bogus = 1\n");
    CHECK(load_error(root / "none").find("unknown key bogus") != std::string::npos);

    write(f, "[engine]\nmax_agents = \"three\"\n");
    e = load_error(root / "none");
    CHECK(e.find("engine.max_agents") != std::string::npos && e.find(f.string()) != std::string::npos);
    CHECK(e.find("line 2") != std::string::npos);

    write(f, "[engine]\nmax_agents = = 3\n");
    e = load_error(root / "none");
    CHECK(e.find(f.string() + ":2:") != std::string::npos);

    clear_env();
}

static void test_validate() {
    clear_env();
    fs::path home = root / "home-val";
    fs::create_directories(home);
    setenv("HOME", home.c_str(), 1);
    write(home / "m.gguf", "x");
    Config c = load_config(root / "none");
    c.engine.model = "main";
    c.models["main"].path = "~/m.gguf";
    CHECK(validate_config(c).empty());

    Config b = c;
    b.models["main"].path = "~/missing.gguf";
    CHECK(validate_config(b).size() == 1);
    b = c; b.engine.model = "nope";
    CHECK(!validate_config(b).empty());
    b = c; b.engine.max_agents = 0;
    CHECK(validate_config(b).size() == 1);
    b.engine.max_agents = 9;
    CHECK(validate_config(b).size() == 1);
    b = c; b.permissions.mode = "yolo";
    CHECK(validate_config(b).size() == 1);
    b = c; b.models["main"].kv_type = "q3_k";
    CHECK(validate_config(b).size() == 1);
    b = c; b.models["main"].kv_type = "q4_0"; b.models["main"].kv_type_k = "q8_0";  // valid quantized types
    CHECK(validate_config(b).empty());
    b.models["main"].kv_type_v = "q2_k";
    CHECK(validate_config(b).size() == 1);
    b = c; b.models["main"].flash_attn = false;  // quantized V needs flash attention
    CHECK(validate_config(b).size() == 1);
    b = c; b.server.port = 0;
    CHECK(validate_config(b).size() == 1);
    b.server.port = 70000;
    CHECK(validate_config(b).size() == 1);
    b = c; b.models["main"].ctx_per_agent = 200000;
    CHECK(validate_config(b).size() == 1);
    b = c; b.context.mode = "bogus";
    CHECK(validate_config(b).size() == 1);
    b = c; b.context.target_pct = 0.8; b.context.trigger_pct = 0.6;
    CHECK(validate_config(b).size() == 1);

    Config n = c;
    n.engine.model = "gone";
    bool threw = false;
    try { n.active_model(); } catch (const std::runtime_error &) { threw = true; }
    CHECK(threw);
}

int main() {
    root = fs::path(JUDAS_TEST_TMP "/judas-test-config");
    fs::remove_all(root);
    fs::create_directories(root);
    test_defaults();
    test_expand_home();
    test_layers();
    test_placeholder_dropped();
    test_project_untrusted();
    test_new_model_and_bias();
    test_ctx_borrow_config();
    test_roundtrip_defaults();
    test_errors();
    test_validate();
    test_phase3();
    test_phase4();
    std::printf("test_config: done\n");
    TEST_MAIN_END();
}
