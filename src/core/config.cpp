// Config loading: defaults <- user file <- project file <- env. See config.h for the contract.
#include "core/config.h"

#include <pwd.h>
#include <unistd.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>

#define TOML_HEADER_ONLY 1
#include <toml++/toml.hpp>

namespace judas {

namespace fs = std::filesystem;

std::string expand_home(const std::string & path) {
    if (path.empty() || path[0] != '~') return path;
    if (path.size() > 1 && path[1] != '/') return path;  // "~user" is left alone
    std::string home;
    if (const char * h = std::getenv("HOME"); h && *h) {
        home = h;
    } else if (const passwd * pw = getpwuid(getuid())) {
        home = pw->pw_dir;
    } else {
        return path;
    }
    return home + path.substr(1);
}

const ModelConfig & Config::active_model() const {
    auto it = models.find(engine.model);
    if (it == models.end()) throw std::runtime_error("engine.model '" + engine.model + "' is not defined in [models]");
    return it->second;
}

namespace {

constexpr float kInf = std::numeric_limits<float>::infinity();

// ---------- defaults ----------

const char * const kDefaultModelPath = "~/models/model.gguf";

Config default_config() {
    Config c;
    // One placeholder model so `judas config init` and validate have something to point at. It is dropped
    // when the config selects another model and never pointed this one somewhere real.
    ModelConfig m;
    m.name = "main";
    m.path = kDefaultModelPath;
    m.kv_type = "q8_0";
    m.ctx_total = 49152;
    m.ctx_per_agent = 24576;
    m.threads = 4;
    m.ubatch = 256;
    m.temp = 0.7f;
    m.top_p = 0.95f;
    m.top_k = 20;
    m.min_p = 0.0f;
    c.models["main"] = m;

    // Seeded deny list. These are also hard-blocked in the shell tool regardless of config or mode (see
    // blocked_command in builtin_tools.cpp); listing them here makes the policy visible in `judas config show`.
    c.permissions.deny_commands = {"sudo", "doas", "pkexec", "su ", "passwd", "faillock", "rm -rf /", "mkfs"};
    return c;
}

// ---------- reading helpers ----------

struct Reader {
    std::string file;
};

[[noreturn]] void fail_at(const Reader & r, const std::string & msg, const toml::node * n) {
    std::string where = r.file;
    if (n) where += " (line " + std::to_string(n->source().begin.line) + ")";
    throw std::runtime_error(msg + " in " + where);
}

[[noreturn]] void unknown_key(const Reader & r, const std::string & key, const toml::node & n) {
    fail_at(r, "unknown key " + key, &n);
}

const toml::table & as_table(const Reader & r, const std::string & key, const toml::node & n) {
    if (auto * t = n.as_table()) return *t;
    fail_at(r, "key " + key + " must be a table", &n);
}

std::string get_str(const Reader & r, const std::string & key, const toml::node & n) {
    if (auto * v = n.as_string()) return v->get();
    fail_at(r, "key " + key + " must be a string", &n);
}

bool get_bool(const Reader & r, const std::string & key, const toml::node & n) {
    if (auto * v = n.as_boolean()) return v->get();
    fail_at(r, "key " + key + " must be a boolean", &n);
}

int get_int(const Reader & r, const std::string & key, const toml::node & n) {
    if (auto * v = n.as_integer()) {
        long long x = v->get();
        if (x < std::numeric_limits<int>::min() || x > std::numeric_limits<int>::max())
            fail_at(r, "key " + key + " is out of range", &n);
        return (int) x;
    }
    fail_at(r, "key " + key + " must be an integer", &n);
}

float get_float(const Reader & r, const std::string & key, const toml::node & n) {
    if (auto * v = n.as_floating_point()) return (float) v->get();
    if (auto * v = n.as_integer()) return (float) v->get();
    fail_at(r, "key " + key + " must be a number", &n);
}

double get_double(const Reader & r, const std::string & key, const toml::node & n) {
    if (auto * v = n.as_floating_point()) return v->get();
    if (auto * v = n.as_integer()) return (double) v->get();
    fail_at(r, "key " + key + " must be a number", &n);
}

std::vector<std::string> get_str_array(const Reader & r, const std::string & key, const toml::node & n) {
    auto * a = n.as_array();
    if (!a) fail_at(r, "key " + key + " must be an array of strings", &n);
    std::vector<std::string> out;
    for (auto & e : *a) out.push_back(get_str(r, key + "[]", e));
    return out;
}

// "-inf", "inf", "+inf" or a plain float. Returns false when the text is not a number.
bool parse_bias(const std::string & s, float & out) {
    if (s == "-inf" || s == "-Inf" || s == "-INF") { out = -kInf; return true; }
    if (s == "inf" || s == "+inf" || s == "Inf" || s == "INF") { out = kInf; return true; }
    if (s.empty()) return false;
    char * end = nullptr;
    float v = std::strtof(s.c_str(), &end);
    if (end != s.c_str() + s.size()) return false;
    out = v;
    return true;
}

float get_bias_value(const Reader & r, const std::string & key, const toml::node & n) {
    if (auto * v = n.as_string()) {
        float b;
        if (!parse_bias(v->get(), b)) fail_at(r, "key " + key + ": bad bias '" + v->get() + "'", &n);
        return b;
    }
    return get_float(r, key, n);
}

std::vector<std::pair<int, float>> get_logit_bias(const Reader & r, const std::string & key, const toml::node & n) {
    std::vector<std::pair<int, float>> out;
    if (auto * t = n.as_table()) {
        for (auto && [k, v] : *t) {
            std::string ks(k.str());
            char * end = nullptr;
            long id = std::strtol(ks.c_str(), &end, 10);
            if (ks.empty() || *end != '\0') fail_at(r, "key " + key + "." + ks + ": token id must be an integer", &v);
            out.emplace_back((int) id, get_bias_value(r, key + "." + ks, v));
        }
    } else if (auto * a = n.as_array()) {
        for (auto & e : *a) {
            auto * p = e.as_array();
            if (!p || p->size() != 2) fail_at(r, "key " + key + ": entries must be [id, bias] pairs", &e);
            out.emplace_back(get_int(r, key + "[0]", *p->get(0)), get_bias_value(r, key + "[1]", *p->get(1)));
        }
    } else {
        fail_at(r, "key " + key + " must be an inline table or an array of [id, bias] pairs", &n);
    }
    return out;
}

// ---------- applying one file ----------

void apply_model(const Reader & r, ModelConfig & m, const std::string & prefix, const toml::table & t) {
    for (auto && [k, v] : t) {
        std::string key(k.str()), full = prefix + "." + key;
        if (key == "path") m.path = get_str(r, full, v);
        else if (key == "gpu_layers") m.gpu_layers = get_int(r, full, v);
        else if (key == "cpu_moe") m.cpu_moe = get_bool(r, full, v);
        else if (key == "kv_type") m.kv_type = get_str(r, full, v);
        else if (key == "kv_type_k") m.kv_type_k = get_str(r, full, v);
        else if (key == "kv_type_v") m.kv_type_v = get_str(r, full, v);
        else if (key == "ctx_total") m.ctx_total = get_int(r, full, v);
        else if (key == "ctx_per_agent") m.ctx_per_agent = get_int(r, full, v);
        else if (key == "ctx_per_agent_max") m.ctx_per_agent_max = get_int(r, full, v);
        else if (key == "threads") m.threads = get_int(r, full, v);
        else if (key == "ubatch") m.ubatch = get_int(r, full, v);
        else if (key == "batch") m.batch = get_int(r, full, v);
        else if (key == "flash_attn") m.flash_attn = get_bool(r, full, v);
        else if (key == "logit_bias") m.logit_bias = get_logit_bias(r, full, v);
        else if (key == "bias_file") m.bias_file = get_str(r, full, v);
        else if (key == "chat_template_file") m.chat_template_file = get_str(r, full, v);
        else if (key == "url") m.url = get_str(r, full, v);
        else if (key == "unit") {
            m.unit = get_str(r, full, v);
            // it ends up in a systemctl command line: plain unit names only
            for (char ch : m.unit)
                if (!(std::isalnum((unsigned char) ch) || ch == '-' || ch == '_' || ch == '.' || ch == '@'))
                    fail_at(r, full + ": not a systemd unit name: " + m.unit, &v);
        }
        else if (key == "remote_model") m.remote_model = get_str(r, full, v);
        else if (key == "mmproj") m.mmproj = get_str(r, full, v);
        else if (key == "mmproj_gpu") m.mmproj_gpu = get_bool(r, full, v);
        else if (key == "image_max_tokens") m.image_max_tokens = get_int(r, full, v);
        else if (key == "vision") m.vision = get_bool(r, full, v);
        else if (key == "sampling") {
            const auto & s = as_table(r, full, v);
            for (auto && [sk, sv] : s) {
                std::string skey(sk.str()), sfull = full + "." + skey;
                if (skey == "temp") m.temp = get_float(r, sfull, sv);
                else if (skey == "top_p") m.top_p = get_float(r, sfull, sv);
                else if (skey == "min_p") m.min_p = get_float(r, sfull, sv);
                else if (skey == "top_k") m.top_k = get_int(r, sfull, sv);
                else unknown_key(r, sfull, sv);
            }
        } else if (key == "template_kwargs") {
            const auto & s = as_table(r, full, v);
            m.template_kwargs.clear();
            for (auto && [sk, sv] : s) {
                std::string sfull = full + "." + std::string(sk.str());
                std::string val;
                if (auto * str = sv.as_string()) val = str->get();
                else if (auto * b = sv.as_boolean()) val = b->get() ? "true" : "false";
                else if (auto * i = sv.as_integer()) val = std::to_string(i->get());
                else fail_at(r, "key " + sfull + " must be a string", &sv);
                m.template_kwargs[std::string(sk.str())] = val;
            }
        } else {
            unknown_key(r, full, v);
        }
    }
}

void apply_file(Config & c, const std::string & file) {
    Reader r{file};
    toml::table root;
    try {
        root = toml::parse_file(file);
    } catch (const toml::parse_error & e) {
        std::ostringstream os;
        os << file << ":" << e.source().begin.line << ":" << e.source().begin.column << ": " << e.description();
        throw std::runtime_error(os.str());
    }

    for (auto && [k, v] : root) {
        std::string key(k.str());
        if (key == "data_dir") {
            c.data_dir = get_str(r, key, v);
        } else if (key == "engine") {
            for (auto && [ek, ev] : as_table(r, key, v)) {
                std::string e(ek.str()), full = key + "." + e;
                if (e == "model") c.engine.model = get_str(r, full, ev);
                else if (e == "max_agents") c.engine.max_agents = get_int(r, full, ev);
                else if (e == "prefill_chunk") c.engine.prefill_chunk = get_int(r, full, ev);
                else if (e == "prefill_duty") c.engine.prefill_duty = get_double(r, full, ev);
                else if (e == "prefill_chunk_busy") c.engine.prefill_chunk_busy = get_int(r, full, ev);
                else if (e == "prefill_max_wait_ms") c.engine.prefill_max_wait_ms = get_int(r, full, ev);
                else if (e == "max_tokens") c.engine.max_tokens = get_int(r, full, ev);
                else if (e == "park_dir") c.engine.park_dir = get_str(r, full, ev);
                else if (e == "prefix_cache") c.engine.prefix_cache = get_int(r, full, ev);
                else if (e == "kv_reserve") c.engine.kv_reserve = get_int(r, full, ev);
                else unknown_key(r, full, ev);
            }
        } else if (key == "models") {
            for (auto && [mk, mv] : as_table(r, key, v)) {
                std::string name(mk.str()), full = key + "." + name;
                auto it = c.models.find(name);
                if (it == c.models.end()) {
                    ModelConfig m;
                    m.name = name;
                    it = c.models.emplace(name, m).first;
                }
                apply_model(r, it->second, full, as_table(r, full, mv));
            }
        } else if (key == "server") {
            for (auto && [sk, sv] : as_table(r, key, v)) {
                std::string s(sk.str()), full = key + "." + s;
                if (s == "bind") c.server.bind = get_str(r, full, sv);
                else if (s == "port") c.server.port = get_int(r, full, sv);
                else if (s == "extra_binds") c.server.extra_binds = get_str_array(r, full, sv);
                else if (s == "allowed_hosts") c.server.allowed_hosts = get_str_array(r, full, sv);
                else if (s == "cors_origins") c.server.cors_origins = get_str_array(r, full, sv);
                else unknown_key(r, full, sv);
            }
        } else if (key == "permissions") {
            for (auto && [pk, pv] : as_table(r, key, v)) {
                std::string p(pk.str()), full = key + "." + p;
                if (p == "mode") c.permissions.mode = get_str(r, full, pv);
                else if (p == "allow_paths") c.permissions.allow_paths = get_str_array(r, full, pv);
                else if (p == "deny_paths") c.permissions.deny_paths = get_str_array(r, full, pv);
                else if (p == "deny_commands") c.permissions.deny_commands = get_str_array(r, full, pv);
                else if (p == "secret_paths") c.permissions.secret_paths = get_str_array(r, full, pv);
                else if (p == "shell_timeout_s") c.permissions.shell_timeout_s = get_int(r, full, pv);
                else if (p == "sandbox") c.permissions.sandbox = get_str(r, full, pv);
                else if (p == "sandbox_net") c.permissions.sandbox_net = get_bool(r, full, pv);
                else if (p == "allow_unsandboxed_mcp") c.permissions.allow_unsandboxed_mcp = get_bool(r, full, pv);
                else unknown_key(r, full, pv);
            }
        } else if (key == "git") {
            for (auto && [gk, gv] : as_table(r, key, v)) {
                std::string g(gk.str()), full = key + "." + g;
                if (g == "commit_per_edit") c.git.commit_per_edit = get_bool(r, full, gv);
                else if (g == "branch_prefix") c.git.branch_prefix = get_str(r, full, gv);
                else unknown_key(r, full, gv);
            }
        } else if (key == "skills") {
            for (auto && [sk, sv] : as_table(r, key, v)) {
                std::string s(sk.str()), full = key + "." + s;
                if (s == "enabled") c.skills.enabled = get_bool(r, full, sv);
                else if (s == "paths") c.skills.paths = get_str_array(r, full, sv);
                else if (s == "autoload") c.skills.autoload = get_str(r, full, sv);
                else if (s == "manage") c.skills.manage = get_bool(r, full, sv);
                else if (s == "track_usage") c.skills.track_usage = get_bool(r, full, sv);
                else unknown_key(r, full, sv);
            }
        } else if (key == "learning") {
            for (auto && [lk, lv] : as_table(r, key, v)) {
                std::string l(lk.str()), full = key + "." + l;
                if (l == "review") c.learning.review = get_bool(r, full, lv);
                else if (l == "review_tool_steps") c.learning.review_tool_steps = get_int(r, full, lv);
                else if (l == "review_memory_turns") c.learning.review_memory_turns = get_int(r, full, lv);
                else if (l == "review_max_steps") c.learning.review_max_steps = get_int(r, full, lv);
                else if (l == "curator") c.learning.curator = get_bool(r, full, lv);
                else if (l == "curator_interval_hours") c.learning.curator_interval_hours = get_int(r, full, lv);
                else if (l == "curator_min_idle_minutes") c.learning.curator_min_idle_minutes = get_int(r, full, lv);
                else if (l == "stale_after_days") c.learning.stale_after_days = get_int(r, full, lv);
                else if (l == "archive_after_days") c.learning.archive_after_days = get_int(r, full, lv);
                else if (l == "curator_backups") c.learning.curator_backups = get_int(r, full, lv);
                else unknown_key(r, full, lv);
            }
        } else if (key == "prompt") {
            for (auto && [pk, pv] : as_table(r, key, v)) {
                std::string p(pk.str()), full = key + "." + p;
                if (p == "max_memory_tokens") c.prompt.max_memory_tokens = get_int(r, full, pv);
                else if (p == "tool_schema") c.prompt.tool_schema = get_str(r, full, pv);
                else unknown_key(r, full, pv);
            }
        } else if (key == "attachments") {
            for (auto && [ak, av] : as_table(r, key, v)) {
                std::string a(ak.str()), full = key + "." + a;
                if (a == "max_bytes") c.attachments.max_bytes = get_int(r, full, av);
                else if (a == "max_text_chars") c.attachments.max_text_chars = get_int(r, full, av);
                else if (a == "pdf_max_pages") c.attachments.pdf_max_pages = get_int(r, full, av);
                else if (a == "ocr") c.attachments.ocr = get_bool(r, full, av);
                else unknown_key(r, full, av);
            }
        } else if (key == "ui") {
            for (auto && [uk, uv] : as_table(r, key, v)) {
                std::string u(uk.str()), full = key + "." + u;
                if (u == "mascot") c.ui.mascot = get_bool(r, full, uv);
                else unknown_key(r, full, uv);
            }
        } else if (key == "memory") {
            for (auto && [mk, mv] : as_table(r, key, v)) {
                std::string m(mk.str()), full = key + "." + m;
                if (m == "path") c.memory.path = get_str(r, full, mv);
                else if (m == "inject_sections") c.memory.inject_sections = get_str_array(r, full, mv);
                else if (m == "reject_dashes") c.memory.reject_dashes = get_bool(r, full, mv);
                else unknown_key(r, full, mv);
            }
        } else if (key == "hooks") {
            for (auto && [hk, hv] : as_table(r, key, v)) {
                std::string h(hk.str()), full = key + "." + h;
                if (h == "enabled") {
                    c.hooks.enabled = get_bool(r, full, hv);
                } else if (h == "hooks") {
                    auto * arr = hv.as_array();
                    if (!arr) fail_at(r, "key " + full + " must be an array of tables ([[hooks.hooks]])", &hv);
                    c.hooks.hooks.clear();
                    for (auto & e : *arr) {
                        Hook hook;
                        for (auto && [ik, iv] : as_table(r, full + "[]", e)) {
                            std::string i(ik.str()), ifull = full + "." + i;
                            if (i == "event") hook.event = get_str(r, ifull, iv);
                            else if (i == "command") hook.command = get_str(r, ifull, iv);
                            else if (i == "timeout_s") hook.timeout_s = get_int(r, ifull, iv);
                            else if (i == "blocking") hook.blocking = get_bool(r, ifull, iv);
                            else if (i == "match") hook.match = get_str(r, ifull, iv);
                            else unknown_key(r, ifull, iv);
                        }
                        c.hooks.hooks.push_back(std::move(hook));
                    }
                } else {
                    unknown_key(r, full, hv);
                }
            }
        } else if (key == "search") {
            for (auto && [sk, sv] : as_table(r, key, v)) {
                std::string s(sk.str()), full = key + "." + s;
                if (s == "searxng") c.search.searxng = get_str(r, full, sv);
                else if (s == "fetch_command") c.search.fetch_command = get_str(r, full, sv);
                else unknown_key(r, full, sv);
            }
        } else if (key == "agents") {
            for (auto && [ak, av] : as_table(r, key, v)) {
                std::string a(ak.str()), full = key + "." + a;
                if (a == "max_depth") c.agents.max_depth = get_int(r, full, av);
                else if (a == "max_steps") c.agents.max_steps = get_int(r, full, av);
                else if (a == "max_auto_continues") c.agents.max_auto_continues = get_int(r, full, av);
                else if (a == "loop_repeats") c.agents.loop_repeats = get_int(r, full, av);
                else if (a == "loop_pause_after") c.agents.loop_pause_after = get_int(r, full, av);
                else if (a == "read_streak_nudge") c.agents.read_streak_nudge = get_int(r, full, av);
                else if (a == "read_streak_force") c.agents.read_streak_force = get_int(r, full, av);
                else if (a == "delegate_after_edits") c.agents.delegate_after_edits = get_int(r, full, av);
                else if (a == "role_dirs") c.agents.role_dirs = get_str_array(r, full, av);
                else if (a == "auto_resume") c.agents.auto_resume = get_bool(r, full, av);
                else if (a == "auto_resume_max") c.agents.auto_resume_max = get_int(r, full, av);
                else if (a == "auto_resume_delay_s") c.agents.auto_resume_delay_s = get_int(r, full, av);
                else unknown_key(r, full, av);
            }
        } else if (key == "context") {
            for (auto && [xk, xv] : as_table(r, key, v)) {
                std::string x(xk.str()), full = key + "." + x;
                if (x == "mode") c.context.mode = get_str(r, full, xv);
                else if (x == "trigger_pct") c.context.trigger_pct = get_double(r, full, xv);
                else if (x == "target_pct") c.context.target_pct = get_double(r, full, xv);
                else if (x == "keep_turns") c.context.keep_turns = get_int(r, full, xv);
                else if (x == "recall") c.context.recall = get_bool(r, full, xv);
                else if (x == "clm_max_edits") c.context.clm_max_edits = get_int(r, full, xv);
                else unknown_key(r, full, xv);
            }
        } else if (key == "heartbeat") {
            for (auto && [hk, hv] : as_table(r, key, v)) {
                std::string h(hk.str()), full = key + "." + h;
                if (h == "enabled") {
                    c.heartbeat.enabled = get_bool(r, full, hv);
                } else if (h == "interval_s") {
                    c.heartbeat.interval_s = get_int(r, full, hv);
                } else if (h == "endpoint") {
                    c.heartbeat.endpoint = get_str(r, full, hv);
                } else if (h == "model") {
                    c.heartbeat.model = get_str(r, full, hv);
                } else if (h == "checklist") {
                    c.heartbeat.checklist = get_str(r, full, hv);
                } else if (h == "quiet_hours") {
                    c.heartbeat.quiet_hours = get_str(r, full, hv);
                } else if (h == "channel") {
                    c.heartbeat.channel = get_str(r, full, hv);
                } else if (h == "collectors") {
                    auto * arr = hv.as_array();
                    if (!arr) fail_at(r, "key " + full + " must be an array of tables ([[heartbeat.collectors]])", &hv);
                    c.heartbeat.collectors.clear();
                    for (auto & e : *arr) {
                        Collector col;
                        for (auto && [ik, iv] : as_table(r, full + "[]", e)) {
                            std::string i(ik.str()), ifull = full + "." + i;
                            if (i == "name") col.name = get_str(r, ifull, iv);
                            else if (i == "kind") col.kind = get_str(r, ifull, iv);
                            else if (i == "arg") col.arg = get_str(r, ifull, iv);
                            else unknown_key(r, ifull, iv);
                        }
                        c.heartbeat.collectors.push_back(std::move(col));
                    }
                } else {
                    unknown_key(r, full, hv);
                }
            }
        } else if (key == "channels") {
            for (auto && [ck, cv] : as_table(r, key, v)) {
                std::string ch(ck.str()), full = key + "." + ch;
                if (ch == "def") {
                    c.channels.def = get_str(r, full, cv);
                } else if (ch == "channels") {
                    auto * arr = cv.as_array();
                    if (!arr) fail_at(r, "key " + full + " must be an array of tables ([[channels.channels]])", &cv);
                    c.channels.channels.clear();
                    for (auto & e : *arr) {
                        Channel chan;
                        for (auto && [ik, iv] : as_table(r, full + "[]", e)) {
                            std::string i(ik.str()), ifull = full + "." + i;
                            if (i == "name") chan.name = get_str(r, ifull, iv);
                            else if (i == "kind") chan.kind = get_str(r, ifull, iv);
                            else if (i == "target") chan.target = get_str(r, ifull, iv);
                            else if (i == "enabled") chan.enabled = get_bool(r, ifull, iv);
                            else unknown_key(r, ifull, iv);
                        }
                        c.channels.channels.push_back(std::move(chan));
                    }
                } else {
                    unknown_key(r, full, cv);
                }
            }
        } else if (key == "mcp") {
            for (auto && [mk, mv] : as_table(r, key, v)) {
                std::string m(mk.str()), full = key + "." + m;
                if (m == "enabled") {
                    c.mcp.enabled = get_bool(r, full, mv);
                } else if (m == "max_tools_per_agent") {
                    c.mcp.max_tools_per_agent = get_int(r, full, mv);
                } else if (m == "max_result_chars") {
                    c.mcp.max_result_chars = get_int(r, full, mv);
                } else if (m == "servers") {
                    for (auto && [sk, sv] : as_table(r, full, mv)) {
                        std::string name(sk.str()), sfull = full + "." + name;
                        McpServerConfig * sc = nullptr;
                        for (auto & e : c.mcp.servers) if (e.name == name) sc = &e;
                        if (!sc) {
                            c.mcp.servers.emplace_back();
                            sc = &c.mcp.servers.back();
                            sc->name = name;
                        }
                        for (auto && [ik, iv] : as_table(r, sfull, sv)) {
                            std::string i(ik.str()), ifull = sfull + "." + i;
                            if (i == "command") sc->command = get_str_array(r, ifull, iv);
                            else if (i == "cwd") sc->cwd = get_str(r, ifull, iv);
                            else if (i == "enabled") sc->enabled = get_bool(r, ifull, iv);
                            else if (i == "timeout_s") sc->timeout_s = get_int(r, ifull, iv);
                            else if (i == "startup_timeout_s") sc->startup_timeout_s = get_int(r, ifull, iv);
                            else if (i == "tools_allow") sc->tools_allow = get_str_array(r, ifull, iv);
                            else if (i == "tools_deny") sc->tools_deny = get_str_array(r, ifull, iv);
                            else if (i == "prefix") { sc->prefix = get_str(r, ifull, iv); sc->prefix_set = true; }
                            else if (i == "sandbox") sc->sandbox = get_bool(r, ifull, iv);
                            else if (i == "network") sc->network = get_bool(r, ifull, iv);
                            else if (i == "writable") sc->writable = get_str_array(r, ifull, iv);
                            else if (i == "trusted") sc->trusted = get_bool(r, ifull, iv);
                            else if (i == "protocol") sc->protocol = get_str(r, ifull, iv);
                            else if (i == "env") {
                                sc->env.clear();
                                for (auto && [ek, ev] : as_table(r, ifull, iv))
                                    sc->env[std::string(ek.str())] = get_str(r, ifull + "." + std::string(ek.str()), ev);
                            } else unknown_key(r, ifull, iv);
                        }
                    }
                } else {
                    unknown_key(r, full, mv);
                }
            }
        } else {
            unknown_key(r, key, v);
        }
    }
    c.sources.push_back(file);
}

// Read "1892-inf,2218-inf,..." and append to the model's logit_bias (ids already present are skipped).
void load_bias_file(ModelConfig & m) {
    std::string path = expand_home(m.bias_file);
    m.bias_file = path;
    std::ifstream in(path);
    if (!in) return;  // a missing bias file is not fatal
    std::string line, text;
    while (std::getline(in, line)) {
        if (auto p = line.find(';'); p != std::string::npos) line.erase(p);
        text += line;
        text += '\n';
    }
    auto bad = [&](const std::string & tok) {
        throw std::runtime_error("bad logit bias entry '" + tok + "' in " + path);
    };
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find_first_of(",\n", pos);
        if (end == std::string::npos) end = text.size();
        std::string tok = text.substr(pos, end - pos);
        pos = end + 1;
        // trim whitespace
        size_t a = tok.find_first_not_of(" \t\r\n");
        if (a == std::string::npos) continue;
        tok = tok.substr(a, tok.find_last_not_of(" \t\r\n") - a + 1);
        size_t d = 0;
        while (d < tok.size() && std::isdigit((unsigned char) tok[d])) d++;
        if (d == 0 || d >= tok.size() || (tok[d] != '-' && tok[d] != '+')) bad(tok);
        int id = std::atoi(tok.substr(0, d).c_str());
        float bias;
        if (!parse_bias(tok.substr(d), bias)) bad(tok);
        bool have = false;
        for (auto & e : m.logit_bias) if (e.first == id) { have = true; break; }
        if (!have) m.logit_bias.emplace_back(id, bias);
    }
}

}  // namespace

Config load_config(const std::string & project_dir) {
    Config c = default_config();

    std::string user_file;
    if (const char * e = std::getenv("JUDAS_CONFIG"); e && *e) user_file = expand_home(e);
    else user_file = expand_home("~/.config/judas/config.toml");
    std::error_code ec;
    if (fs::is_regular_file(user_file, ec)) apply_file(c, user_file);

    std::string proj_file = (fs::path(project_dir.empty() ? "." : project_dir) / ".judas" / "config.toml").string();
    if (fs::is_regular_file(proj_file, ec)) {
        // A project file must not be able to start programs just by being opened: MCP servers come from the
        // user file only (JUDAS_PROJECT_MCP=1 lets a project file define them).
        std::vector<McpServerConfig> user_servers = c.mcp.servers;
        // A repository's config is untrusted: by default it may only tune how agents work ([context], [prompt],
        // [agents] except role_dirs, [git]). Permissions, sandbox, hooks, skills, models, server, memory, data
        // and channels stay as the user set them, or opening a repo could switch off the sandbox or run commands.
        // JUDAS_PROJECT_TRUST=1 accepts the whole file.
        Config user = c;
        apply_file(c, proj_file);
        const char * trust = std::getenv("JUDAS_PROJECT_TRUST");
        if (!(trust && *trust && std::string(trust) != "0")) {
            Config proj = c;
            c = user;
            c.context = proj.context;
            c.prompt = proj.prompt;
            c.agents = proj.agents;
            c.agents.role_dirs = user.agents.role_dirs;
            c.git = proj.git;
            c.sources = proj.sources;
            c.mcp.servers = proj.mcp.servers;   // filtered below like before
            c.mcp.max_tools_per_agent = proj.mcp.max_tools_per_agent;
            // It may still make permissions stricter: a lower mode, and more denied commands and paths.
            auto rank = [](const std::string & m) { return m == "suggest" ? 0 : m == "auto-edit" ? 1 : 2; };
            if (rank(proj.permissions.mode) < rank(c.permissions.mode)) c.permissions.mode = proj.permissions.mode;
            auto add_all = [](std::vector<std::string> & to, const std::vector<std::string> & from) {
                for (auto & x : from)
                    if (std::find(to.begin(), to.end(), x) == to.end()) to.push_back(x);
            };
            add_all(c.permissions.deny_commands, proj.permissions.deny_commands);
            add_all(c.permissions.deny_paths, proj.permissions.deny_paths);
            add_all(c.permissions.secret_paths, proj.permissions.secret_paths);
        }
        const char * allow = std::getenv("JUDAS_PROJECT_MCP");
        if (!(allow && *allow && std::string(allow) != "0")) {
            for (auto & s : c.mcp.servers) {
                bool known = false;
                for (auto & u : user_servers) if (u.name == s.name) known = true;
                if (!known) c.mcp.ignored_project_servers.push_back(s.name);
            }
            c.mcp.servers = user_servers;
        }
    }

    if (const char * p = std::getenv("JUDAS_PORT"); p && *p) {
        char * end = nullptr;
        long v = std::strtol(p, &end, 10);
        if (*end != '\0') throw std::runtime_error(std::string("JUDAS_PORT is not an integer: ") + p);
        c.server.port = (int) v;
    }
    if (const char * d = std::getenv("JUDAS_DATA_DIR"); d && *d) c.data_dir = d;

    if (c.engine.model != "main") {
        auto it = c.models.find("main");
        if (it != c.models.end() && it->second.path == kDefaultModelPath && it->second.url.empty()) c.models.erase(it);
    }
    for (auto & [name, m] : c.models) {
        m.name = name;
        if (!m.bias_file.empty()) load_bias_file(m);
    }
    return c;
}

std::vector<std::string> validate_config(const Config & cfg) {
    std::vector<std::string> p;
    auto it = cfg.models.find(cfg.engine.model);
    if (it == cfg.models.end()) {
        p.push_back("engine.model '" + cfg.engine.model + "' is not defined in [models]");
    } else {
        std::error_code ec;
        if (!it->second.url.empty()) {}  // remote model: no local file
        else if (it->second.path.empty()) p.push_back("models." + it->first + ".path is not set");
        else if (!fs::exists(expand_home(it->second.path), ec))
            p.push_back("models." + it->first + ".path does not exist: " + expand_home(it->second.path));
    }
    for (auto & [name, m] : cfg.models) {
        auto kv_ok = [](const std::string & t) {
            return t == "f16" || t == "q8_0" || t == "q5_1" || t == "q5_0" || t == "q4_1" || t == "q4_0";
        };
        if (!kv_ok(m.kv_type))
            p.push_back("models." + name + ".kv_type must be f16, q8_0, q5_1, q5_0, q4_1 or q4_0, got '" + m.kv_type + "'");
        for (auto [key, val] : {std::pair<const char *, const std::string *>{"kv_type_k", &m.kv_type_k},
                                {"kv_type_v", &m.kv_type_v}})
            if (!val->empty() && !kv_ok(*val))
                p.push_back("models." + name + "." + key + " must be empty or a kv_type value, got '" + *val + "'");
        {
            const std::string & vt = m.kv_type_v.empty() ? m.kv_type : m.kv_type_v;
            if (vt != "f16" && !m.flash_attn)
                p.push_back("models." + name + ": a quantized V cache (" + vt + ") needs flash_attn = true");
        }
        if (m.ctx_per_agent > m.ctx_total)
            p.push_back("models." + name + ".ctx_per_agent (" + std::to_string(m.ctx_per_agent) +
                        ") exceeds ctx_total (" + std::to_string(m.ctx_total) + ")");
        if (m.ctx_per_agent_max != 0 && m.ctx_per_agent_max < m.ctx_per_agent)
            p.push_back("models." + name + ".ctx_per_agent_max (" + std::to_string(m.ctx_per_agent_max) +
                        ") must be 0 (off) or at least ctx_per_agent (" + std::to_string(m.ctx_per_agent) + ")");
        if (m.ctx_per_agent_max > m.ctx_total)
            p.push_back("models." + name + ".ctx_per_agent_max (" + std::to_string(m.ctx_per_agent_max) +
                        ") exceeds ctx_total (" + std::to_string(m.ctx_total) + ")");
    }
    if (cfg.engine.max_agents < 1 || cfg.engine.max_agents > 8)
        p.push_back("engine.max_agents must be in 1..8, got " + std::to_string(cfg.engine.max_agents));
    if (!(cfg.engine.prefill_duty > 0.0 && cfg.engine.prefill_duty <= 1.0))
        p.push_back("engine.prefill_duty must be > 0 and <= 1, got " + std::to_string(cfg.engine.prefill_duty));
    if (cfg.engine.prefill_chunk_busy < 0)
        p.push_back("engine.prefill_chunk_busy must be >= 0, got " + std::to_string(cfg.engine.prefill_chunk_busy));
    if (cfg.engine.prefill_max_wait_ms < 0)
        p.push_back("engine.prefill_max_wait_ms must be >= 0, got " + std::to_string(cfg.engine.prefill_max_wait_ms));
    const auto & mode = cfg.permissions.mode;
    if (mode != "suggest" && mode != "auto-edit" && mode != "full-auto")
        p.push_back("permissions.mode must be suggest, auto-edit or full-auto, got '" + mode + "'");
    if (cfg.server.port < 1 || cfg.server.port > 65535)
        p.push_back("server.port must be in 1..65535, got " + std::to_string(cfg.server.port));
    const auto & sb = cfg.permissions.sandbox;
    if (sb != "none" && sb != "bwrap")
        p.push_back("permissions.sandbox must be none or bwrap, got '" + sb + "'");
    const auto & al = cfg.skills.autoload;
    if (al != "off" && al != "trigger" && al != "on")
        p.push_back("skills.autoload must be off, trigger or on, got '" + al + "'");
    {
        const auto & lc = cfg.learning;
        if (lc.curator_interval_hours < 1)
            p.push_back("learning.curator_interval_hours must be at least 1, got " +
                        std::to_string(lc.curator_interval_hours));
        if (lc.stale_after_days < 0)
            p.push_back("learning.stale_after_days must be 0 or more, got " + std::to_string(lc.stale_after_days));
        if (lc.archive_after_days < lc.stale_after_days)
            p.push_back("learning.archive_after_days (" + std::to_string(lc.archive_after_days) +
                        ") must be at least stale_after_days (" + std::to_string(lc.stale_after_days) + ")");
    }
    const auto & ts = cfg.prompt.tool_schema;
    if (ts != "full" && ts != "lazy")
        p.push_back("prompt.tool_schema must be full or lazy, got '" + ts + "'");
    const auto & cx = cfg.context;
    if (cx.mode != "append" && cx.mode != "compact" && cx.mode != "clm")
        p.push_back("context.mode must be append, compact or clm, got '" + cx.mode + "'");
    if (!(cx.target_pct > 0.0 && cx.target_pct < cx.trigger_pct && cx.trigger_pct <= 0.95))
        p.push_back("context needs 0 < target_pct < trigger_pct <= 0.95, got target_pct " +
                    std::to_string(cx.target_pct) + " and trigger_pct " + std::to_string(cx.trigger_pct));
    if (cx.keep_turns < 1)
        p.push_back("context.keep_turns must be at least 1, got " + std::to_string(cx.keep_turns));
    if (cx.clm_max_edits < 0)
        p.push_back("context.clm_max_edits must be at least 0, got " + std::to_string(cx.clm_max_edits));
    for (size_t i = 0; i < cfg.hooks.hooks.size(); i++) {
        const auto & ev = cfg.hooks.hooks[i].event;
        if (ev != "pre-tool" && ev != "post-tool" && ev != "pre-edit" && ev != "post-edit" && ev != "turn-end" &&
            ev != "task-done")
            p.push_back("hooks.hooks[" + std::to_string(i) + "].event must be pre-tool, post-tool, pre-edit, " +
                        "post-edit, turn-end or task-done, got '" + ev + "'");
    }
    if (cfg.heartbeat.interval_s < 30)
        p.push_back("heartbeat.interval_s must be at least 30, got " + std::to_string(cfg.heartbeat.interval_s));
    for (size_t i = 0; i < cfg.heartbeat.collectors.size(); i++) {
        const auto & k = cfg.heartbeat.collectors[i].kind;
        if (k != "units" && k != "http" && k != "cpu" && k != "ram" && k != "gpu" && k != "disk" &&
            k != "dirgrowth" && k != "backups" && k != "command")
            p.push_back("heartbeat.collectors[" + std::to_string(i) + "].kind must be units, http, cpu, ram, gpu, " +
                        "disk, dirgrowth, backups or command, got '" + k + "'");
    }
    for (size_t i = 0; i < cfg.channels.channels.size(); i++) {
        const auto & k = cfg.channels.channels[i].kind;
        if (k != "ntfy" && k != "command" && k != "matrix")
            p.push_back("channels.channels[" + std::to_string(i) + "].kind must be ntfy, command or matrix, got '" + k + "'");
    }
    {
        const auto & mc = cfg.mcp;
        if (mc.max_tools_per_agent < 0) p.push_back("mcp.max_tools_per_agent must be 0 (no cap) or more");
        if (mc.max_result_chars < 200) p.push_back("mcp.max_result_chars must be at least 200");
        for (const auto & s : mc.servers) {
            std::string n = "mcp.servers." + s.name;
            if (s.command.empty()) p.push_back(n + ".command is not set");
            if (s.timeout_s < 1) p.push_back(n + ".timeout_s must be at least 1");
            if (s.startup_timeout_s < 1) p.push_back(n + ".startup_timeout_s must be at least 1");
            if (s.protocol != "auto" && s.protocol != "legacy" && s.protocol != "modern")
                p.push_back(n + ".protocol must be auto, legacy or modern, got '" + s.protocol + "'");
            for (char ch : s.name)
                if (!(std::isalnum((unsigned char) ch) || ch == '_' || ch == '-')) {
                    p.push_back(n + ": server names may only use letters, digits, _ and -");
                    break;
                }
        }
    }
    return p;
}

// ---------- text output ----------

namespace {

std::string q(const std::string & s) {
    std::ostringstream os;
    os << toml::value<std::string>(s);
    return os.str();
}

std::string key_name(const std::string & k) {
    bool bare = !k.empty();
    for (char ch : k) if (!(std::isalnum((unsigned char) ch) || ch == '_' || ch == '-')) bare = false;
    return bare ? k : q(k);
}

std::string num(float f) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.9g", (double) f);
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::string dnum(double d) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.15g", d);
    std::string s = buf;
    if (s.find_first_of(".eEn") == std::string::npos) s += ".0";
    return s;
}

std::string bias_str(float b) {
    if (std::isinf(b)) return b < 0 ? "\"-inf\"" : "\"inf\"";
    return num(b);
}

std::string str_array(const std::vector<std::string> & v) {
    std::string s = "[";
    for (size_t i = 0; i < v.size(); i++) s += (i ? ", " : "") + q(v[i]);
    return s + "]";
}

}  // namespace

std::string dump_config(const Config & c) {
    std::ostringstream o;
    o << "data_dir = " << q(c.data_dir) << "\n\n";
    o << "[engine]\n"
      << "model = " << q(c.engine.model) << "\n"
      << "max_agents = " << c.engine.max_agents << "\n"
      << "prefill_chunk = " << c.engine.prefill_chunk << "\n"
      << "prefill_duty = " << c.engine.prefill_duty << "\n"
      << "prefill_chunk_busy = " << c.engine.prefill_chunk_busy << "\n"
      << "prefill_max_wait_ms = " << c.engine.prefill_max_wait_ms << "\n"
      << "max_tokens = " << c.engine.max_tokens << "\n"
      << "park_dir = " << q(c.engine.park_dir) << "\n"
      << "prefix_cache = " << c.engine.prefix_cache << "\n"
      << "kv_reserve = " << c.engine.kv_reserve << "\n\n";
    for (auto & [name, m] : c.models) {
        std::string k = "models." + key_name(name);
        o << "[" << k << "]\n"
          << "path = " << q(m.path) << "\n"
          << "gpu_layers = " << m.gpu_layers << "\n"
          << "cpu_moe = " << (m.cpu_moe ? "true" : "false") << "\n"
          << "kv_type = " << q(m.kv_type) << "\n"
          << "kv_type_k = " << q(m.kv_type_k) << "\n"
          << "kv_type_v = " << q(m.kv_type_v) << "\n"
          << "ctx_total = " << m.ctx_total << "\n"
          << "ctx_per_agent = " << m.ctx_per_agent << "\n"
          << "ctx_per_agent_max = " << m.ctx_per_agent_max << "\n"
          << "threads = " << m.threads << "\n"
          << "ubatch = " << m.ubatch << "\n"
          << "batch = " << m.batch << "\n"
          << "flash_attn = " << (m.flash_attn ? "true" : "false") << "\n";
        if (!m.bias_file.empty()) o << "bias_file = " << q(m.bias_file) << "\n";
        if (!m.chat_template_file.empty()) o << "chat_template_file = " << q(m.chat_template_file) << "\n";
        if (!m.logit_bias.empty()) {
            o << "logit_bias = [\n";
            for (size_t i = 0; i < m.logit_bias.size(); i++) {
                if (i % 6 == 0) o << "    ";
                o << "[" << m.logit_bias[i].first << ", " << bias_str(m.logit_bias[i].second) << "],";
                o << (i % 6 == 5 || i + 1 == m.logit_bias.size() ? "\n" : " ");
            }
            o << "]\n";
        }
        o << "\n[" << k << ".sampling]\n"
          << "temp = " << num(m.temp) << "\n"
          << "top_p = " << num(m.top_p) << "\n"
          << "min_p = " << num(m.min_p) << "\n"
          << "top_k = " << m.top_k << "\n\n";
        if (!m.template_kwargs.empty()) {
            o << "[" << k << ".template_kwargs]\n";
            for (auto & [tk, tv] : m.template_kwargs) o << key_name(tk) << " = " << q(tv) << "\n";
            o << "\n";
        }
    }
    o << "[server]\n"
      << "bind = " << q(c.server.bind) << "\n"
      << "port = " << c.server.port << "\n"
      << "extra_binds = " << str_array(c.server.extra_binds) << "\n"
      << "allowed_hosts = " << str_array(c.server.allowed_hosts) << "\n"
      << "cors_origins = " << str_array(c.server.cors_origins) << "\n\n"
      << "[permissions]\n"
      << "mode = " << q(c.permissions.mode) << "\n"
      << "allow_paths = " << str_array(c.permissions.allow_paths) << "\n"
      << "deny_paths = " << str_array(c.permissions.deny_paths) << "\n"
      << "secret_paths = " << str_array(c.permissions.secret_paths) << "\n"
      << "deny_commands = " << str_array(c.permissions.deny_commands) << "\n"
      << "shell_timeout_s = " << c.permissions.shell_timeout_s << "\n"
      << "sandbox = " << q(c.permissions.sandbox) << "\n"
      << "sandbox_net = " << (c.permissions.sandbox_net ? "true" : "false") << "\n"
      << "allow_unsandboxed_mcp = " << (c.permissions.allow_unsandboxed_mcp ? "true" : "false") << "\n\n"
      << "[git]\n"
      << "commit_per_edit = " << (c.git.commit_per_edit ? "true" : "false") << "\n"
      << "branch_prefix = " << q(c.git.branch_prefix) << "\n\n"
      << "[skills]\n"
      << "enabled = " << (c.skills.enabled ? "true" : "false") << "\n"
      << "paths = " << str_array(c.skills.paths) << "\n"
      << "autoload = " << q(c.skills.autoload) << "\n"
      << "manage = " << (c.skills.manage ? "true" : "false") << "\n"
      << "track_usage = " << (c.skills.track_usage ? "true" : "false") << "\n\n"
      << "[learning]\n"
      << "review = " << (c.learning.review ? "true" : "false") << "\n"
      << "review_tool_steps = " << c.learning.review_tool_steps << "\n"
      << "review_memory_turns = " << c.learning.review_memory_turns << "\n"
      << "review_max_steps = " << c.learning.review_max_steps << "\n"
      << "curator = " << (c.learning.curator ? "true" : "false") << "\n"
      << "curator_interval_hours = " << c.learning.curator_interval_hours << "\n"
      << "curator_min_idle_minutes = " << c.learning.curator_min_idle_minutes << "\n"
      << "stale_after_days = " << c.learning.stale_after_days << "\n"
      << "archive_after_days = " << c.learning.archive_after_days << "\n"
      << "curator_backups = " << c.learning.curator_backups << "\n\n"
      << "[prompt]\n"
      << "max_memory_tokens = " << c.prompt.max_memory_tokens << "\n"
      << "tool_schema = " << q(c.prompt.tool_schema) << "\n\n"
      << "[memory]\n"
      << "path = " << q(c.memory.path) << "\n"
      << "inject_sections = " << str_array(c.memory.inject_sections) << "\n"
      << "reject_dashes = " << (c.memory.reject_dashes ? "true" : "false") << "\n\n"
      << "[hooks]\n"
      << "enabled = " << (c.hooks.enabled ? "true" : "false") << "\n\n"
      << "[search]\n"
      << "searxng = " << q(c.search.searxng) << "\n"
      << "fetch_command = " << q(c.search.fetch_command) << "\n\n"
      << "[agents]\n"
      << "max_depth = " << c.agents.max_depth << "\n"
      << "max_steps = " << c.agents.max_steps << "\n"
      << "max_auto_continues = " << c.agents.max_auto_continues << "\n"
      << "loop_repeats = " << c.agents.loop_repeats << "\n"
      << "loop_pause_after = " << c.agents.loop_pause_after << "\n"
      << "read_streak_nudge = " << c.agents.read_streak_nudge << "\n"
      << "read_streak_force = " << c.agents.read_streak_force << "\n"
      << "delegate_after_edits = " << c.agents.delegate_after_edits << "\n"
      << "role_dirs = " << str_array(c.agents.role_dirs) << "\n"
      << "auto_resume = " << (c.agents.auto_resume ? "true" : "false") << "\n"
      << "auto_resume_max = " << c.agents.auto_resume_max << "\n"
      << "auto_resume_delay_s = " << c.agents.auto_resume_delay_s << "\n\n"
      << "[context]\n"
      << "mode = " << q(c.context.mode) << "\n"
      << "trigger_pct = " << dnum(c.context.trigger_pct) << "\n"
      << "target_pct = " << dnum(c.context.target_pct) << "\n"
      << "keep_turns = " << c.context.keep_turns << "\n"
      << "recall = " << (c.context.recall ? "true" : "false") << "\n"
      << "clm_max_edits = " << c.context.clm_max_edits << "\n";
    for (auto & h : c.hooks.hooks) {
        o << "\n[[hooks.hooks]]\n"
          << "event = " << q(h.event) << "\n"
          << "command = " << q(h.command) << "\n"
          << "timeout_s = " << h.timeout_s << "\n"
          << "blocking = " << (h.blocking ? "true" : "false") << "\n"
          << "match = " << q(h.match) << "\n";
    }
    o << "\n[heartbeat]\n"
      << "enabled = " << (c.heartbeat.enabled ? "true" : "false") << "\n"
      << "interval_s = " << c.heartbeat.interval_s << "\n"
      << "endpoint = " << q(c.heartbeat.endpoint) << "\n"
      << "model = " << q(c.heartbeat.model) << "\n"
      << "checklist = " << q(c.heartbeat.checklist) << "\n"
      << "quiet_hours = " << q(c.heartbeat.quiet_hours) << "\n"
      << "channel = " << q(c.heartbeat.channel) << "\n";
    for (auto & col : c.heartbeat.collectors) {
        o << "\n[[heartbeat.collectors]]\n"
          << "name = " << q(col.name) << "\n"
          << "kind = " << q(col.kind) << "\n"
          << "arg = " << q(col.arg) << "\n";
    }
    o << "\n[channels]\n"
      << "def = " << q(c.channels.def) << "\n";
    for (auto & ch : c.channels.channels) {
        o << "\n[[channels.channels]]\n"
          << "name = " << q(ch.name) << "\n"
          << "kind = " << q(ch.kind) << "\n"
          << "target = " << q(ch.target) << "\n"
          << "enabled = " << (ch.enabled ? "true" : "false") << "\n";
    }
    o << "\n[mcp]\n"
      << "enabled = " << (c.mcp.enabled ? "true" : "false") << "\n"
      << "max_tools_per_agent = " << c.mcp.max_tools_per_agent << "\n"
      << "max_result_chars = " << c.mcp.max_result_chars << "\n";
    for (auto & s : c.mcp.servers) {
        std::string h = "\n[mcp.servers." + key_name(s.name) + "]\n";
        o << h
          << "command = " << str_array(s.command) << "\n"
          << "cwd = " << q(s.cwd) << "\n"
          << "enabled = " << (s.enabled ? "true" : "false") << "\n"
          << "timeout_s = " << s.timeout_s << "\n"
          << "startup_timeout_s = " << s.startup_timeout_s << "\n"
          << "tools_allow = " << str_array(s.tools_allow) << "\n"
          << "tools_deny = " << str_array(s.tools_deny) << "\n"
          << "prefix = " << q(s.prefix_set ? s.prefix : s.name + "__") << "\n"
          << "sandbox = " << (s.sandbox ? "true" : "false") << "\n"
          << "network = " << (s.network ? "true" : "false") << "\n"
          << "writable = " << str_array(s.writable) << "\n"
          << "trusted = " << (s.trusted ? "true" : "false") << "\n"
          << "protocol = " << q(s.protocol) << "\n";
        if (!s.env.empty()) {
            o << "\n[mcp.servers." << key_name(s.name) << ".env]\n";
            for (auto & [k, val] : s.env) o << key_name(k) << " = " << q(val) << "\n";
        }
    }
    return o.str();
}

std::string default_config_toml() {
    return R"TOML(# Judas config. Edit freely; every value shown here is the built-in default.
# Load order: built-in defaults, this file, ./.judas/config.toml in the project, then env
# (JUDAS_PORT, JUDAS_DATA_DIR). JUDAS_CONFIG=/path replaces this file.

# Where sessions.db, snapshots/ and logs/ live.
data_dir = "~/.local/share/judas"

[engine]
model = "main"             # which [models.x] is loaded
max_agents = 8             # live sequences, 1..8; more agents queue
prefill_chunk = 256        # tokens per prefill step
prefill_duty = 1.0         # fraction of busy decode steps that may carry a prefill chunk (0 < x <= 1)
prefill_chunk_busy = 0     # tokens per prefill chunk while others generate; 0 = same as prefill_chunk
prefill_max_wait_ms = 2000 # a foreground prefill waiting this long skips the duty pattern
max_tokens = 8192          # default generation cap per turn
park_dir = "/dev/shm/judas"
prefix_cache = 4           # RAM snapshots of shared prompt prefixes (LRU)
kv_reserve = 512           # KV tokens held back per live request for generation (admission control)

# ---- models ----------------------------------------------------------------
# bias_file format: "1892-inf,2218-inf,..." (token id, then a float or -inf).
# Entries are appended to logit_bias at load time. logit_bias itself can be an
# array of [id, bias] pairs, e.g. logit_bias = [[1892, "-inf"], [5, -1.5]].

[models.main]
path = "~/models/model.gguf"
gpu_layers = 99
cpu_moe = false            # keep expert tensors on the CPU
kv_type = "q8_0"           # f16 | q8_0 | q5_1 | q5_0 | q4_1 | q4_0 (q4_0 roughly doubles the pool vs q8_0)
# kv_type_k = "q8_0"       # optional: K and V cache types separately (K is the quality-sensitive one)
# kv_type_v = "q4_0"
ctx_total = 49152         # unified KV pool shared by all agents
ctx_per_agent = 24576      # soft cap for one agent's sequence (compaction trigger)
ctx_per_agent_max = 0      # 0 = off; lets one big prompt borrow free KV up to this hard cap
threads = 4
ubatch = 256
batch = 2048
flash_attn = true
# mmproj = ""              # vision projector GGUF; "" = auto (an mmproj*.gguf next to the model), "none" = off
# mmproj_gpu = false       # run the projector on the GPU (faster image encode, costs VRAM)
# image_max_tokens = 1024  # token cap per image
# bias_file = "~/.config/judas/bias.txt"   # optional token bans, format above

[models.main.sampling]
temp = 0.7
top_p = 0.95
min_p = 0.0
top_k = 20

# Add more [models.name] tables to switch between models (one is loaded at a time). A remote model sets
# url = "http://127.0.0.1:8081" (an OpenAI-compatible server) instead of path.
# vision = false           # remote only: send images to the upstream as image_url data URLs (it must accept them)

# ---- attachments -----------------------------------------------------------
# Files the user attaches in the chat bar (web and TUI). Documents become text; images go to the model when
# it has vision, otherwise they get OCR text (tesseract, when installed).
[attachments]
max_bytes = 52428800       # largest upload accepted (50 MiB); bigger uploads get HTTP 413
max_text_chars = 120000    # extracted text kept per attachment (head kept, the rest is cut)
pdf_max_pages = 6          # scanned PDFs (no text layer): pages rendered to images
ocr = true                 # run tesseract on images when it is installed

# ---- ui --------------------------------------------------------------------
[ui]
mascot = true              # pixel-art companions in the TUI and web UI

# ---- server ----------------------------------------------------------------
[server]
bind = "127.0.0.1"
port = 8079                # OpenAI-compatible /v1
extra_binds = []           # extra addresses; the API has no auth, so only a network you fully trust
allowed_hosts = []         # extra Host names accepted (localhost, binds and this hostname always are)
cors_origins = []          # web origins allowed to call the API from another page, e.g. "http://box:8010"

# ---- permissions -----------------------------------------------------------
[permissions]
mode = "auto-edit"         # suggest | auto-edit | full-auto
allow_paths = ["~"]        # writes outside these are refused
deny_paths = []            # always refused (globs, ~ expanded)
secret_paths = ["~/.ssh", "~/.gnupg"]  # never read or written, hidden in the sandbox (dirs, or globs with *)
deny_commands = ["sudo", "doas", "pkexec", "su ", "passwd", "faillock", "rm -rf /", "mkfs"]  # also hard-blocked in the shell tool
shell_timeout_s = 120
sandbox = "bwrap"          # none | bwrap (bubblewrap: root read-only, allow_paths read-write)
sandbox_net = true         # allow network inside the sandbox
allow_unsandboxed_mcp = false  # MCP servers whose sandbox is unavailable (bwrap missing, sandbox = "none")
                               # refuse to start unless this says yes

# ---- git -------------------------------------------------------------------
[git]
commit_per_edit = false    # inside a repo, commit each edited file on its own ref (your branch is never touched)
branch_prefix = "judas/"   # refs are refs/<prefix><session-short>, e.g. git log refs/judas/ab12cd34

# ---- skills ----------------------------------------------------------------
[skills]
enabled = true
paths = ["~/.config/judas/skills"]
autoload = "trigger"       # off | trigger | on
manage = true              # let the agent create and maintain skills with the skill_manage tool
track_usage = true         # record skill loads and patches in <data_dir>/skills.db

# ---- learning --------------------------------------------------------------
# After a lead turn finishes, a hidden review agent looks for lessons worth keeping. It can write memory
# and agent-authored skills only; user-owned and pinned skills are refused. Threshold 0 disables a nudge.
[learning]
review = true              # run a background review when a nudge counter reaches its threshold
review_tool_steps = 10     # tool-calling steps since the last review (skill nudge)
review_memory_turns = 10   # lead turns since the last review (memory nudge)
review_max_steps = 12      # model steps the review agent may take
# The curator keeps the agent-authored skill library healthy: unused skills go stale, long-unused ones move
# into .archive/ (never deleted), and every pass is preceded by a backup. It fires only between a week-minimum
# interval and only while no agent has been active for a while, so it never competes with your agents.
curator = true                  # false ends all automatic curation
curator_interval_hours = 168    # min hours between automatic runs
curator_min_idle_minutes = 30   # engine idle for this long before a run may start
stale_after_days = 30           # days unused before an agent skill is marked stale
archive_after_days = 90         # days unused before it moves into .archive/
curator_backups = 5             # newest skill backups kept in <data_dir>/skill-backups/

# ---- prompt ----------------------------------------------------------------
[prompt]
max_memory_tokens = 3000   # cap on an appended memory block
tool_schema = "full"       # full | lazy

# ---- memory ----------------------------------------------------------------
# One Markdown file with "## " sections (other assistants may share it). The listed sections go in the system
# prompt (capped by prompt.max_memory_tokens); the memory_read / memory_search / memory_append tools reach the rest.
[memory]
path = "~/.local/share/judas/memory.md"
inject_sections = ["About me", "Rules"]
reject_dashes = false      # memory_append refuses facts containing em or en dashes

# ---- hooks -----------------------------------------------------------------
# events: pre-tool | post-tool | pre-edit | post-edit | turn-end | task-done
[hooks]
enabled = false
# [[hooks.hooks]]
# event = "post-edit"
# command = "echo edited"
# timeout_s = 30
# blocking = false         # if true and it exits non-zero on a pre-* event, the action is refused
# match = ""               # only fire when the tool name or path contains this

# ---- search ----------------------------------------------------------------
[search]
searxng = "http://127.0.0.1:8888"
fetch_command = "webfetch"

# ---- agents ----------------------------------------------------------------
[agents]
max_depth = 2              # lead -> worker -> helper
max_steps = 100            # model steps per agent turn; with open plan items the turn pauses here (resumable)
max_auto_continues = 8     # harness nudges per turn when a reply has no tool call and is not a final answer
loop_repeats = 3           # the same tool call or error this many times = looping (warn, then pause)
loop_pause_after = 2       # loop warnings in a short time before the turn pauses
read_streak_nudge = 8     # code/cowork: read-only calls in a row before a "write now" message (0 = off)
read_streak_force = 14    # read-only calls in a row after which reads are refused until a write or build/test (0 = off)
delegate_after_edits = 8   # lead: edits in a row with 2+ open plan items before a delegate reminder (0 = off)
role_dirs = ["~/.config/judas/roles"]
auto_resume = false        # resume paused lead turns by itself (a session can override it)
auto_resume_max = 20       # auto-resumes per session per day
auto_resume_delay_s = 30   # wait this long after a pause before resuming

# ---- context ---------------------------------------------------------------
# What to do as an agent's conversation nears its context cap.
# mode: append (never compact) | compact (digest old tool output, summarize last) | clm (model edits its own context)
[context]
mode = "compact"
trigger_pct = 0.75         # compact when ctx_used / ctx_per_agent crosses this
target_pct = 0.45          # compact down to about this
keep_turns = 3             # last N turns always kept verbatim
recall = true              # compacted tool output stays fetchable via the recall tool
clm_max_edits = 4          # clm: max self-edits per turn

# ---- heartbeat -------------------------------------------------------------
# Every interval_s, read-only collectors gather facts, a model summarizes and flags
# anything wrong, and an alert is pushed to a channel only when something needs attention.
# collector kinds: units | http | cpu | ram | gpu | disk | dirgrowth | backups | command
[heartbeat]
enabled = false
interval_s = 900           # at least 30
endpoint = "local"         # "local" (this daemon) or an OpenAI-compatible base URL
model = ""                 # model name to send to a remote endpoint
checklist = ""             # optional Markdown file of what to watch
quiet_hours = ""           # e.g. "23:00-07:00": no pushes then unless critical
channel = ""               # channel name for alerts (else channels.def)
# [[heartbeat.collectors]]
# name = "root disk"
# kind = "disk"
# arg = "/"                # kind-specific: url, path, command line or unit glob

# ---- channels --------------------------------------------------------------
# channel kinds: ntfy (target = topic URL) | command (target = command, message on stdin)
[channels]
def = ""                   # default channel name for heartbeat alerts
# [[channels.channels]]
# name = "phone"
# kind = "ntfy"
# target = "https://ntfy.sh/my-judas-topic"
# enabled = true

# ---- mcp -------------------------------------------------------------------
# Tools from MCP servers (stdio), registered as "<prefix><tool>". Third-party servers are untrusted: when
# permissions.sandbox = "bwrap" they run under bwrap (read-only root, hidden secret dirs), and each one gets
# no network unless its own network = true. When the sandbox is unavailable a server refuses to start;
# permissions.allow_unsandboxed_mcp = true lets it run unconfined.
# See docs/MCP.md for the safety model and role filters (tools: mcp:*, mcp:<server>, !mcp:*).
[mcp]
enabled = true
max_tools_per_agent = 12   # MCP tools one agent sees (0 = no cap); small local models degrade with many tools
max_result_chars = 30000   # a longer tool result keeps its head and tail

# [mcp.servers.docs]       # Microsoft markitdown-mcp: PDF, DOCX, XLSX, PPTX, HTML to Markdown (uv tool install markitdown-mcp)
# command = ["~/.local/bin/markitdown-mcp"]
# enabled = true
# timeout_s = 120          # per tool call
# startup_timeout_s = 30   # spawn + handshake + first tools/list
# tools_allow = []         # globs on the server's own tool names (empty = all)
# tools_deny = []          # globs; win over tools_allow
# prefix = "docs__"        # default is "<name>__"
# sandbox = true           # false runs the server directly (only for servers you trust)
# network = false          # network inside its sandbox; per server, keep off unless the tool needs it
# writable = []            # paths the server may write; everything else is read-only
# trusted = false          # true skips the approval prompt for tools that can change things
# protocol = "auto"        # auto | legacy (initialize handshake) | modern (2026-07-28)
# env = { SOME_KEY = "${SOME_KEY}" }
)TOML";
}

// The config file the model switch persists to (JUDAS_CONFIG wins, else the user config).
std::string user_config_path() {
    if (const char * e = std::getenv("JUDAS_CONFIG")) return e;
    return expand_home("~/.config/judas/config.toml");
}

// Surgically set `model = "<name>"` inside [engine], preserving the rest of the file (comments and all).
// Inserts the line right after [engine] if it is missing. Returns false if the file cannot be read/written.
bool set_engine_model_in_file(const std::string & path_in, const std::string & model) {
    // Follow a symlinked config to its target; a missing file is created with just [engine].
    std::error_code ec;
    std::string path = path_in;
    if (fs::is_symlink(path_in, ec)) {
        auto t = fs::canonical(path_in, ec);
        if (ec) return false;
        path = t.string();
    }
    std::vector<std::string> lines;
    if (fs::exists(path, ec)) {
        std::ifstream in(path);
        if (!in) return false;
        std::string line;
        while (std::getline(in, line)) lines.push_back(line);
        if (in.bad()) return false;
    }
    const std::string newline = "model = \"" + model + "\"";
    bool in_engine = false, done = false;
    int engine_hdr = -1;
    for (size_t i = 0; i < lines.size() && !done; i++) {
        size_t a = lines[i].find_first_not_of(" \t");
        std::string s = a == std::string::npos ? "" : lines[i].substr(a);
        if (!s.empty() && s[0] == '[') {
            if (in_engine) break;  // left [engine] without a model line; insert below
            in_engine = s.rfind("[engine]", 0) == 0;
            if (in_engine) engine_hdr = static_cast<int>(i);
            continue;
        }
        size_t eq = s.find('=');
        std::string key = eq == std::string::npos ? "" : s.substr(0, eq);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) key.pop_back();
        if (in_engine && key == "model") {
            std::string comment;
            size_t h = lines[i].find('#');
            if (h != std::string::npos) comment = "            " + lines[i].substr(h);
            lines[i] = newline + comment;
            done = true;
        }
    }
    if (!done) {
        if (engine_hdr < 0) {
            lines.push_back("[engine]");
            lines.push_back(newline);
        } else {
            lines.insert(lines.begin() + engine_hdr + 1, newline);
        }
    }
    // Write a temp file next to it and rename, so a crash or full disk never leaves a truncated config.
    fs::create_directories(fs::path(path).parent_path(), ec);
    std::string tmp = path + ".tmp." + std::to_string(::getpid());
    {
        std::ofstream out(tmp, std::ios::trunc);
        if (!out) return false;
        for (auto & l : lines) out << l << "\n";
        out.flush();
        if (!out) {
            fs::remove(tmp, ec);
            return false;
        }
    }
    if (fs::exists(path, ec)) fs::permissions(tmp, fs::status(path, ec).permissions(), ec);
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(tmp, ec);
        return false;
    }
    return true;
}

}  // namespace judas
