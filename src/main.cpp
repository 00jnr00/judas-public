// judas: the betrayer harness. One binary: daemon (serve), one-shot agent runs, snapshots, config.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <regex>
#include <map>
#include <chrono>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "agent/agent.h"
#include "core/config.h"
#include "cpp-httplib/httplib.h"
#include "engine/engine.h"
#include "learning/curator.h"
#include "models/models.h"
#include "server/server.h"
#include "tui/tui.h"
#include "skills/skills.h"
#include "skills/install.h"
#include "tools/memory_tools.h"
#include "tools/tools.h"
#include "snap/journal.h"
#include "store/store.h"

namespace judas {
const char * version();
}

using namespace judas;
namespace fs = std::filesystem;

namespace {

const char * USAGE = R"(judas, the betrayer harness

usage:
  judas serve [--mode M]              run the daemon: model, agents, OpenAI-compatible /v1 and the Judas API
  judas [chat] [options]              terminal UI (a client of the daemon; see docs/TUI.md)
      --session ID    open that session (id or unique prefix)
      --continue, -c  open the most recent session
      --cwd DIR       working directory for tools (default: current)
      --local         if no daemon is up, run one inside this process on a private port
  judas run "task" [options]          run a task with a lead agent (uses the daemon if it is up, else in-process)
      --cwd DIR       working directory for tools (default: current)
      --session ID    continue a session
      --local         never use the daemon
      --yes           permission mode full-auto for this run (local only)
      --think         show the model's thinking
  judas state                         daemon state (model, agents, VRAM)
  judas sessions                      recent sessions
  judas snap list [--session ID]      snapshot checkpoints, newest first
  judas snap diff ID                  what a checkpoint changed
  judas snap revert ID [--to]         undo one checkpoint (or it and everything after it)
  judas curator status | run [--dry-run]
                                      skill library curator: state, or run a pass (dry = report only)
  judas config show|validate|init     effective config, check it, or write ~/.config/judas/config.toml
  judas models [list] [--all] [--dir D]
                                      GGUF models on this machine (your dirs, LM Studio, Hugging Face cache,
                                      Ollama), with size, quant, context and whether they fit your GPU
  judas models info N|PATH            details for one model (N = number from the list)
  judas models add N|PATH [--name X] [--use]
                                      write a [models.X] block with defaults that suit the file and GPU
  judas models use NAME               make [models.NAME] the one the daemon loads
  judas models search WORDS           find GGUF repos on Hugging Face (most downloaded first)
  judas models files REPO             the GGUF files in a repo, with sizes and fit
  judas models pull REPO [FILE|QUANT] [--dir D] [--add] [--use] [--name X]
                                      download (resumable; HF_TOKEN for gated repos); no FILE picks the
                                      biggest quant that fits your GPU
  judas prompt-size                   estimated prompt budget: tool schemas and skills index
  judas version
)";

// Silver for Judas, blood red for errors (art direction suggestion, see DESIGN.md section 12).
const char * C_SILVER = "\033[38;2;192;192;200m";
const char * C_BRIGHT = "\033[1;38;2;232;232;240m";
const char * C_DIM = "\033[2;38;2;150;150;160m";
const char * C_RED = "\033[38;2;178;34;34m";
const char * C_RESET = "\033[0m";

std::string arg_value(std::vector<std::string> & args, const std::string & flag, const std::string & def = "") {
    for (size_t i = 0; i + 1 < args.size(); i++)
        if (args[i] == flag) {
            std::string v = args[i + 1];
            args.erase(args.begin() + i, args.begin() + i + 2);
            return v;
        }
    return def;
}

std::string lower_copy(std::string s) {
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

bool arg_flag(std::vector<std::string> & args, const std::string & flag) {
    for (size_t i = 0; i < args.size(); i++)
        if (args[i] == flag) {
            args.erase(args.begin() + i);
            return true;
        }
    return false;
}

Config load_or_die() {
    try {
        return load_config(".");
    } catch (const std::exception & e) {
        std::fprintf(stderr, "%sconfig error:%s %s\n", C_RED, C_RESET, e.what());
        std::exit(2);
    }
}

std::string data_path(const Config & cfg, const std::string & sub) {
    fs::path d = expand_home(cfg.data_dir);
    fs::create_directories(d);
    return (d / sub).string();
}

// Prints agent events: the lead's answer streams, everyone else gets one status line per action.
struct Printer {
    bool think = false;
    std::mutex mu;
    std::string streaming;  // agent currently streaming text

    void operator()(const AgentEvent & ev) {
        std::lock_guard lk(mu);
        bool lead = ev.parent.empty();
        std::string who = ev.role + " " + ev.agent.substr(ev.agent.size() > 4 ? ev.agent.size() - 4 : 0);
        auto line = [&](const std::string & s) {
            if (!streaming.empty()) {
                std::printf("%s\n", C_RESET);
                streaming.clear();
            }
            std::printf("%s", s.c_str());
            std::fflush(stdout);
        };
        if (ev.type == "delta" && lead) {
            if (streaming != ev.agent) {
                if (!streaming.empty()) std::printf("%s\n", C_RESET);
                streaming = ev.agent;
                std::printf("%s", C_BRIGHT);  // one color switch per stream, not per token
            }
            std::printf("%s", ev.data.value("text", "").c_str());
            std::fflush(stdout);
        } else if (ev.type == "done" && lead && !streaming.empty()) {
            std::printf("%s\n", C_RESET);
            streaming.clear();
        } else if (ev.type == "reasoning" && think) {
            std::printf("%s%s%s", C_DIM, ev.data.value("text", "").c_str(), C_RESET);
            std::fflush(stdout);
        } else if (ev.type == "spawned" && !lead) {
            line(std::string(C_SILVER) + "+ " + who + ": " + ev.data.value("task", "").substr(0, 100) + C_RESET + "\n");
        } else if (ev.type == "tool_call") {
            std::string args = ev.data.contains("args") ? ev.data["args"].dump() : "";
            if (args.size() > 120) args = args.substr(0, 117) + "...";
            line(std::string(C_DIM) + "  [" + who + "] " + ev.data.value("name", "") + " " + args + C_RESET + "\n");
        } else if (ev.type == "tool_result" && !ev.data.value("ok", true)) {
            line(std::string(C_RED) + "  [" + who + "] " + ev.data.value("name", "") + " failed: " +
                 ev.data.value("output", "").substr(0, 200) + C_RESET + "\n");
        } else if (ev.type == "done" && !lead) {
            line(std::string(C_SILVER) + "- " + who + " done" + C_RESET + "\n");
        } else if (ev.type == "status" && ev.data.contains("error")) {
            line(std::string(C_RED) + "  [" + who + "] " + ev.data.value("error", "") + C_RESET + "\n");
        }
    }
};

bool daemon_up(const Config & cfg) {
    httplib::Client c(cfg.server.bind == "0.0.0.0" ? "127.0.0.1" : cfg.server.bind, cfg.server.port);
    c.set_connection_timeout(0, 300000);
    auto r = c.Get("/health");
    return r && r->status == 200;
}

int cmd_serve(std::vector<std::string> args) {
    Config cfg = load_or_die();
    std::string mode = arg_value(args, "--mode");
    if (!mode.empty()) cfg.permissions.mode = mode;
    for (const auto & p : validate_config(cfg)) std::fprintf(stderr, "config: %s\n", p.c_str());
    // VRAM guard: a big model can fill most of the GPU, and loading it while another model (or a game) is
    // resident can push the driver into a GPU reset that takes the desktop with it. Wait up to ~25 s for VRAM to
    // drain (covers a self-restart), then refuse if it is still high, unless forced. CPU-only models
    // (gpu_layers 0) do not touch VRAM, so they are exempt. Remote models load nothing here. A local GPU model first stops any remote model's unit so its
    // VRAM drains before the guard below measures it.
    if (cfg.active_model().url.empty())
        for (const auto & [k, m] : cfg.models)
            if (!m.unit.empty()) std::system(("systemctl --user stop " + m.unit + " 2>/dev/null").c_str());
    bool cpu_only = cfg.active_model().gpu_layers == 0 || !cfg.active_model().url.empty();
    if (!cpu_only && !std::getenv("JUDAS_FORCE")) {
        auto vram_mib = [] {
            // amdgpu exposes this per card; take the busiest one (other drivers: no reading, no guard)
            long best = 0;
            for (int card = 0; card < 8; card++) {
                std::ifstream vf("/sys/class/drm/card" + std::to_string(card) + "/device/mem_info_vram_used");
                long used = 0;
                if (vf >> used) best = std::max(best, used / 1048576);
            }
            return best;
        };
        long mib = vram_mib();
        for (int i = 0; i < 25 && mib > 5000; i++) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            mib = vram_mib();
        }
        if (mib > 5000) {
            std::fprintf(stderr,
                         "%sjudas: %ld MiB of GPU VRAM is still in use after waiting.%s Another model or a game is "
                         "using the GPU. Loading on top of it can reset the GPU and crash the desktop.\n"
                         "Stop it first, or set JUDAS_FORCE=1 to override.\n",
                         C_RED, mib, C_RESET);
            return 1;
        }
    }
    std::fprintf(stderr, "judas %s: loading %s\n", version(), cfg.engine.model.c_str());
    auto engine = Engine::create(cfg);
    Store store(data_path(cfg, "sessions.db"));
    if (int n = store.interrupt_orphans()) std::fprintf(stderr, "judas: marked %d agent(s) from the last run as interrupted\n", n);
    Journal journal(data_path(cfg, "snapshots"));
    AgentManager agents(cfg, *engine, store, journal);
    agents.set_approver([](const std::string &, const std::string & what) {
        std::fprintf(stderr, "denied (no approver in the daemon yet; use full-auto or judas run --local): %s\n",
                     what.c_str());
        return false;
    });
    Server server(cfg, *engine, agents, store, journal);
    std::fprintf(stderr, "judas: serving on %s:%d (mode %s)\n", cfg.server.bind.c_str(), cfg.server.port,
                 cfg.permissions.mode.c_str());
    return server.run() ? 0 : 1;
}

int run_local(Config & cfg, const std::string & task, const std::string & cwd, const std::string & session, bool think) {
    std::fprintf(stderr, "%sjudas: loading %s in-process...%s\n", C_DIM, cfg.engine.model.c_str(), C_RESET);
    auto engine = Engine::create(cfg);
    Store store(data_path(cfg, "sessions.db"));
    Journal journal(data_path(cfg, "snapshots"));
    AgentManager agents(cfg, *engine, store, journal);
    Printer printer;
    printer.think = think;
    agents.add_listener([&](const AgentEvent & ev) { printer(ev); });
    agents.set_approver([&](const std::string &, const std::string & what) {
        std::lock_guard lk(printer.mu);
        std::fprintf(stderr, "\n%sapprove?%s %s [y/N] ", C_RED, C_RESET, what.c_str());
        std::string ans;
        if (!isatty(0) || !std::getline(std::cin, ans)) return false;
        return ans == "y" || ans == "Y" || ans == "yes";
    });
    std::string sid = agents.submit(session, task, cwd);
    agents.wait_session(sid);
    std::printf("\n%ssession %s%s\n", C_DIM, sid.c_str(), C_RESET);
    return 0;
}

int run_remote(const Config & cfg, const std::string & task, const std::string & cwd, const std::string & session,
               bool think) {
    httplib::Client c("127.0.0.1", cfg.server.port);
    c.set_read_timeout(3600, 0);
    json body = {{"content", task}, {"cwd", cwd}};
    auto r = c.Post("/api/sessions/" + (session.empty() ? std::string("new") : session) + "/messages", body.dump(),
                    "application/json");
    if (!r || r->status != 200) {
        std::fprintf(stderr, "%sdaemon refused the task:%s %s\n", C_RED, C_RESET, r ? r->body.c_str() : "no response");
        return 1;
    }
    std::string sid = json::parse(r->body).value("session", "");
    Printer printer;
    printer.think = think;
    std::string buf;
    c.Get("/api/sessions/" + sid + "/events", [&](const char * data, size_t n) {
        buf.append(data, n);
        for (size_t p; (p = buf.find("\n\n")) != std::string::npos;) {
            std::string ev = buf.substr(0, p);
            buf.erase(0, p + 2);
            if (ev.rfind("data: ", 0) != 0) continue;
            try {
                json j = json::parse(ev.substr(6));
                AgentEvent e{j.value("type", ""), j.value("session", ""), j.value("agent", ""),
                             j.value("parent", ""), j.value("role", ""), j.value("data", json::object())};
                printer(e);
            } catch (...) {
            }
        }
        return true;
    });
    std::printf("\n%ssession %s%s\n", C_DIM, sid.c_str(), C_RESET);
    return 0;
}

int cmd_run(std::vector<std::string> args) {
    Config cfg = load_or_die();
    std::string cwd = arg_value(args, "--cwd", fs::current_path().string());
    std::string session = arg_value(args, "--session");
    bool local = arg_flag(args, "--local"), yes = arg_flag(args, "--yes"), think = arg_flag(args, "--think");
    std::string task;
    for (auto & a : args) task += (task.empty() ? "" : " ") + a;
    if (task.empty()) {
        std::fprintf(stderr, "judas run: give a task\n");
        return 2;
    }
    cwd = fs::weakly_canonical(expand_home(cwd)).string();
    if (yes) cfg.permissions.mode = "full-auto";
    if (!local && !yes && daemon_up(cfg)) return run_remote(cfg, task, cwd, session, think);
    return run_local(cfg, task, cwd, session, think);
}

// The TUI. With --local and no daemon up, an in-process daemon (same code as `judas serve`) runs on a private
// port for the life of the TUI, like `judas run --local`.
int cmd_tui(std::vector<std::string> args) {
    Config cfg = load_or_die();
    TuiOptions o;
    o.session = arg_value(args, "--session");
    o.resume_last = arg_flag(args, "--continue") || arg_flag(args, "-c");
    o.cwd = arg_value(args, "--cwd");
    if (!o.cwd.empty()) o.cwd = fs::weakly_canonical(expand_home(o.cwd)).string();
    bool local = arg_flag(args, "--local");
    if (!local || daemon_up(cfg)) return run_tui(cfg, o);

    cfg.server.bind = "127.0.0.1";
    cfg.server.extra_binds.clear();
    cfg.server.port = 20000 + (int) (std::chrono::steady_clock::now().time_since_epoch().count() % 30000);
    std::fprintf(stderr, "%sjudas: no daemon; loading %s in-process on 127.0.0.1:%d...%s\n", C_DIM, cfg.engine.model.c_str(),
                 cfg.server.port, C_RESET);
    auto engine = Engine::create(cfg);
    Store store(data_path(cfg, "sessions.db"));
    store.interrupt_orphans();
    Journal journal(data_path(cfg, "snapshots"));
    AgentManager agents(cfg, *engine, store, journal);
    agents.set_approver([](const std::string &, const std::string &) { return false; });
    Server server(cfg, *engine, agents, store, journal);
    std::thread srv([&] { server.run(); });
    for (int i = 0; i < 100 && !daemon_up(cfg); i++) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int rc = run_tui(cfg, o);
    server.stop();
    srv.join();
    return rc;
}

int cmd_get(const std::string & path) {
    Config cfg = load_or_die();
    httplib::Client c("127.0.0.1", cfg.server.port);
    auto r = c.Get(path);
    if (!r) {
        std::fprintf(stderr, "judas daemon is not running on port %d\n", cfg.server.port);
        return 1;
    }
    try {
        std::printf("%s\n", json::parse(r->body).dump(2).c_str());
    } catch (...) {
        std::printf("%s\n", r->body.c_str());
    }
    return r->status == 200 ? 0 : 1;
}

int cmd_snap(std::vector<std::string> args) {
    Config cfg = load_or_die();
    Journal journal(data_path(cfg, "snapshots"));
    std::string sub = args.empty() ? "list" : args[0];
    if (!args.empty()) args.erase(args.begin());
    if (sub == "list") {
        std::string session = arg_value(args, "--session");
        for (const auto & c : journal.list(session, 50)) {
            std::printf("%s  %s%-40s%s  %zu file(s)%s%s\n", c.id.c_str(), C_SILVER, c.label.substr(0, 40).c_str(),
                        C_RESET, c.files.size(), c.irreversible.empty() ? "" : "  +side effects",
                        c.reverted ? "  (reverted)" : "");
        }
        return 0;
    }
    if (args.empty()) {
        std::fprintf(stderr, "judas snap %s: give a checkpoint id\n", sub.c_str());
        return 2;
    }
    std::string id = args[0];
    if (sub == "diff") {
        std::printf("%s", journal.diff(id).c_str());
        return 0;
    }
    if (sub == "revert") {
        bool to = arg_flag(args, "--to");
        auto r = to ? journal.revert_to(id) : journal.revert(id);
        for (auto & p : r.restored) std::printf("restored %s\n", p.c_str());
        for (auto & p : r.removed) std::printf("removed  %s\n", p.c_str());
        for (auto & p : r.conflicts) std::printf("%sconflict%s %s (changed since; restored anyway)\n", C_RED, C_RESET, p.c_str());
        for (auto & p : r.irreversible) std::printf("not undone: %s\n", p.c_str());
        if (!r.ok) std::fprintf(stderr, "%s%s%s\n", C_RED, r.error.c_str(), C_RESET);
        return r.ok ? 0 : 1;
    }
    std::fprintf(stderr, "judas snap: unknown subcommand %s\n", sub.c_str());
    return 2;
}

// Curator: `judas curator status` shows the schedule state; `run` performs one pass now (dry = report only;
// the scheduler, by contrast, waits for the interval and an idle engine).
int cmd_curator(std::vector<std::string> args) {
    std::string sub = args.empty() ? "status" : args[0];
    if (!args.empty()) args.erase(args.begin());
    if (sub != "status" && sub != "run") {
        std::fprintf(stderr, "judas curator: use status or run [--dry-run]\n");
        return 2;
    }
    bool dry = arg_flag(args, "--dry-run");
    Config cfg = load_or_die();
    std::string data = expand_home(cfg.data_dir);
    Skills skills(cfg.skills);
    SkillUsage usage((fs::path(data) / "skills.db").string());
    if (sub == "status") {
        auto st = read_curator_state(data);
        if (!st) {
            std::printf("never ran; the first pass (daemon or `judas curator run`) seeds the schedule\n");
            return 0;
        }
        std::time_t t = (std::time_t)(st->last_run_ms / 1000);
        char stamp[64];
        std::strftime(stamp, sizeof stamp, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
        std::printf("last pass   : %s (run_count %d)\nsummary     : %s\nauto cadence: every %d h, only when no agent has been active for %d min\n",
                    stamp, st->run_count, st->summary.c_str(), cfg.learning.curator_interval_hours,
                    cfg.learning.curator_min_idle_minutes);
        return 0;
    }
    auto r = run_curator(cfg.learning, skills.all(), data, skills.write_dir(), usage, now_ms(), dry);
    if (!r.error.empty()) {
        std::fprintf(stderr, "%scurator error:%s %s\n", C_RED, C_RESET, r.error.c_str());
        return 1;
    }
    if (r.first_run && !dry)
        std::printf("schedule seeded; the first curation pass lands after one full interval (%d h)\n",
                    cfg.learning.curator_interval_hours);
    std::printf("%s\n", r.summary.c_str());
    if (!r.report_path.empty()) std::printf("report: %s\n", r.report_path.c_str());
    return 0;
}

int cmd_config(std::vector<std::string> args) {
    std::string sub = args.empty() ? "show" : args[0];
    if (sub == "init") {
        fs::path p = expand_home("~/.config/judas/config.toml");
        if (fs::exists(p)) {
            std::fprintf(stderr, "%s exists; not overwriting\n", p.c_str());
            return 1;
        }
        fs::create_directories(p.parent_path());
        FILE * f = std::fopen(p.c_str(), "w");
        std::fputs(default_config_toml().c_str(), f);
        std::fclose(f);
        std::printf("wrote %s\n", p.c_str());
        return 0;
    }
    Config cfg = load_or_die();
    if (sub == "validate") {
        auto problems = validate_config(cfg);
        for (auto & p : problems) std::printf("%s\n", p.c_str());
        if (problems.empty()) std::printf("ok\n");
        return problems.empty() ? 0 : 1;
    }
    for (auto & s : cfg.sources) std::printf("# loaded %s\n", s.c_str());
    std::printf("%s", dump_config(cfg).c_str());
    return 0;
}

int cmd_skills(std::vector<std::string> args) {
    std::string sub = args.empty() ? "" : args[0];
    if (sub != "install") {
        std::fprintf(stderr, "usage: judas skills install <path-to-skill.md> [--force]\n");
        return 2;
    }
    args.erase(args.begin());
    bool force = arg_flag(args, "--force");
    if (args.empty()) {
        std::fprintf(stderr, "judas skills install: give a path to a skill .md file\n");
        return 2;
    }
    Config cfg = load_or_die();
    std::string dir = Skills(cfg.skills).write_dir();
    if (dir.empty()) {
        std::fprintf(stderr, "judas skills install: no skills path configured\n");
        return 2;
    }
    InstallResult r = install_skill(args[0], dir, force);
    if (!r.report.empty()) std::printf("%s", r.report.c_str());
    if (r.installed) std::printf("installed: %s\n", r.dest.c_str());
    else if (r.quarantined) std::printf("quarantined: %s (blocked by the guard; review it, or reinstall with --force)\n", r.dest.c_str());
    else std::printf("not installed\n");
    return r.installed ? 0 : 1;
}


// ---- judas models --------------------------------------------------------------------------------------------

void print_vram(uint64_t vram) {
    if (vram) std::printf("%sGPU memory: %s%s\n", C_DIM, human_bytes(vram).c_str(), C_RESET);
    else std::printf("%sGPU memory: unknown (fit column is a guess)%s\n", C_DIM, C_RESET);
}

const char * fit_color(const std::string & fit) {
    if (!*C_RESET) return "";
    return fit == "fits" ? "\033[38;2;127;176;138m" : fit == "tight" ? "\033[38;2;208;168;92m" : fit == "too big" ? C_RED : C_DIM;
}

// "3" (from the list) or a path to a .gguf
bool resolve_model(const Config & cfg, const std::string & ref, GgufInfo & out) {
    bool num = !ref.empty() && ref.find_first_not_of("0123456789") == std::string::npos;
    if (num) {
        auto all = scan_models(cfg);
        std::vector<GgufInfo> shown;
        for (auto & m : all) if (!m.projector) shown.push_back(m);
        size_t i = std::stoul(ref);
        if (i < 1 || i > shown.size()) {
            std::fprintf(stderr, "no model #%s; run `judas models` for the list\n", ref.c_str());
            return false;
        }
        out = shown[i - 1];
        return true;
    }
    std::string p = expand_home(ref);
    if (!fs::exists(p)) {
        std::fprintf(stderr, "not found: %s\n", p.c_str());
        return false;
    }
    out = gguf_info(fs::absolute(p).string());
    return true;
}

int models_list(const Config & cfg, std::vector<std::string> & args) {
    bool all = arg_flag(args, "--all");
    std::vector<std::string> dirs;
    for (std::string d; !(d = arg_value(args, "--dir")).empty();) dirs.push_back(d);
    uint64_t vram = gpu_vram_total();
    auto found = scan_models(cfg, dirs);
    print_vram(vram);
    if (found.empty()) {
        std::printf("\nNo GGUF models found. Looked in:\n");
        for (auto & [d, src] : model_search_dirs(cfg)) std::printf("  %s\n", d.c_str());
        std::printf("\nFind one:      judas models search qwen3 coder\n"
                    "Download one:  judas models pull <repo> --add --use\n"
                    "Elsewhere:     judas models --dir /path/to/models  (or JUDAS_MODEL_DIRS=/a:/b)\n");
        return 0;
    }
    std::printf("\n %3s  %-14s %-46s %9s  %-9s %7s  %-8s %s\n", "#", "CONFIG", "MODEL", "SIZE", "QUANT", "CTX", "FIT", "FROM");
    int n = 0, hidden = 0;
    for (auto & m : found) {
        if (m.projector) {
            hidden++;
            if (!all) continue;
        }
        std::string label = m.label;
        if (m.parts > 1) label += " (" + std::to_string(m.parts) + " parts)";
        if (label.size() > 46) label = label.substr(0, 43) + "...";
        std::string ctx = m.ctx_train ? std::to_string(m.ctx_train / 1024) + "k" : "-";
        std::string fit = m.projector ? "mmproj" : m.ok ? fit_verdict(m.bytes, vram) : "error";
        bool active = !m.config_name.empty() && m.config_name == cfg.engine.model;
        std::printf(" %3s  %s%-14s%s %-46s %9s  %-9s %7s  %s%-8s%s %s%s%s\n",
                    m.projector ? "" : std::to_string(++n).c_str(), active ? C_BRIGHT : C_SILVER,
                    (m.config_name.empty() ? "-" : m.config_name + (active ? " *" : "")).c_str(), C_RESET,
                    label.c_str(), m.bytes ? human_bytes(m.bytes).c_str() : "-", m.quant.empty() ? "-" : m.quant.c_str(),
                    ctx.c_str(), fit_color(fit), fit.c_str(), C_RESET, C_DIM, m.source.c_str(), C_RESET);
        if (!m.error.empty()) std::printf("      %s%s%s\n", C_RED, m.error.c_str(), C_RESET);
    }
    std::printf("\n%s* = loaded by the daemon (engine.model)", C_DIM);
    if (hidden && !all) std::printf("; %d vision projector file(s) hidden, --all shows them", hidden);
    std::printf("%s\n", C_RESET);
    std::printf("Connect one:  judas models add <#> --use        Details: judas models info <#>\n");
    return 0;
}

int models_info(const Config & cfg, std::vector<std::string> & args) {
    if (args.empty()) {
        std::fprintf(stderr, "usage: judas models info N|PATH\n");
        return 2;
    }
    GgufInfo m;
    if (!resolve_model(cfg, args[0], m)) return 1;
    uint64_t vram = gpu_vram_total();
    std::printf("%s%s%s\n", C_BRIGHT, m.label.c_str(), C_RESET);
    auto row = [](const char * k, const std::string & v) { if (!v.empty()) std::printf("  %-13s %s\n", k, v.c_str()); };
    row("path", m.path);
    row("name", m.name);
    row("architecture", m.arch);
    row("size", m.size_label);
    if (m.experts) row("experts", std::to_string(m.experts_used) + " active of " + std::to_string(m.experts) + " (MoE)");
    row("quant", m.quant);
    row("file", human_bytes(m.bytes) + (m.parts > 1 ? " in " + std::to_string(m.parts) + " parts" : ""));
    if (m.ctx_train) row("context", std::to_string(m.ctx_train) + " tokens (trained)");
    row("configured", m.config_name.empty() ? "no" : "as [models." + m.config_name + "]");
    row("fit", vram ? fit_verdict(m.bytes, vram) + " in " + human_bytes(vram) + " of VRAM" : "unknown VRAM");
    if (!m.error.empty()) row("problem", m.error);
    if (m.config_name.empty() && m.ok && !m.projector) {
        std::printf("\nWould be added as:\n%s", model_block(suggest_model_name(m.label), m, vram).c_str());
    }
    return m.ok ? 0 : 1;
}

int models_use(const std::string & name, const Config & cfg) {
    if (!cfg.models.count(name)) {
        std::fprintf(stderr, "no [models.%s] in the config; `judas models` lists what is configured\n", name.c_str());
        return 1;
    }
    std::string path = user_config_path();
    if (!set_engine_model_in_file(path, name)) {
        std::fprintf(stderr, "cannot update %s\n", path.c_str());
        return 1;
    }
    std::printf("engine.model = \"%s\" in %s\n", name.c_str(), path.c_str());
    httplib::Client c("127.0.0.1", cfg.server.port);
    c.set_connection_timeout(1);
    if (auto r = c.Get("/health"); r && r->status == 200)
        std::printf("The daemon is running the old model: restart it (systemctl --user restart judas), or switch in the web app under Settings.\n");
    else
        std::printf("Start it with: systemctl --user start judas   (or judas serve)\n");
    return 0;
}

int models_add(const Config & cfg, std::vector<std::string> & args, const GgufInfo * given = nullptr) {
    std::string name = arg_value(args, "--name");
    bool use = arg_flag(args, "--use");
    GgufInfo m;
    if (given) {
        m = *given;
    } else {
        if (args.empty()) {
            std::fprintf(stderr, "usage: judas models add N|PATH [--name X] [--use]\n");
            return 2;
        }
        if (!resolve_model(cfg, args[0], m)) return 1;
    }
    if (!m.ok) {
        std::fprintf(stderr, "cannot use %s: %s\n", m.path.c_str(), m.error.c_str());
        return 1;
    }
    if (!m.config_name.empty() && name.empty()) {
        std::printf("already configured as [models.%s]\n", m.config_name.c_str());
        return use ? models_use(m.config_name, cfg) : 0;
    }
    if (name.empty()) name = suggest_model_name(m.label);
    uint64_t vram = gpu_vram_total();
    std::string path = user_config_path();
    std::string err = add_model_to_config(path, name, m, vram);
    if (!err.empty()) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    std::printf("added [models.%s] to %s%s", name.c_str(), path.c_str(), model_block(name, m, vram).c_str());
    std::string fit = fit_verdict(m.bytes, vram);
    if (fit == "tight") std::printf("%sTight fit: if it fails to load, lower ctx_total.%s\n", C_DIM, C_RESET);
    if (!use) {
        std::printf("Load it with: judas models use %s\n", name.c_str());
        return 0;
    }
    Config fresh = load_or_die();
    return models_use(name, fresh);
}

int models_search(std::vector<std::string> & args) {
    std::string q;
    for (auto & a : args) q += (q.empty() ? "" : " ") + a;
    if (q.empty()) {
        std::fprintf(stderr, "usage: judas models search WORDS   (e.g. judas models search qwen3 coder)\n");
        return 2;
    }
    std::string err;
    auto repos = hf_search(q, 20, &err);
    if (!err.empty()) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    if (repos.empty()) {
        std::printf("No GGUF repos match \"%s\".\n", q.c_str());
        return 0;
    }
    std::printf("\n %-60s %10s %6s\n", "REPO", "DOWNLOADS", "LIKES");
    for (auto & r : repos) std::printf(" %-60s %10lld %6lld\n", r.id.c_str(), r.downloads, r.likes);
    std::printf("\nSee the files: judas models files <repo>      Get one: judas models pull <repo> --add --use\n");
    return 0;
}

int models_files(std::vector<std::string> & args) {
    if (args.empty()) {
        std::fprintf(stderr, "usage: judas models files REPO\n");
        return 2;
    }
    std::string err;
    auto files = hf_gguf_files(args[0], &err);
    if (!err.empty()) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    uint64_t vram = gpu_vram_total();
    print_vram(vram);
    std::printf("\n %-64s %9s  %s\n", "FILE", "SIZE", "FIT");
    for (auto & f : files) {
        bool proj = lower_copy(f.path).find("mmproj") != std::string::npos;
        std::string fit = proj ? "mmproj" : fit_verdict(f.bytes, vram);
        std::printf(" %-64s %9s  %s%s%s\n", f.path.c_str(), human_bytes(f.bytes).c_str(), fit_color(fit), fit.c_str(), C_RESET);
    }
    std::printf("\nGet one: judas models pull %s <file or quant, e.g. Q4_K_M> --add --use\n", args[0].c_str());
    return 0;
}

int models_pull(const Config & cfg, std::vector<std::string> & args) {
    std::string dir = expand_home(arg_value(args, "--dir", "~/models"));
    bool add = arg_flag(args, "--add");
    bool use = arg_flag(args, "--use");
    std::string name = arg_value(args, "--name");
    if (args.empty()) {
        std::fprintf(stderr, "usage: judas models pull REPO [FILE|QUANT] [--dir D] [--add] [--use] [--name X]\n");
        return 2;
    }
    std::string repo = args[0], want = args.size() > 1 ? args[1] : "";
    std::string err;
    auto files = hf_gguf_files(repo, &err);
    if (!err.empty()) {
        std::fprintf(stderr, "%s\n", err.c_str());
        return 1;
    }
    // group split parts under their first file; drop projectors
    struct Pick { std::string first; std::vector<HfFile> parts; uint64_t bytes = 0; };
    std::map<std::string, Pick> groups;
    static const std::regex split(R"(^(.*)-(\d{5})-of-(\d{5})\.gguf$)", std::regex::icase);
    for (auto & f : files) {
        if (lower_copy(f.path).find("mmproj") != std::string::npos) continue;
        std::smatch sm;
        std::string key = std::regex_match(f.path, sm, split) ? sm[1].str() : f.path;
        auto & g = groups[key];
        g.parts.push_back(f);
        g.bytes += f.bytes;
        if (g.first.empty() || f.path < g.first) g.first = f.path;
    }
    if (groups.empty()) {
        std::fprintf(stderr, "no GGUF model files in %s\n", repo.c_str());
        return 1;
    }
    uint64_t vram = gpu_vram_total();
    const Pick * pick = nullptr;
    if (!want.empty()) {
        std::string w = lower_copy(want);
        for (auto & [k, g] : groups)
            if (lower_copy(g.first) == w || lower_copy(k) == w) pick = &g;
        if (!pick) {
            std::vector<const Pick *> hits;
            for (auto & [k, g] : groups)
                if (lower_copy(k).find(w) != std::string::npos) hits.push_back(&g);
            if (hits.size() == 1) pick = hits[0];
            else if (hits.size() > 1) {
                std::fprintf(stderr, "\"%s\" matches %zu files; name one exactly:\n", want.c_str(), hits.size());
                for (auto * h : hits) std::fprintf(stderr, "  %s\n", h->first.c_str());
                return 1;
            }
        }
        if (!pick) {
            std::fprintf(stderr, "no file matching \"%s\"; see: judas models files %s\n", want.c_str(), repo.c_str());
            return 1;
        }
    } else {
        // the biggest file that fits comfortably; failing that, the smallest
        for (auto & [k, g] : groups)
            if (fit_verdict(g.bytes, vram) == "fits" && (!pick || g.bytes > pick->bytes)) pick = &g;
        if (!pick)
            for (auto & [k, g] : groups)
                if (!pick || g.bytes < pick->bytes) pick = &g;
        std::printf("Picked %s (%s, %s for %s of VRAM)\n", pick->first.c_str(), human_bytes(pick->bytes).c_str(),
                    fit_verdict(pick->bytes, vram).c_str(), vram ? human_bytes(vram).c_str() : "unknown");
    }
    std::error_code ec;
    auto space = fs::space(fs::exists(dir) ? dir : fs::path(dir).parent_path().string(), ec);
    if (!ec && space.available < pick->bytes) {
        std::fprintf(stderr, "not enough disk space in %s: need %s, have %s\n", dir.c_str(),
                     human_bytes(pick->bytes).c_str(), human_bytes(space.available).c_str());
        return 1;
    }
    std::string first_local;
    for (auto & part : pick->parts) {
        std::printf("downloading %s/%s -> %s\n", repo.c_str(), part.path.c_str(), dir.c_str());
        auto t0 = std::chrono::steady_clock::now();
        int last = -1;
        std::string local = hf_download(repo, part.path, dir, [&](uint64_t done, uint64_t total) {
            int pct = total ? (int) (done * 100 / total) : -1;
            if (pct == last) return;
            if (!::isatty(1) && pct % 10 != 0) return;   // logs get a line every 10%, terminals a live counter
            last = pct;
            double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            std::printf(::isatty(1) ? "\r  %s / %s  %3d%%  %.0f MB/s   " : "  %s / %s  %3d%%  %.0f MB/s\n", human_bytes(done).c_str(), human_bytes(total).c_str(),
                        pct < 0 ? 0 : pct, s > 0 ? done / 1048576.0 / s : 0.0);
            std::fflush(stdout);
        }, &err);
        std::printf("\n");
        if (local.empty()) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        if (part.path == pick->first) first_local = local;
    }
    std::printf("saved %s\n", first_local.c_str());
    if (!add && !use) {
        std::printf("Connect it: judas models add %s --use\n", first_local.c_str());
        return 0;
    }
    GgufInfo m = gguf_info(first_local);
    std::vector<std::string> rest;
    if (!name.empty()) rest = {"--name", name};
    if (use) rest.push_back("--use");
    return models_add(cfg, rest, &m);
}

int cmd_models(std::vector<std::string> args) {
    if (!::isatty(1)) C_SILVER = C_BRIGHT = C_DIM = C_RED = C_RESET = "";
    Config cfg = load_or_die();
    std::string sub = args.empty() || args[0].rfind("--", 0) == 0 ? "list" : args[0];
    if (!args.empty() && sub == args[0]) args.erase(args.begin());
    if (sub == "list" || sub == "ls") return models_list(cfg, args);
    if (sub == "info") return models_info(cfg, args);
    if (sub == "add") return models_add(cfg, args);
    if (sub == "use") {
        if (args.empty()) {
            std::fprintf(stderr, "usage: judas models use NAME\n");
            return 2;
        }
        return models_use(args[0], cfg);
    }
    if (sub == "search") return models_search(args);
    if (sub == "files") return models_files(args);
    if (sub == "pull" || sub == "download") return models_pull(cfg, args);
    std::fprintf(stderr, "unknown: judas models %s (list, info, add, use, search, files, pull)\n", sub.c_str());
    return 2;
}

}  // namespace

int main(int argc, char ** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (!args.empty() && (args[0] == "-h" || args[0] == "--help" || args[0] == "help")) {
        std::printf("%s", USAGE);
        return 0;
    }
    if (args.empty() || args[0][0] == '-') return cmd_tui(args);  // bare `judas` (or just flags) opens the TUI
    std::string cmd = args[0];
    args.erase(args.begin());
    try {
        if (cmd == "chat" || cmd == "tui") return cmd_tui(args);
        if (cmd == "serve") return cmd_serve(args);
        if (cmd == "run") return cmd_run(args);
        if (cmd == "state") return cmd_get("/state");
        if (cmd == "sessions") return cmd_get("/api/sessions");
        if (cmd == "snap") return cmd_snap(args);
        if (cmd == "curator") return cmd_curator(args);
        if (cmd == "config") return cmd_config(args);
        if (cmd == "skills") return cmd_skills(args);
        if (cmd == "models") return cmd_models(args);
        if (cmd == "prompt-size") {
            Config cfg = load_or_die();
            auto est = [](const std::string & s) { return (long) (s.size() / 4); };  // ~4 bytes/token
            ToolRegistry reg;
            register_builtin_tools(reg);
            register_memory_tools(reg);
            long full = 0, lazy = 0;
            std::printf("%stool schemas (estimated tokens, ~4 bytes each):%s\n", C_BRIGHT, C_RESET);
            for (const Tool * t : reg.list()) {
                long n = est(t->name + t->description + t->parameters.dump());
                bool occasional = t->name.rfind("web_", 0) == 0 || t->name.rfind("snapshot_", 0) == 0;
                full += n;
                if (!occasional) lazy += n;
                std::printf("  %-16s %4ld%s\n", t->name.c_str(), n, occasional ? "  (dropped when tool_schema=lazy)" : "");
            }
            Skills sk(cfg.skills);
            long skidx = est(sk.index_block());
            std::printf("%stotals:%s tools full %ld, tools lazy %ld, skills index %ld (%zu skills), memory cap %d\n",
                        C_BRIGHT, C_RESET, full, lazy, skidx, sk.all().size(), cfg.prompt.max_memory_tokens);
            std::printf("current tool_schema = %s\n", cfg.prompt.tool_schema.c_str());
            return 0;
        }
        if (cmd == "version") {
            std::printf("judas %s\n", version());
            return 0;
        }
    } catch (const std::exception & e) {
        std::fprintf(stderr, "%serror:%s %s\n", C_RED, C_RESET, e.what());
        return 1;
    }
    std::fprintf(stderr, "judas: unknown command %s\n\n%s", cmd.c_str(), USAGE);
    return 2;
}
