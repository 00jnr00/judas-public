// judas: the betrayer harness. One binary: daemon (serve), one-shot agent runs, snapshots, config.

#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "agent/agent.h"
#include "core/config.h"
#include "cpp-httplib/httplib.h"
#include "engine/engine.h"
#include "learning/curator.h"
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
