// Judas TUI: a client of the daemon (judas serve). Raw ANSI + termios, no alternate screen: finished
// transcript lines are printed once into the normal scrollback, and a small live area at the bottom
// (running tools, streamed partial line, input box, status line) is redrawn in place.
//
// Threads: the main thread owns the terminal and all UI state. A follower thread reads the session's SSE
// stream, a poller thread reads /state; both push into a queue and poke a pipe that wakes the main loop.
#include "tui/tui.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>

#include "core/json.h"
#include "cpp-httplib/httplib.h"
#include "learning/learn.h"
#include "tui/commands.h"
#include "tui/input.h"
#include "tui/layout.h"
#include "tui/text.h"
#include "tui/view.h"

namespace judas {
namespace {

using json = judas::json;
using namespace judas::tui;
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

int g_wake_fd = -1;
volatile sig_atomic_t g_winch = 0, g_term = 0;

void on_signal(int sig) {
    if (sig == SIGWINCH) g_winch = 1;
    else g_term = 1;
    if (g_wake_fd >= 0) { char c = 'w'; ssize_t r = write(g_wake_fd, &c, 1); (void) r; }
}

std::string jstr(const json & j, const char * k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}

double now_s() { return std::chrono::duration<double>(Clock::now().time_since_epoch()).count(); }

// Data shared with the network threads.
struct Net {
    std::mutex mu;
    std::deque<json> events;
    json state = json::object();
    bool connected = true;
    std::atomic<int> gen{0};
    std::atomic<bool> quit{false};
    std::shared_ptr<httplib::Client> sse;  // the live SSE client, so a switch can break its read
    int wake_w = -1;
    void wake() { char c = 'e'; ssize_t r = write(wake_w, &c, 1); (void) r; }
};

void follow_thread(std::shared_ptr<Net> net, int port, std::string sid, int gen, long long since) {
    while (!net->quit && net->gen == gen) {
        auto cli = std::make_shared<httplib::Client>("127.0.0.1", port);
        cli->set_connection_timeout(2, 0);
        cli->set_read_timeout(60, 0);
        { std::lock_guard<std::mutex> lk(net->mu); if (net->gen != gen) return; net->sse = cli; }
        std::string path = "/api/sessions/" + sid + "/events?follow=1";
        if (since >= 0) path += "&since=" + std::to_string(since);
        std::string buf;
        bool got = false;
        auto res = cli->Get(path, [&](const char * d, size_t n) {
            if (net->quit || net->gen != gen) return false;
            buf.append(d, n);
            size_t p;
            while ((p = buf.find("\n\n")) != std::string::npos) {
                std::string blk = buf.substr(0, p);
                buf.erase(0, p + 2);
                long long id = -1;
                std::string data;
                size_t i = 0;
                while (i < blk.size()) {
                    size_t e = blk.find('\n', i);
                    std::string ln = blk.substr(i, e == std::string::npos ? std::string::npos : e - i);
                    i = e == std::string::npos ? blk.size() : e + 1;
                    if (ln.rfind("id:", 0) == 0) id = std::atoll(ln.c_str() + 3);
                    else if (ln.rfind("data:", 0) == 0) data += ln.substr(ln.size() > 5 && ln[5] == ' ' ? 6 : 5);
                }
                if (data.empty()) continue;
                auto ev = json::parse(data, nullptr, false);
                if (ev.is_discarded()) continue;
                if (id >= 0) since = id;
                got = true;
                std::lock_guard<std::mutex> lk(net->mu);
                net->events.push_back(std::move(ev));
            }
            net->wake();
            return true;
        });
        (void) got;
        if (net->quit || net->gen != gen) return;
        {
            std::lock_guard<std::mutex> lk(net->mu);
            net->connected = false;
        }
        net->wake();
        (void) res;
        for (int i = 0; i < 20 && !net->quit && net->gen == gen; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        {
            std::lock_guard<std::mutex> lk(net->mu);
            net->connected = true;
        }
    }
}

void poll_thread(std::shared_ptr<Net> net, int port) {
    httplib::Client c("127.0.0.1", port);
    c.set_connection_timeout(1, 0);
    c.set_read_timeout(2, 0);
    while (!net->quit) {
        auto r = c.Get("/state");
        {
            std::lock_guard<std::mutex> lk(net->mu);
            if (r && r->status == 200) {
                auto j = json::parse(r->body, nullptr, false);
                if (!j.is_discarded()) net->state = j;
                net->connected = true;
            } else {
                net->connected = false;
            }
        }
        net->wake();
        for (int i = 0; i < 10 && !net->quit; i++) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

void fcntl_nonblock(int fd) { fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK); }

struct Reply {
    int status = 0;
    json body;
};

struct Api {
    int port;
    Reply call(const char * method, const std::string & path, const std::string & body = "") {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(2, 0);
        c.set_read_timeout(8, 0);
        httplib::Result r(nullptr, httplib::Error::Unknown);
        std::string m = method;
        if (m == "GET") r = c.Get(path);
        else if (m == "POST") r = c.Post(path, body.empty() ? "{}" : body, "application/json");
        else if (m == "PATCH") r = c.Patch(path, body, "application/json");
        Reply out;
        if (!r) return out;
        out.status = r->status;
        auto j = json::parse(r->body, nullptr, false);
        out.body = j.is_discarded() ? json::object() : j;
        return out;
    }
    Reply get(const std::string & p) { return call("GET", p); }
    Reply post(const std::string & p, const json & b = json::object()) { return call("POST", p, b.dump()); }
    Reply patch(const std::string & p, const json & b) { return call("PATCH", p, b.dump()); }
};

std::string err_text(const Reply & r) {
    if (r.status == 0) return "the daemon is not reachable";
    if (r.body.is_object() && r.body.contains("error")) {
        const json & e = r.body["error"];
        if (e.is_object() && e.contains("message")) return e["message"].dump();
        if (e.is_string()) return e.get<std::string>();
    }
    return "HTTP " + std::to_string(r.status);
}

std::string sh_quote(const std::string & s) {
    std::string o = "'";
    for (char c : s) o += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return o + "'";
}

bool have_wl_copy() {
    const char * p = std::getenv("PATH");
    if (!p) return false;
    std::string path = p;
    size_t i = 0;
    while (i <= path.size()) {
        size_t e = path.find(':', i);
        std::string d = path.substr(i, e == std::string::npos ? std::string::npos : e - i);
        if (!d.empty() && access((d + "/wl-copy").c_str(), X_OK) == 0) return true;
        if (e == std::string::npos) break;
        i = e + 1;
    }
    return false;
}

std::string ago(long long ms) {
    long long s = (std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() - ms) / 1000;
    if (s < 0) s = 0;
    if (s < 90) return std::to_string(s) + "s ago";
    if (s < 5400) return std::to_string(s / 60) + "m ago";
    if (s < 129600) return std::to_string(s / 3600) + "h ago";
    return std::to_string(s / 86400) + "d ago";
}

struct App {
    Config cfg;
    TuiOptions opts;
    Api api;
    std::shared_ptr<Net> net = std::make_shared<Net>();
    Palette pal;
    Stream stream;
    Editor ed;
    History hist;
    KeyDecoder dec;
    std::string cwd, sid, mode = "chat", flash;
    double flash_until = 0, last_ctrlc = 0, turn_start = 0, last_esc = 0;
    bool quit = false, dismissed = false;
    std::string dismissed_text, last_menu_text;
    int menu_sel = 0, spin = 0, rows = 24, cols = 80, region_cursor_row = 0, region_rows = 0;
    std::vector<std::string> queued;
    std::vector<std::string> models;
    // generic picker (sessions, models)
    Menu picker;
    std::string picker_kind;
    int wake_r = -1, wake_w = -1;
    termios orig{};
    bool raw = false;

    App(const Config & c, const TuiOptions & o)
        : cfg(c), opts(o), api{c.server.port}, pal(Palette::from_skin(std::getenv("JUDAS_SKIN") ? std::getenv("JUDAS_SKIN") : "judas")),
          stream(pal, 80), hist(expand_home("~/.local/share/judas/tui_history")) {}

    // ---- terminal -------------------------------------------------------------------------------------------
    void out(const std::string & s) {
        size_t off = 0;
        while (off < s.size()) {
            ssize_t n = write(STDOUT_FILENO, s.data() + off, s.size() - off);
            if (n <= 0) break;
            off += (size_t) n;
        }
    }
    void measure() {
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) { cols = ws.ws_col; rows = ws.ws_row ? ws.ws_row : 24; }
        stream.set_width(cols - 1);
    }
    void enter_raw() {
        tcgetattr(STDIN_FILENO, &orig);
        termios t = orig;
        t.c_iflag &= ~(IXON | ICRNL | INLCR | ISTRIP | BRKINT);
        t.c_lflag &= ~(ICANON | ECHO | ISIG | IEXTEN);
        t.c_cc[VMIN] = 1;
        t.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &t);
        raw = true;
        out(terminal_input_enable());
    }
    void leave_raw() {
        if (!raw) return;
        out(std::string(terminal_input_disable()) + "\033[?25h");
        tcsetattr(STDIN_FILENO, TCSANOW, &orig);
        raw = false;
    }

    // ---- live area ------------------------------------------------------------------------------------------
    json state() { std::lock_guard<std::mutex> lk(net->mu); return net->state; }

    std::string status_row() {
        json st = state();
        StatusInfo si;
        si.model = jstr(st, "model");
        si.mode = mode;
        si.session = sid;
        si.turn = stream.turn_status();
        si.kv_used = st.value("ctx_used", 0LL);
        si.kv_total = st.value("ctx_total", 0LL);
        si.queued = st.value("queued", 0);
        si.active = st.value("active", 0);
        si.tok_s = st.value("gen_tok_s", 0.0);
        si.pending_msgs = (int) queued.size();
        { std::lock_guard<std::mutex> lk(net->mu); si.connected = net->connected; }
        return render_status(si, cols - 1, pal);
    }

    Menu current_menu() {
        if (picker.active()) { Menu m = picker; m.sel = std::clamp(menu_sel, 0, (int) m.items.size() - 1); return m; }
        if (stream.approval) return Menu();
        MenuContext mc;
        mc.cwd = cwd;
        mc.models = models;
        Menu m = compute_menu(ed.text, ed.cur, mc);
        if (m.active() && dismissed && dismissed_text == ed.text) return Menu();
        if (ed.text != last_menu_text) { menu_sel = 0; last_menu_text = ed.text; }
        m.sel = std::clamp(menu_sel, 0, std::max(0, (int) m.items.size() - 1));
        return m;
    }

    // Redraw: print committed rows into scrollback, then rebuild the live area in place.
    void repaint(bool full_clear = false) {
        std::string f = "\033[?2026h\033[?25l";
        if (full_clear) { f += "\033[2J\033[H"; region_cursor_row = 0; }
        else {
            if (region_cursor_row > 0) f += "\033[" + std::to_string(region_cursor_row) + "A";
            f += "\r\033[J";
        }
        for (auto & r : stream.take_committed()) f += r + pal.reset + "\r\n";
        // build the live area
        std::vector<std::string> rowsv;
        Menu menu = current_menu();
        BoxView box = render_input_box(ed, cols - 1, pal, "Message Judas  (/ commands, ! shell, @ files)",
                                       !ed.text.empty() && ed.text[0] == '!', 8);
        std::vector<std::string> below;
        if (menu.active()) below = render_menu(menu, cols - 1, pal, 8);
        else if (stream.approval) below.push_back(pal.warn + std::string("  Allow ") + truncate_width(one_line(stream.approval->what), cols - 40) +
                                                   "?  [y] yes  [n] no  [a] always" + pal.reset);
        else if (!flash.empty() && now_s() < flash_until) below.push_back(pal.warn + std::string(" ") + flash + pal.reset);
        else below.push_back(status_row());
        int reserve = (int) box.rows.size() + (int) below.size() + (int) queued.size() + 1;
        auto live = stream.live(spin, turn_start > 0 ? now_s() - turn_start : 0);
        int room = std::max(1, rows - reserve - 1);
        if ((int) live.size() > room) live.erase(live.begin(), live.end() - room);
        for (auto & l : live) rowsv.push_back(l);
        for (auto & q : queued) rowsv.push_back(pal.dim + std::string("  queued: ") + truncate_width(one_line(q), cols - 14) + pal.reset);
        int base = (int) rowsv.size();
        for (auto & r : box.rows) rowsv.push_back(r);
        for (auto & r : below) rowsv.push_back(r);
        for (size_t i = 0; i < rowsv.size(); i++) f += rowsv[i] + (i + 1 < rowsv.size() ? "\r\n" : "");
        int last = (int) rowsv.size() - 1;
        int cur_row = base + box.cursor_row;
        if (last - cur_row > 0) f += "\033[" + std::to_string(last - cur_row) + "A";
        f += "\r";
        if (box.cursor_col > 0) f += "\033[" + std::to_string(box.cursor_col) + "C";
        f += "\033[?25h\033[?2026l";
        region_cursor_row = cur_row;
        region_rows = (int) rowsv.size();
        out(f);
    }

    void say(const std::string & s, char level = 'd') { stream.note(s, level); }
    void set_flash(const std::string & s, double secs = 3) { flash = s; flash_until = now_s() + secs; }

    // ---- sessions -------------------------------------------------------------------------------------------
    void stop_follow() {
        std::lock_guard<std::mutex> lk(net->mu);
        net->gen++;
        if (net->sse) { net->sse->stop(); net->sse.reset(); }
        net->events.clear();
    }
    void start_follow(long long since) {
        int gen;
        { std::lock_guard<std::mutex> lk(net->mu); gen = ++net->gen; }
        std::thread(follow_thread, net, cfg.server.port, sid, gen, since).detach();
    }

    void new_session() {
        stop_follow();
        sid.clear();
        stream.reset();
        queued.clear();
        turn_start = 0;
        say("new session (created with your first message)");
    }

    bool open_session(const std::string & id_or_prefix) {
        std::string id = id_or_prefix;
        if (id.size() < 16) {  // prefix lookup
            auto r = api.get("/api/sessions?limit=200");
            std::string hit;
            int n = 0;
            if (r.body.contains("sessions"))
                for (auto & s : r.body["sessions"])
                    if (jstr(s, "id").rfind(id, 0) == 0) { hit = jstr(s, "id"); n++; }
            if (n != 1) { say(n ? "ambiguous session prefix: " + id : "no session starts with " + id, 'e'); return false; }
            id = hit;
        }
        auto r = api.get("/api/sessions/" + id);
        if (r.status != 200) { say("could not open session: " + err_text(r), 'e'); return false; }
        stop_follow();
        stream.reset();
        queued.clear();
        sid = id;
        const json & s = r.body["session"];
        if (!jstr(s, "mode").empty()) mode = jstr(s, "mode");
        const json & t = r.body["turn"];
        bool running = t.value("running", false);
        long long floor = t.value("msg_floor", 0LL);
        stream.note("session " + sid.substr(0, 8) + "  " + jstr(s, "title") + "  (" + mode + ")", 'n');
        stream.load_history(r.body["messages"], r.body["agents"], running ? floor : -1);
        if (r.body.value("more_before", false)) say("(earlier messages not shown)");
        if (running) {
            stream.set_running(true);
            turn_start = now_s();
            start_follow(t.value("event_floor", 0LL) > 0 ? (long long) t.value("event_floor", 0LL) : -1);
        } else {
            std::string st = jstr(t, "status");
            if (st == "paused") {
                stream.on_event(json{{"type", "done"}, {"agent", "-"}, {"parent", ""}, {"role", "lead"},
                                     {"data", {{"status", "paused"}, {"reason", jstr(t, "reason")}}}});
            }
            start_follow(t.value("events_head", 0LL));
        }
        return true;
    }

    void send_text(const std::string & shown, const std::string & full) {
        if (stream.running()) {
            queued.push_back(full);
            return;
        }
        json body = {{"content", full}, {"cwd", cwd}, {"mode", mode}};
        auto r = api.call("POST", "/api/sessions/" + (sid.empty() ? std::string("new") : sid) + "/messages", body.dump());
        if (r.status != 200) { say("send failed: " + err_text(r), 'e'); return; }
        std::string got = r.body["session"].is_string() ? r.body["session"].get<std::string>() : jstr(r.body["session"], "id");
        stream.user_message(shown);
        stream.set_running(true);
        turn_start = now_s();
        if (sid.empty() || sid != got) {
            sid = got;
            stop_follow();
            start_follow(-1);
        }
    }

    // ---- commands -------------------------------------------------------------------------------------------
    void help() {
        say("Commands (type / for the menu):", 'n');
        for (auto & c : commands())
            say("  /" + c.name + (c.args_hint.empty() ? "" : " " + c.args_hint) + "  " + c.desc);
        say("Input:", 'n');
        say("  Enter send   Shift+Enter, Alt+Enter, Ctrl+J or \\ then Enter: newline");
        say("  Up/Down history   Ctrl+A/E line start/end   Ctrl+K/U kill to end/start   Ctrl+W delete word");
        say("  Esc interrupt   Ctrl+C cancel turn (twice to quit)   Ctrl+L redraw   Ctrl+O show thinking and full tool output");
        say("  ! cmd runs a shell command locally   @path completes files   Tab completes the menu");
        say("Select and copy with the mouse as usual; /copy copies the last reply (OSC 52, wl-copy as fallback).");
    }

    void do_copy(const std::string & arg) {
        auto reps = stream.replies();
        if (reps.empty()) { say("nothing to copy yet", 'w'); return; }
        int n = arg.empty() ? 1 : std::atoi(arg.c_str());
        if (n < 1 || n > (int) reps.size()) { say("/copy N: N is 1 to " + std::to_string(reps.size()) + " (1 = last reply)", 'w'); return; }
        const std::string & text = reps[reps.size() - n];
        std::string via;
        if (text.size() < 90000) { out(osc52(text)); via = "OSC 52"; }
        if (!std::getenv("SSH_CONNECTION") && std::getenv("WAYLAND_DISPLAY") && have_wl_copy()) {
            FILE * p = popen("wl-copy 2>/dev/null", "w");
            if (p) { fwrite(text.data(), 1, text.size(), p); if (pclose(p) == 0) via += (via.empty() ? "" : " + ") + std::string("wl-copy"); }
        }
        if (via.empty()) say("reply is too large for OSC 52 and wl-copy is not available", 'e');
        else say("copied " + std::to_string(text.size()) + " chars (" + via + ")");
    }

    void list_sessions() {
        auto r = api.get("/api/sessions?limit=30");
        if (r.status != 200) { say("could not list sessions: " + err_text(r), 'e'); return; }
        Menu m;
        m.kind = Menu::Picker;
        for (auto & s : r.body["sessions"]) {
            std::string id = jstr(s, "id");
            std::string title = truncate_width(one_line(jstr(s, "title")), 44);
            m.items.push_back({id, id.substr(0, 16) + "  " + title,
                               jstr(s, "mode") + (s.value("running", false) ? "  running" : "") + "  " + ago(s.value("updated_ms", 0LL))});
        }
        if (m.items.empty()) { say("no sessions yet"); return; }
        picker = m;
        picker_kind = "session";
        menu_sel = 0;
    }

    void list_models(const std::string & arg) {
        auto r = api.get("/api/models");
        if (r.status != 200) { say("could not list models: " + err_text(r), 'e'); return; }
        models.clear();
        Menu m;
        m.kind = Menu::Picker;
        std::string active = jstr(r.body, "active");
        for (auto & x : r.body["models"]) {
            std::string name = jstr(x, "name");
            models.push_back(name);
            m.items.push_back({name, name + (name == active ? "  (active)" : ""),
                               "ctx " + human_tokens(x.value("ctx_total", 0LL)) + (x.value("gpu_layers", 0) ? "  gpu" : "")});
        }
        if (!arg.empty()) { switch_model(arg); return; }
        picker = m;
        picker_kind = "model";
        menu_sel = 0;
    }

    void switch_model(const std::string & name) {
        auto r = api.post("/api/model", {{"name", name}});
        if (r.status != 200) { say("model switch failed: " + err_text(r), 'e'); return; }
        if (r.body.value("restarting", false)) say("model set to " + name + "; the daemon is restarting to load it", 'w');
        else say("model set to " + name + " in the config; restart the daemon to load it (" + jstr(r.body, "restart_cmd") + ")", 'w');
    }

    void show_agents() {
        if (sid.empty()) { say("no session yet"); return; }
        auto r = api.get("/api/sessions/" + sid + "?limit=1");
        if (r.status != 200) { say("could not read agents: " + err_text(r), 'e'); return; }
        if (r.body["agents"].empty()) { say("no agents in this session"); return; }
        say("agents:", 'n');
        for (auto & a : r.body["agents"]) {
            bool lead = jstr(a, "parent").empty();
            std::string line = std::string(lead ? "  " : "    ") + jstr(a, "id").substr(0, 6) + "  " + jstr(a, "role") + "  " +
                               jstr(a, "status") + "  " + human_tokens(a.value("completion_tokens", 0LL)) + " tok  " +
                               truncate_width(one_line(jstr(a, "task")), 50);
            say(line);
        }
    }

    void show_state() {
        auto r = api.get("/api/state");
        if (r.status != 200) { say("daemon: " + err_text(r), 'e'); return; }
        const json & s = r.body;
        say("model " + jstr(s, "model") + (s.value("loaded", false) ? " (loaded)" : " (not loaded)") + "  version " + jstr(s, "version"), 'n');
        say("KV " + human_tokens(s.value("ctx_used", 0LL)) + "/" + human_tokens(s.value("ctx_total", 0LL)) + "  active " +
            std::to_string(s.value("active", 0)) + "  queued " + std::to_string(s.value("queued", 0)) + "  max agents " +
            std::to_string(s.value("max_agents", 0)));
        char b[96];
        std::snprintf(b, sizeof b, "%.1f tok/s  VRAM %.1f/%.1f GiB  requests %lld", s.value("gen_tok_s", 0.0),
                      s.value("vram_used_mib", 0.0) / 1024, s.value("vram_total_mib", 0.0) / 1024, s.value("requests", 0LL));
        say(b);
    }

    void memory_search(const std::string & q) {
        std::ifstream f(expand_home("~/.local/share/shared-memory/SHARED.md"));
        if (!f) { say("no shared memory file at ~/.local/share/shared-memory/SHARED.md", 'w'); return; }
        std::vector<std::string> words;
        {
            std::string w, lq = q;
            for (auto & c : lq) c = (char) std::tolower((unsigned char) c);
            for (char c : lq + " ") { if (c == ' ') { if (!w.empty()) words.push_back(w); w.clear(); } else w += c; }
        }
        std::string line;
        int n = 0;
        while (std::getline(f, line) && n < 15) {
            std::string l = line;
            for (auto & c : l) c = (char) std::tolower((unsigned char) c);
            bool all = !words.empty();
            for (auto & w : words) all = all && l.find(w) != std::string::npos;
            if (all) { say(truncate_width(line, cols * 3 - 6), 'n'); n++; }
        }
        if (!n) say("no matches in shared memory");
    }

    void run_shell(const std::string & cmdline) {
        say("$ " + cmdline, 'w');
        std::string full = "cd " + sh_quote(cwd) + " && timeout 120 sh -c " + sh_quote(cmdline) + " </dev/null 2>&1";
        FILE * p = popen(full.c_str(), "r");
        if (!p) { say("could not run the command", 'e'); return; }
        std::string data;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, p)) > 0 && data.size() < 65536) data.append(buf, n);
        int rc = pclose(p);
        int lines = 0;
        size_t pos = 0;
        while (pos < data.size()) {
            size_t nl = data.find('\n', pos);
            std::string ln = data.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? data.size() : nl + 1;
            if (lines++ >= 200) { say("... (output cut at 200 lines)"); break; }
            stream.commit({pal.dim + std::string("  ") + strip_ansi(ln) + pal.reset});
        }
        if (WIFEXITED(rc) && WEXITSTATUS(rc) != 0) say("exit status " + std::to_string(WEXITSTATUS(rc)), 'w');
    }

    void run_slash(const Parsed & p) {
        const std::string & n = p.name;
        if (!find_command(n)) { say("unknown command /" + n + "  (try /help)", 'w'); return; }
        if (n == "help") help();
        else if (n == "new") new_session();
        else if (n == "sessions") list_sessions();
        else if (n == "resume") {
            if (sid.empty()) { say("no session to resume", 'w'); return; }
            auto r = api.post("/api/sessions/" + sid + "/resume");
            if (r.status != 200) { say("resume: " + err_text(r), 'w'); return; }
            stream.set_running(true);
            turn_start = now_s();
            say("resuming");
        } else if (n == "cancel") {
            if (sid.empty() || !stream.running()) { say("nothing is running"); return; }
            api.post("/api/sessions/" + sid + "/cancel");
            queued.clear();
            say("cancel requested");
        } else if (n == "mode") {
            if (p.arg.empty()) { say("mode: " + mode + "  (/mode chat|cowork|code)"); return; }
            if (p.arg != "chat" && p.arg != "cowork" && p.arg != "code") { say("mode must be chat, cowork or code", 'w'); return; }
            mode = p.arg;
            if (!sid.empty()) {
                auto r = api.patch("/api/sessions/" + sid, {{"mode", mode}});
                if (r.status != 200) { say("mode: " + err_text(r), 'e'); return; }
            }
            say("mode: " + mode);
        } else if (n == "model") list_models(p.arg);
        else if (n == "agents") show_agents();
        else if (n == "plan") {
            if (stream.plan().empty()) { say("no plan yet (the agent writes one with update_plan on bigger tasks)"); return; }
            say("plan:", 'n');
            size_t pos = 0;
            const std::string & pl = stream.plan();
            while (pos < pl.size()) {
                size_t nl = pl.find('\n', pos);
                stream.commit({"  " + pl.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos)});
                pos = nl == std::string::npos ? pl.size() : nl + 1;
            }
        } else if (n == "state") show_state();
        else if (n == "copy") do_copy(p.arg);
        else if (n == "expand") {
            int k = p.arg.empty() ? 0 : std::atoi(p.arg.c_str());
            const ToolLog * t = stream.tool(k);
            if (!t) { say("no such tool call", 'w'); return; }
            say("#" + std::to_string(t->n) + " " + t->name + " " + t->label, 'n');
            stream.commit(stream.expand_rows(t->n));
        } else if (n == "clear") {
            repaint(true);
            return;
        } else if (n == "auto-resume") {
            if (p.arg != "on" && p.arg != "off") { say("usage: /auto-resume on|off", 'w'); return; }
            if (sid.empty()) { say("send a message first (auto-resume is per session)", 'w'); return; }
            auto r = api.patch("/api/sessions/" + sid, {{"auto_resume", p.arg == "on"}});
            say(r.status == 200 ? "auto-resume " + p.arg : "auto-resume: " + err_text(r), r.status == 200 ? 'd' : 'e');
        } else if (n == "memory") {
            if (p.arg.empty()) { say("usage: /memory <query>", 'w'); return; }
            memory_search(p.arg);
        } else if (n == "learn") send_text("/learn" + (p.arg.empty() ? "" : " " + p.arg), build_learn_prompt(p.arg));
        else if (n == "quit") quit = true;
    }

    void submit() {
        std::string visible = ed.text, full = ed.expanded();
        Parsed p = parse_input(visible);
        if (p.kind == Parsed::Empty) return;
        hist.add(visible.size() > 4000 ? visible : full.size() < 20000 ? full : visible);
        ed.clear();
        if (p.kind == Parsed::Slash) run_slash(p);
        else if (p.kind == Parsed::Shell) run_shell(p.arg);
        else send_text(visible, full);
    }

    // ---- keys -----------------------------------------------------------------------------------------------
    void apply_completion(const Menu & m) {
        const MenuItem & it = m.items[m.sel];
        ed.text.replace(m.begin, m.end - m.begin, it.value);
        ed.cur = m.begin + it.value.size();
        dismissed = false;
    }

    void interrupt() {
        if (sid.empty() || !stream.running()) return;
        api.post("/api/sessions/" + sid + "/cancel");
        queued.clear();
        set_flash("interrupting...");
    }

    void on_ctrl_c() {
        double t = now_s();
        if (picker.active()) { picker = Menu(); return; }
        if (t - last_ctrlc < 2.0) { quit = true; return; }
        last_ctrlc = t;
        if (stream.running()) { interrupt(); set_flash("turn cancelled; Ctrl+C again to quit"); }
        else if (!ed.empty()) ed.clear();
        else set_flash("press Ctrl+C again to quit");
    }

    void answer_approval(const std::string & decision) {
        if (!stream.approval || sid.empty()) return;
        api.post("/api/sessions/" + sid + "/approvals", {{"id", stream.approval->id}, {"decision", decision}});
        say(std::string("  ") + (decision == "no" ? "denied: " : decision == "always" ? "allowed (always): " : "allowed: ") +
            truncate_width(one_line(stream.approval->what), cols - 24));
        stream.approval.reset();
    }

    void on_key(const Key & k) {
        // approval prompt first
        if (stream.approval) {
            if (k.type == Key::Char && !k.ctrl && !k.alt && (k.text == "y" || k.text == "Y")) return answer_approval("yes");
            if (k.type == Key::Char && !k.ctrl && !k.alt && (k.text == "n" || k.text == "N")) return answer_approval("no");
            if (k.type == Key::Char && !k.ctrl && !k.alt && (k.text == "a" || k.text == "A")) return answer_approval("always");
            if (k.type == Key::Enter) return answer_approval("yes");
            if (k.type == Key::Esc) return answer_approval("no");
        }
        Menu menu = current_menu();
        const bool picking = picker.active();
        if (k.type == Key::Char && k.ctrl && !k.alt) {
            char c = k.text.empty() ? 0 : k.text[0];
            switch (c) {
                case 'c': return on_ctrl_c();
                case 'd': if (ed.empty()) { quit = true; } else ed.del(); return;
                case 'l': repaint(true); return;
                case 'o':
                    stream.set_verbose(!stream.verbose());
                    set_flash(stream.verbose() ? "verbose on: thinking and tool output are shown in full" : "verbose off");
                    return;
                case 'a': ed.home(); return;
                case 'e': ed.end(); return;
                case 'k': ed.kill_to_eol(); return;
                case 'u': ed.kill_to_bol(); return;
                case 'w': ed.kill_word(); return;
                case 'b': ed.left(); return;
                case 'f': ed.right(); return;
                case 'p':
                    if (menu.active()) menu_sel = std::max(0, menu_sel - 1); else history_prev();
                    return;
                case 'n':
                    if (menu.active()) menu_sel = std::min((int) menu.items.size() - 1, menu_sel + 1); else history_next();
                    return;
                case 'j': ed.newline(); return;
                default: return;
            }
        }
        if (k.alt && k.type == Key::Char) {
            if (k.text == "b") ed.word_left();
            else if (k.text == "f") ed.word_right();
            else if (k.text == "d") { size_t s = ed.cur; ed.word_right(); ed.text.erase(s, ed.cur - s); ed.cur = s; }
            return;
        }
        switch (k.type) {
            case Key::Paste:
                if (!picking) ed.paste(k.text);
                break;
            case Key::Char:
                if (picking) break;
                ed.insert(k.text);
                dismissed = false;
                break;
            case Key::Enter:
                if (picking) {
                    const MenuItem it = picker.items[menu.sel];
                    std::string kind = picker_kind;
                    picker = Menu();
                    if (kind == "session") open_session(it.value);
                    else if (kind == "model") switch_model(it.value);
                    break;
                }
                if (k.shift || k.alt) { ed.newline(); break; }
                if (menu.active() && menu.kind == Menu::At) { apply_completion(menu); break; }
                if (menu.active() && (menu.kind == Menu::Slash || menu.kind == Menu::Arg)) {
                    const MenuItem & it = menu.items[menu.sel];
                    if (ed.text != it.value) {
                        apply_completion(menu);
                        if (it.value.back() == ' ') break;  // needs an argument: leave it for the user
                    }
                    submit();
                    break;
                }
                if (ed.cur == ed.text.size() && !ed.text.empty() && ed.text.back() == '\\') {
                    ed.text.back() = '\n';
                    ed.cur = ed.text.size();
                    break;
                }
                submit();
                break;
            case Key::Tab:
                if (menu.active()) apply_completion(menu);
                break;
            case Key::BackTab: break;
            case Key::Backspace: if (!picking) { ed.backspace(); dismissed = false; } break;
            case Key::Delete: if (!picking) ed.del(); break;
            case Key::Left:
                if (k.ctrl || k.alt) ed.word_left(); else ed.left();
                break;
            case Key::Right:
                if (k.ctrl || k.alt) ed.word_right(); else ed.right();
                break;
            case Key::Home: ed.home(); break;
            case Key::End: ed.end(); break;
            case Key::Up:
                if (menu.active()) menu_sel = std::max(0, menu_sel - 1);
                else if (!ed.up()) history_prev();
                break;
            case Key::Down:
                if (menu.active()) menu_sel = std::min((int) menu.items.size() - 1, menu_sel + 1);
                else if (!ed.down()) history_next();
                break;
            case Key::PgUp: case Key::PgDn: break;
            case Key::Esc: {
                double t = now_s();
                if (picking) { picker = Menu(); break; }
                if (menu.active()) { dismissed = true; dismissed_text = ed.text; break; }
                if (stream.running()) { interrupt(); break; }
                if (!ed.empty()) {
                    if (t - last_esc < 1.0) ed.clear(); else set_flash("press Esc again to clear the input", 1.0);
                }
                last_esc = t;
                break;
            }
        }
    }
    void history_prev() { std::string o; if (hist.prev(ed.text, o)) ed.set(o); }
    void history_next() { std::string o; if (hist.next(o)) ed.set(o); }

    // ---- main loop ------------------------------------------------------------------------------------------
    void drain_events() {
        std::deque<json> evs;
        { std::lock_guard<std::mutex> lk(net->mu); evs.swap(net->events); }
        bool was = stream.running();
        for (auto & e : evs) stream.on_event(e);
        if (!was && stream.running()) turn_start = now_s();
        if (was && !stream.running() && !queued.empty()) {
            std::string next = queued.front();
            queued.erase(queued.begin());
            send_text(next, next);
        }
    }

    int run() {
        measure();
        int fds[2];
        if (pipe(fds) != 0) return 1;
        wake_r = fds[0];
        wake_w = fds[1];
        for (int fd : fds) fcntl_nonblock(fd);
        g_wake_fd = wake_w;
        net->wake_w = wake_w;
        struct sigaction sa{};
        sa.sa_handler = on_signal;
        sigaction(SIGWINCH, &sa, nullptr);
        sigaction(SIGTERM, &sa, nullptr);
        sigaction(SIGHUP, &sa, nullptr);
        signal(SIGPIPE, SIG_IGN);

        cwd = !opts.cwd.empty() ? opts.cwd : fs::current_path().string();
        hist.load();
        enter_raw();

        {
            auto r = api.get("/api/models");
            for (auto & x : r.body.value("models", json::array())) models.push_back(jstr(x, "name"));
        }
        stream.commit({"", pal.accent + pal.bold + "Judas" + pal.reset + pal.dim + "  " + jstr(api.get("/health").body, "model") + "  " + cwd + pal.reset,
                       pal.dim + std::string("/help for commands, ! for a shell, Esc to interrupt, Ctrl+C twice to quit") + pal.reset});
        std::thread poller(poll_thread, net, cfg.server.port);
        std::string target = opts.session;
        if (target.empty() && opts.resume_last) {
            auto r = api.get("/api/sessions?limit=1");
            if (r.body.contains("sessions") && !r.body["sessions"].empty()) target = jstr(r.body["sessions"][0], "id");
        }
        if (!target.empty()) open_session(target);
        repaint();

        double next_tick = 0;
        while (!quit && !g_term) {
            int timeout = dec.pending() ? 40 : (stream.running() ? 100 : 1000);
            pollfd pf[2] = {{STDIN_FILENO, POLLIN, 0}, {wake_r, POLLIN, 0}};
            int pr = poll(pf, 2, timeout);
            bool dirty = false;
            if (pr > 0 && (pf[1].revents & POLLIN)) {
                char b[256];
                while (read(wake_r, b, sizeof b) > 0) {}
                dirty = true;
            }
            if (g_winch) { g_winch = 0; measure(); dirty = true; }
            if (pr > 0 && (pf[0].revents & (POLLIN | POLLHUP))) {
                char b[4096];
                ssize_t n = read(STDIN_FILENO, b, sizeof b);
                if (n <= 0) { quit = true; break; }
                for (auto & k : dec.feed(std::string(b, (size_t) n))) on_key(k);
                dirty = true;
            } else if (dec.pending()) {
                for (auto & k : dec.flush_esc()) on_key(k);
                dirty = true;
            }
            drain_events();
            double t = now_s();
            if (stream.running() && t >= next_tick) { spin++; next_tick = t + 0.12; dirty = true; }
            if (!flash.empty() && t >= flash_until) { flash.clear(); dirty = true; }
            if (pr == 0) dirty = true;
            if (dirty && !quit) repaint();
        }
        // leave: clear the live area, say how to come back
        net->quit = true;
        stop_follow();
        std::string f;
        if (region_cursor_row > 0) f += "\033[" + std::to_string(region_cursor_row) + "A";
        f += "\r\033[J";
        for (auto & r : stream.take_committed()) f += r + pal.reset + "\r\n";
        if (!sid.empty()) f += pal.dim + std::string("session ") + sid + "  (judas --session " + sid.substr(0, 8) + " to continue)" + pal.reset + "\r\n";
        out(f);
        leave_raw();
        poller.join();
        return 0;
    }

};

}  // namespace
}  // namespace judas

namespace judas {

int run_tui(const Config & cfg, const TuiOptions & opts) {
    {
        httplib::Client hc("127.0.0.1", cfg.server.port);
        hc.set_connection_timeout(2, 0);
        hc.set_read_timeout(3, 0);
        auto r = hc.Get("/health");
        bool ok = false;
        if (r && r->status == 200) {
            auto j = json::parse(r->body, nullptr, false);
            ok = !j.is_discarded() && j.is_object() && jstr(j, "status") == "ok";
        }
        if (!ok) {
            std::fprintf(stderr, "judas daemon not running on port %d. start it with: judas serve (or use judas --local)\n", cfg.server.port);
            return 1;
        }
    }
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
        std::fprintf(stderr, "judas: the TUI needs a terminal. use judas run \"task\" for scripts.\n");
        return 2;
    }
    App app(cfg, opts);
    return app.run();
}

}  // namespace judas
