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
#include "core/skin.h"
#include "cpp-httplib/httplib.h"
#include "learning/learn.h"
#include "tui/approvals.h"
#include "tui/attach.h"
#include "tui/commands.h"
#include "tui/input.h"
#include "tui/layout.h"
#include "tui/snapshots.h"
#include "tui/sprites.h"
#include "tui/team.h"
#include "tui/theme.h"
#include "tui/text.h"
#include "tui/view.h"
#ifdef JUDAS_TUI_PLANNER
#include "tui_planner.h"
#endif

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
    std::string raw;  // the response body as sent (plain-text endpoints such as a snapshot diff)
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
        out.raw = r->body;
        auto j = json::parse(r->body, nullptr, false);
        out.body = j.is_discarded() ? json::object() : j;
        return out;
    }
    // Raw body upload (attachments) or a slow call (screenshot with a region picker): own read timeout.
    Reply post_raw(const std::string & path, const std::string & body, const std::string & ctype, int read_timeout) {
        httplib::Client c("127.0.0.1", port);
        c.set_connection_timeout(2, 0);
        c.set_read_timeout(read_timeout, 0);
        c.set_write_timeout(read_timeout, 0);
        auto r = c.Post(path, body, ctype);
        Reply out;
        if (!r) return out;
        out.status = r->status;
        out.raw = r->body;
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

// Runs a shell command and returns its stdout bytes (stderr is the caller's business).
std::string run_capture(const std::string & cmd, size_t max_bytes = 64u << 20) {
    FILE * p = popen(cmd.c_str(), "r");
    if (!p) return "";
    std::string data;
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, p)) > 0 && data.size() < max_bytes) data.append(buf, n);
    pclose(p);
    return data;
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
    ApprovalQueue appr;                 // pending approvals for this session (src/tui/approvals.h)
    std::string appr_announced;         // id of the request already printed
    std::vector<std::string> appr_body; // full body rows of the current request, for the pager
    bool appr_viewing = false;
    size_t appr_pos = 0;
    Editor ed;
    History hist;
    KeyDecoder dec;
    std::string cwd, sid, mode = "chat", flash;
    double flash_until = 0, last_ctrlc = 0, turn_start = 0, last_esc = 0;
    bool quit = false, dismissed = false;
    std::string dismissed_text, last_menu_text;
    int menu_sel = 0, spin = 0, rows = 24, cols = 80, region_cursor_row = 0, region_rows = 0;
    struct Queued {
        std::string text;
        std::vector<std::string> ids, chips;
    };
    std::vector<Queued> queued;
    std::vector<std::string> models;
    // attachments waiting for the next message, upload errors, the pixel companions
    std::vector<PendingAttachment> pending;
    std::vector<std::string> attach_errs;
    Companions comp;
    bool mascot_on = false;
    double sprite_next = 0, last_paint = 0;
    int panel_n = 0, last_cursor_col = 0;
    // generic picker (sessions, models)
    Menu picker;
    std::string picker_kind;
    int wake_r = -1, wake_w = -1;
    // Swarm team view: /team watch keeps a live block in the live area; /cancel-agent asks y/n
    bool team_watch = false, watch_dirty = false;
    TeamView watch_view;
    double watch_fetched = 0;
    std::string cancel_agent, cancel_label;
    termios orig{};
    bool raw = false;
    // snapshots, revert confirmation, diff pager, theme (tui/snapshots.cpp, tui/theme.cpp)
    ThemeLook look;
    std::string theme_name, term_bg, revert_id;
    bool revert_to = false;
    std::vector<std::string> pager;
    size_t pager_pos = 0;

    App(const Config & c, const TuiOptions & o)
        : cfg(c), opts(o), api{c.server.port}, pal(Palette::from_skin(std::getenv("JUDAS_SKIN") ? std::getenv("JUDAS_SKIN") : "judas")),
          stream(pal, 80), hist(expand_home("~/.local/share/judas/tui_history")) { init_theme(); }

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
        if (appr.current() || !cancel_agent.empty() || !revert_id.empty() || !pager.empty()) return Menu();
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
        double tnow = now_s();
        panel_n = 0;
        if (mascot_visible()) {
            auto prow = comp.render(tnow, stream.running(), pal.dim, pal.reset);
            std::string pad((size_t) std::max(0, cols - 1 - comp.panel_width()), ' ');
            for (auto & r : prow) rowsv.push_back(pad + r);
            panel_n = (int) prow.size();
            sprite_next = tnow + comp.frame_seconds(tnow, stream.running());
        }
        Menu menu = current_menu();
        BoxView box = render_input_box(ed, cols - 1, pal, "Message Judas  (/ commands, ! shell, @ files)",
                                       !ed.text.empty() && ed.text[0] == '!', 8);
        std::vector<std::string> below;
        // priority: pending approval, /cancel-agent y/n, revert y/n, snapshot pager, menu
        if (appr.current())
            below.push_back(approval_prompt(pal, appr.size() - 1, appr_viewing, appr_pos, appr_body.size()));
        else if (!cancel_agent.empty()) below.push_back(pal.warn + std::string("  Cancel ") + truncate_width(cancel_label, cols - 30) + "?  [y] yes  [n] no" + pal.reset);
        else if (!revert_id.empty()) below.push_back(pal.warn + std::string("  Revert ") + revert_id + (revert_to ? " and everything after it" : "") + "?  [y] yes  [any other key] cancel" + pal.reset);
        else if (!pager.empty()) below.push_back(pal.dim + std::string("  -- ") + std::to_string(pager.size() - pager_pos) + " more lines: Enter or space for the next page, q to stop --" + pal.reset);
        else if (menu.active()) below = render_menu(menu, cols - 1, pal, 8);
        else if (!flash.empty() && now_s() < flash_until) below.push_back(pal.warn + std::string(" ") + flash + pal.reset);
        else below.push_back(status_row());
        auto chips = render_chip_rows(pending, cols - 1, pal);
        for (auto & e : attach_errs) chips.push_back(pal.err + std::string("  ! ") + truncate_width(one_line(e), cols - 8) + pal.reset);
        if (vision_hint()) chips.push_back(pal.warn + std::string("  this model cannot see images; Judas will use OCR text") + pal.reset);
        int reserve = (int) box.rows.size() + (int) below.size() + (int) queued.size() + (int) chips.size() + 1 + panel_n;
        auto live = stream.live(spin, turn_start > 0 ? now_s() - turn_start : 0);
        int room = std::max(1, rows - reserve - 1);
        if (team_watch) {
            auto wr = watch_rows(room);
            for (auto & l : wr) rowsv.push_back(l);
            room = std::max(1, room - (int) wr.size());
        }
        if ((int) live.size() > room) live.erase(live.begin(), live.end() - room);
        for (auto & l : live) rowsv.push_back(l);
        for (auto & q : queued) rowsv.push_back(pal.dim + std::string("  queued: ") + truncate_width(one_line(q.text.empty() ? "(attachments)" : q.text), cols - 14) + pal.reset);
        for (auto & c : chips) rowsv.push_back(c);
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
        last_cursor_col = box.cursor_col;
        last_paint = tnow;
        out(f);
    }

    // ---- companions -----------------------------------------------------------------------------------------
    bool mascot_visible() const {
        return mascot_on && comp.ready() && cols >= 100 && rows >= 30 && comp.panel_width() * 2 <= cols;
    }
    // Redraw only the sprite rows (the top of the live area) and put the cursor back: no flicker, no full repaint.
    void repaint_sprites() {
        if (!mascot_visible() || panel_n == 0 || region_cursor_row < panel_n - 1) return;
        double t = now_s();
        auto prow = comp.render(t, stream.running(), pal.dim, pal.reset);
        if ((int) prow.size() != panel_n) return;
        int col = std::max(0, cols - 1 - comp.panel_width()) + 1;
        std::string f = "\033[?2026h\033[?25l";
        if (region_cursor_row > 0) f += "\033[" + std::to_string(region_cursor_row) + "A";
        for (int i = 0; i < panel_n; i++) {
            f += "\033[" + std::to_string(col) + "G" + prow[i];
            if (i + 1 < panel_n) f += "\033[B";
        }
        int delta = region_cursor_row - (panel_n - 1);
        if (delta > 0) f += "\033[" + std::to_string(delta) + "B";
        else if (delta < 0) f += "\033[" + std::to_string(-delta) + "A";
        f += "\r";
        if (last_cursor_col > 0) f += "\033[" + std::to_string(last_cursor_col) + "C";
        f += "\033[?25h\033[?2026l";
        sprite_next = t + comp.frame_seconds(t, stream.running());
        out(f);
    }

    bool vision_hint() {
        json st = state();
        if (!st.contains("vision") || !st["vision"].is_boolean() || st["vision"].get<bool>()) return false;
        for (auto & a : pending) if (a.kind == "image") return true;
        return false;
    }

    void load_companions() {
        auto r = api.get("/sprites.json");
        if (r.status != 200 || !comp.load(r.body)) return;  // no companions: nothing to show
        auto st = api.get("/api/state");
        bool def = true;
        if (st.status == 200 && st.body.contains("ui") && st.body["ui"].is_object()) def = st.body["ui"].value("mascot", true);
        mascot_on = def;
    }

    // ---- attachments ----------------------------------------------------------------------------------------
    void add_uploaded(const Reply & r, const std::string & name) {
        if (r.status != 200 || jstr(r.body, "id").empty()) {
            attach_errs.push_back(name + ": " + (r.status == 501 ? "not available on the host (grim missing?)" : err_text(r)));
            comp.error(now_s());
            return;
        }
        PendingAttachment a;
        a.id = jstr(r.body, "id");
        a.name = jstr(r.body, "name").empty() ? name : jstr(r.body, "name");
        a.kind = jstr(r.body, "kind");
        a.bytes = r.body.value("bytes", 0LL);
        a.text_chars = r.body.value("text_chars", 0LL);
        pending.push_back(a);
    }

    void upload_bytes(const std::string & name, const std::string & data, const std::string & mime) {
        if (data.empty()) { attach_errs.push_back(name + ": empty"); return; }
        set_flash("uploading " + name + " (" + human_bytes((long long) data.size()) + ")...", 60);
        repaint();
        auto r = api.post_raw("/api/attachments?name=" + url_encode(name), data, mime, 180);
        flash.clear();
        add_uploaded(r, name);
    }

    void attach_file(const std::string & path) {
        std::error_code ec;
        auto sz = fs::file_size(path, ec);
        std::string name = fs::path(path).filename().string();
        if (ec) { attach_errs.push_back(name + ": cannot read"); return; }
        if (sz > (256u << 20)) { attach_errs.push_back(name + ": larger than 256 MB"); return; }
        std::ifstream f(path, std::ios::binary);
        if (!f) { attach_errs.push_back(name + ": cannot read"); return; }
        std::string data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        upload_bytes(name, data, guess_mime(name));
    }

    void attach_paths(const std::vector<std::string> & paths) {
        attach_errs.clear();
        for (auto & p : paths) attach_file(p);
    }

    void do_attach(const std::string & arg) {
        attach_errs.clear();
        if (arg.empty()) { say("usage: /attach <path> [more paths]   (quote paths with spaces)", 'w'); return; }
        std::vector<std::string> missing;
        auto paths = parse_attach_args(arg, cwd, &missing);
        for (auto & m : missing) attach_errs.push_back(m + ": no such file");
        for (auto & p : paths) attach_file(p);
    }

    void do_screenshot(const std::string & arg) {
        attach_errs.clear();
        bool region = arg == "region";
        if (!arg.empty() && !region) { say("usage: /screenshot [region]", 'w'); return; }
        set_flash(region ? "pick an area on the screen..." : "taking a screenshot...", 120);
        repaint();
        auto r = api.post_raw("/api/screenshot", json{{"region", region}}.dump(), "application/json", region ? 180 : 30);
        flash.clear();
        add_uploaded(r, "screenshot");
    }

    void do_paste_clipboard() {
        attach_errs.clear();
        std::string types = run_capture("timeout 3 wl-paste --list-types 2>/dev/null", 65536);
        if (types.empty()) { attach_errs.push_back("clipboard is empty, or wl-paste is not installed"); return; }
        std::string img, txt;
        size_t pos = 0;
        while (pos < types.size()) {
            size_t nl = types.find('\n', pos);
            std::string t = types.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? types.size() : nl + 1;
            if (t.rfind("image/", 0) == 0 && (img.empty() || t == "image/png")) img = t;
            if (txt.empty() && (t.rfind("text/plain", 0) == 0 || t == "UTF8_STRING" || t == "STRING")) txt = t;
        }
        if (!img.empty()) {
            std::string data = run_capture("timeout 10 wl-paste --type " + sh_quote(img) + " 2>/dev/null");
            std::string ext = img.substr(6);
            if (ext == "jpeg") ext = "jpg";
            if (ext.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") != std::string::npos) ext = "png";
            upload_bytes("clipboard." + ext, data, img);
        } else if (!txt.empty()) {
            std::string data = run_capture("timeout 5 wl-paste --no-newline --type " + sh_quote(txt) + " 2>/dev/null", 4u << 20);
            if (data.empty()) { attach_errs.push_back("clipboard text is empty"); return; }
            handle_paste(data);
        } else {
            attach_errs.push_back("the clipboard holds nothing attachable (" + truncate_width(one_line(types), 60) + ")");
        }
    }

    void do_detach(const std::string & arg) {
        attach_errs.clear();
        if (pending.empty()) { say("no pending attachments"); return; }
        if (arg.empty()) { pending.clear(); say("removed all pending attachments"); return; }
        int n = std::atoi(arg.c_str());
        if (n < 1 || n > (int) pending.size()) { say("/detach N: N is 1 to " + std::to_string(pending.size()), 'w'); return; }
        say("removed " + pending[n - 1].name);
        pending.erase(pending.begin() + (n - 1));
    }

    // A bracketed paste: file paths (a dropped file) become attachments, anything else goes into the input.
    void handle_paste(const std::string & text) {
        auto paths = detect_paths(text, cwd);
        if (!paths.empty()) { attach_paths(paths); return; }
        ed.paste(text);
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
        appr.clear();
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
        appr.clear();
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
        appr_refresh();
        appr_announce();
        return true;
    }

    bool send_text(const std::string & shown, const std::string & full, const std::vector<std::string> & ids = {},
                   const std::vector<std::string> & chips = {}) {
        if (stream.running()) {
            queued.push_back({full, ids, chips});
            return true;
        }
        json body = {{"content", full}, {"cwd", cwd}, {"mode", mode}};
        if (!ids.empty()) body["attachments"] = ids;
        auto r = api.call("POST", "/api/sessions/" + (sid.empty() ? std::string("new") : sid) + "/messages", body.dump());
        if (r.status != 200) { say("send failed: " + err_text(r), 'e'); comp.error(now_s()); return false; }
        std::string got = r.body["session"].is_string() ? r.body["session"].get<std::string>() : jstr(r.body["session"], "id");
        stream.user_message(shown, chips);
        stream.set_running(true);
        turn_start = now_s();
        if (sid.empty() || sid != got) {
            sid = got;
            stop_follow();
            start_follow(-1);
        }
        return true;
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
        say("Attachments: /attach path, /screenshot [region], /paste (clipboard), /detach [n]; dropping a file into the terminal attaches it.");
        say("  Pending attachments show above the input and are sent with the next message (the text may be empty).");
        say("Snapshots: /snapshots [N] lists recent edits, /snapshots show ID prints a diff, /revert ID [--to] undoes one (asks y first).");
        say("Themes: /theme picks a color theme live (kept in ~/.config/judas/tui.toml); light themes change text colors only.");
        say("Companions: /mascot toggles the pixel companions (terminals at least 100 columns by 30 rows).");
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

    // ---- Swarm team view (src/tui/team.h) -----------------------------------------------------------------------
    bool fetch_team(TeamView & v) {
        if (sid.empty()) { say("no session yet"); return false; }
        auto r = api.get("/api/sessions/" + sid + "/team");
        if (r.status != 200) { say("team: " + err_text(r), 'e'); return false; }
        v = parse_team(r.body);
        return true;
    }
    std::vector<std::string> watch_rows(int room) {
        auto rowsv = render_team(watch_view, pal, cols - 1);
        std::vector<std::string> o;
        o.push_back(pal.dim + std::string("  team watch  (Esc to stop)") + pal.reset);
        for (auto & r : rowsv) o.push_back(r);
        size_t cap = (size_t) std::max(4, room - 3);
        if (o.size() > cap) { o.resize(cap - 1); o.push_back(pal.dim + std::string("   ... more rows than fit; /team prints all") + pal.reset); }
        return o;
    }
    void stop_team_watch() {
        if (!team_watch) return;
        team_watch = false;
        stream.commit(render_team(watch_view, pal, cols - 1));
        say("team watch stopped");
    }
    void team_event(const json & e) {
        if (auto note = handoff_note(e)) {
            if (mode == "swarm" || team_watch) say("  " + *note);
        }
        std::string t = jstr(e, "type");
        if (team_watch && (t == "activity" || t == "team" || t == "spawned" || t == "done")) watch_dirty = true;
    }
    void team_tick() {
        if (!team_watch) return;
        if (!stream.running()) { stop_team_watch(); return; }
        double t = now_s();
        if ((watch_dirty && t - watch_fetched >= 0.4) || t - watch_fetched >= 2.0) {
            auto r = api.get("/api/sessions/" + sid + "/team");
            if (r.status == 200) { watch_view = parse_team(r.body); }
            watch_fetched = t;
            watch_dirty = false;
        }
    }
    void confirm_cancel_agent(bool yes) {
        std::string id = cancel_agent, label = cancel_label;
        cancel_agent.clear();
        cancel_label.clear();
        if (!yes) { say("cancel-agent: not cancelled"); return; }
        auto r = api.post("/api/sessions/" + sid + "/agents/" + id + "/cancel");
        say(r.status == 200 ? "cancelled " + label : "cancel-agent: " + err_text(r), r.status == 200 ? 'd' : 'e');
    }
    void team_cmd(const Parsed & p) {
        const std::string & n = p.name;
        if (n == "team") {
            if (p.arg == "watch") {
                if (team_watch) { stop_team_watch(); return; }
                if (!fetch_team(watch_view)) return;
                if (!stream.running()) {
                    say("no turn is running; here is the team once (/team watch follows a running turn)");
                    stream.commit(render_team(watch_view, pal, cols - 1));
                    return;
                }
                team_watch = true;
                watch_fetched = now_s();
                return;
            }
            if (!p.arg.empty()) { say("usage: /team [watch]", 'w'); return; }
            TeamView v;
            if (!fetch_team(v)) return;
            stream.commit(render_team(v, pal, cols - 1));
            return;
        }
        std::string key;
        size_t num = 0;
        split_key_arg(p.arg, key, num);
        if (key.empty()) { say("usage: /" + n + " N|copy" + (n == "read" ? " [chars]" : "") + "   (N is the row number from /team)", 'w'); return; }
        TeamView v;
        if (!fetch_team(v)) return;
        const TeamRow * r = find_row(v, key);
        if (!r || r->agent.empty()) { say(r ? "that copy has no agent yet" : "no such agent: " + key + "  (see /team)", 'w'); return; }
        std::string label = r->copy.empty() ? r->agent : r->copy;
        if (n == "read") {
            std::string text = agent_output_text(*r, num ? num : 4000);
            if (text.empty()) { say(label + " has no output yet"); return; }
            say(label + " (" + team_word(r->activity) + "):", 'n');
            size_t pos = 0;
            while (pos <= text.size()) {
                size_t nl = text.find('\n', pos);
                stream.commit({"  " + text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos)});
                if (nl == std::string::npos) break;
                pos = nl + 1;
            }
        } else if (n == "cancel-agent") {
            cancel_agent = r->agent;
            cancel_label = label;
        } else {
            auto rep = api.post("/api/sessions/" + sid + "/agents/" + r->agent + "/" + n);
            say(rep.status == 200 ? (n == "pause" ? "paused " : "resumed ") + label : n + ": " + err_text(rep), rep.status == 200 ? 'd' : 'e');
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

    // /coach: propose from this session, or list / show / adopt / reject proposals. Nothing applies until adopted.
    void coach_cmd(const std::string & arg) {
        std::string sub = arg, rest;
        if (auto sp = arg.find(' '); sp != std::string::npos) { sub = arg.substr(0, sp); rest = arg.substr(sp + 1); }
        if (sub == "list") {
            auto r = api.get("/api/coach");
            if (r.status != 200) { say("coach: " + err_text(r), 'e'); return; }
            if (!r.body.value("enabled", false)) say("coach is disabled; set [coach] enabled = true in config.toml", 'w');
            int n = 0;
            for (auto & v : r.body["variants"]) {
                say(v.value("id", "") + "  " + v.value("status", "") + "  " + std::to_string(v.value("edit_count", 0)) + " edit(s)", 'n');
                n++;
            }
            for (auto & j : r.body["jobs"])
                if (j.value("state", "") == "running") say("running: " + j.value("session", ""), 'd');
            if (!n) say("no coach proposals yet");
        } else if (sub == "show" || sub == "adopt" || sub == "reject") {
            if (rest.empty()) { say("usage: /coach " + sub + " ID", 'w'); return; }
            if (sub == "show") {
                auto r = api.get("/api/coach/" + rest);
                if (r.status != 200) { say("coach: " + err_text(r), 'e'); return; }
                say(r.body.value("status", "") + ": " + r.body.value("summary", ""), 'n');
                for (auto & e : r.body["edits"]) say("  " + e.value("kind", "") + " " + e.value("name", "") + ": " + e.value("rationale", ""));
                for (auto & d : r.body["discarded"]) say("  discarded " + d.value("what", "") + ": " + d.value("reason", ""), 'w');
                say("files: " + r.body.value("dir", ""), 'd');
            } else {
                auto r = api.post("/api/coach/" + rest + "/" + sub);
                if (r.status != 200) { say("coach: " + err_text(r), 'e'); return; }
                say(sub == "adopt" ? "adopted " + rest : "rejected " + rest);
            }
        } else {
            std::string target = arg.empty() ? sid : arg;
            if (target.empty()) { say("send a message first, or give a session id: /coach SESSION", 'w'); return; }
            auto r = api.post("/api/coach", {{"session", target}});
            if (r.status == 202) say("coach is reading " + target + " in the background; see /coach list");
            else say("coach: " + err_text(r), 'e');
        }
    }


    // ---- theme ----------------------------------------------------------------------------------------------
    void init_theme() {
        term_bg = terminal_bg_from_colorfgbg(std::getenv("COLORFGBG"));
        auto skins = load_skins();
        std::string want = load_theme_pref(tui_prefs_path());
        if (want.empty() && std::getenv("JUDAS_SKIN")) want = std::getenv("JUDAS_SKIN");
        theme_name = resolve_theme(skins, want);
        if (theme_name.empty()) theme_name = "judas";
        look = look_for(skin_by_name(theme_name), term_bg);
        pal = look.pal;
        stream.set_palette(pal);
    }

    void apply_theme(const std::string & name) {
        Skin sk = skin_by_name(name);
        look = look_for(sk, term_bg);
        pal = look.pal;
        stream.set_palette(pal);
        theme_name = sk.name;
        bool saved = save_theme_pref(tui_prefs_path(), sk.name);
        say("theme: " + sk.label + (sk.dark ? " (dark)" : " (light)") + (saved ? ", saved to " + tui_prefs_path() : "; could not save " + tui_prefs_path()), saved ? 'd' : 'w');
        if (!look.warning.empty()) say(look.warning, 'w');
    }

    void theme_cmd(const std::string & arg) {
        auto skins = load_skins();
        if (arg == "sync") {
            say("/theme sync is not available: the web app keeps its theme choice in the browser, and the server only reports the JUDAS_SKIN default (GET /api/skins/current).", 'w');
            return;
        }
        if (!arg.empty()) {
            std::string n = resolve_theme(skins, arg);
            if (n.empty()) { say("no theme matches \"" + arg + "\"; /theme lists them", 'w'); return; }
            apply_theme(n);
            return;
        }
        Menu m;
        m.kind = Menu::Picker;
        int cur = 0;
        for (auto & t : list_themes(skins)) {
            if (t.name == theme_name) cur = (int) m.items.size();
            m.items.push_back({t.name, t.name + (t.name == theme_name ? "  (current)" : ""), (t.dark ? "dark" : "light") + std::string("  ") + t.label});
        }
        picker = m;
        picker_kind = "theme";
        menu_sel = cur;
    }

    // ---- snapshots ------------------------------------------------------------------------------------------
    // The current session's snapshots, or (no session yet) those touching files under the working folder.
    std::vector<SnapInfo> fetch_snaps(size_t limit, bool all, std::string * err) {
        std::string path = "/api/snapshots?limit=" + std::to_string(sid.empty() || all ? 500 : limit * 2 + 50);
        if (!sid.empty() && !all) path += "&session=" + sid;
        auto r = api.get(path);
        if (r.status != 200) { *err = err_text(r); return {}; }
        auto v = parse_snapshots(r.body["snapshots"]);
        if (sid.empty() && !all) v = in_folder(v, cwd);
        return v;
    }
    long long wall_ms() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }
    bool pick_snap(const std::string & q, SnapInfo & out) {
        std::string err;
        auto v = fetch_snaps(200, false, &err);
        std::string id = resolve_snapshot_id(v, q);
        if (id.empty()) { v = fetch_snaps(200, true, &err); id = resolve_snapshot_id(v, q); }
        if (!err.empty() && id.empty()) { say("snapshots: " + err, 'e'); return false; }
        if (id.empty()) { say("no single snapshot matches \"" + q + "\"; /snapshots lists them", 'w'); return false; }
        for (auto & s : v) if (s.id == id) { out = s; return true; }
        return false;
    }

    void snapshots_cmd(const std::string & arg) {
        if (arg.rfind("show", 0) == 0 && (arg.size() == 4 || arg[4] == ' ')) {
            std::string q = arg.size() > 4 ? arg.substr(5) : "";
            while (!q.empty() && q.front() == ' ') q.erase(q.begin());
            if (q.empty()) { say("usage: /snapshots show ID", 'w'); return; }
            SnapInfo s;
            if (!pick_snap(q, s)) return;
            auto r = api.get("/api/snapshots/" + s.id + "/diff");
            if (r.status != 200) { say("diff: " + err_text(r), 'e'); return; }
            stream.commit({"", pal.accent + pal.bold + std::string("Snapshot ") + pal.reset + pal.text + s.id + pal.reset + pal.dim + "  " + truncate_width(one_line(s.label), cols - 30) + pal.reset});
            if (r.raw.empty()) { say("no differences: the files already match this snapshot's before-state (reverted or unchanged)"); return; }
            pager = render_diff(r.raw, cols - 1, look);
            pager_pos = 0;
            pager_next();
            return;
        }
        int n = 10;
        if (!arg.empty()) {
            n = std::atoi(arg.c_str());
            if (n < 1) { say("usage: /snapshots [N]  or  /snapshots show ID", 'w'); return; }
            n = std::min(n, 40);
        }
        std::string err;
        auto v = fetch_snaps((size_t) n, false, &err);
        if (!err.empty()) { say("snapshots: " + err, 'e'); return; }
        if ((int) v.size() > n) v.resize((size_t) n);
        for (auto & s : v) {
            auto r = api.get("/api/snapshots/" + s.id + "/diff");
            if (r.status == 200) { DiffStat st = diff_stat(r.raw); s.added = st.added; s.removed = st.removed; }
        }
        stream.commit(render_snapshot_list(v, wall_ms(), cols - 1, pal, sid.empty() ? "folder " + cwd : "session " + sid.substr(0, 8)));
    }

    // Prints the next page of the pending diff; keeps the pager open while rows remain.
    void pager_next() {
        size_t page = (size_t) std::max(5, rows - 6), next = 0;
        stream.commit(page_slice(pager, pager_pos, page, &next));
        pager_pos = next;
        if (pager_pos >= pager.size()) { pager.clear(); pager_pos = 0; }
    }

    void revert_cmd(const std::string & arg) {
        std::string q = arg;
        bool to = false;
        size_t k = q.find("--to");
        if (k != std::string::npos) { to = true; q.erase(k, 4); }
        while (!q.empty() && q.back() == ' ') q.pop_back();
        while (!q.empty() && q.front() == ' ') q.erase(q.begin());
        if (q.empty()) { say("usage: /revert ID [--to]   (ids: /snapshots)", 'w'); return; }
        SnapInfo s;
        if (!pick_snap(q, s)) return;
        auto r = api.get("/api/snapshots/" + s.id + "/diff");
        DiffStat st = r.status == 200 ? diff_stat(r.raw) : DiffStat();
        stream.commit(render_revert_preview(s, st, to, cols - 1, pal));
        if (stream.running()) say("  a turn is running; edits it makes after this are not covered by the revert", 'w');
        revert_id = s.id;
        revert_to = to;
    }

    void answer_revert(bool yes) {
        std::string id = revert_id;
        bool to = revert_to;
        revert_id.clear();
        if (!yes) { say("revert cancelled"); return; }
        auto r = api.post("/api/snapshots/" + id + "/revert", to ? json{{"to", true}} : json::object());
        if (r.status != 200) { say("revert: " + err_text(r), 'e'); return; }
        stream.commit(render_revert_result(r.body, cols - 1, pal));
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

#ifdef JUDAS_TUI_PLANNER
    planner::Get planner_get() {
        return [this](const std::string & path) { auto r = api.get(path); return std::make_pair(r.status, r.body); };
    }
    // Show the day, then start a chat turn asking the lead to plan it (a new chat session if this one is not chat or cowork).
    void plan_day(const std::string & arg) {
        std::string date, wish;
        planner::split_plan_arg(arg, planner::today_local(), date, wish);
        bool ok = false;
        auto rows = planner::day_rows_for_plan(date, planner_get(), pal, cols, &ok);
        stream.commit(rows);
        if (!ok) return;
        if (mode != "chat" && mode != "cowork") {
            if (!sid.empty()) new_session();
            mode = "chat";
        }
        send_text(planner::plan_label(date, wish), planner::plan_prompt(date, wish));
    }
#endif

    void run_slash(const Parsed & p) {
        const std::string & n = p.name;
        if (!find_command(n)) { say("unknown command /" + n + "  (try /help)", 'w'); return; }
        if (n == "help") help();
        else if (n == "new") new_session();
        else if (n == "sessions") list_sessions();
        else if (n == "resume") {
            if (!p.arg.empty()) { team_cmd(p); return; }   // /resume N|copy: a paused agent
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
            if (p.arg.empty()) { say("mode: " + mode + "  (/mode chat|cowork|code|learn|swarm)"); return; }
            if (p.arg != "chat" && p.arg != "cowork" && p.arg != "code" && p.arg != "learn" && p.arg != "swarm") { say("mode must be chat, cowork, code, learn or swarm", 'w'); return; }
            mode = p.arg;
            if (!sid.empty()) {
                auto r = api.patch("/api/sessions/" + sid, {{"mode", mode}});
                if (r.status != 200) { say("mode: " + err_text(r), 'e'); return; }
            }
            say("mode: " + mode);
        } else if (n == "model") list_models(p.arg);
        else if (n == "agents") show_agents();
        else if (n == "approvals") show_approvals();
        else if (n == "team" || n == "pause" || n == "cancel-agent" || n == "read") team_cmd(p);
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
        else if (n == "coach") coach_cmd(p.arg);
        else if (n == "snapshots") snapshots_cmd(p.arg);
        else if (n == "revert") revert_cmd(p.arg);
        else if (n == "theme") theme_cmd(p.arg);
        else if (n == "attach") do_attach(p.arg);
        else if (n == "screenshot") do_screenshot(p.arg);
        else if (n == "paste") do_paste_clipboard();
        else if (n == "detach") do_detach(p.arg);
        else if (n == "mascot") {
            if (!comp.ready()) { say("no companions available (the daemon did not serve /sprites.json)", 'w'); return; }
            mascot_on = !mascot_on;
            say(mascot_on ? (mascot_visible() ? "companions on" : "companions on (they show in terminals of at least 100 columns by 30 rows)") : "companions off");
#ifdef JUDAS_TUI_PLANNER
        } else if (n == "day" || n == "week") {
            stream.commit(planner::run(n, p.arg, planner_get(), pal, cols));
        } else if (planner::is_plan_command(n)) {
            plan_day(p.arg);
#endif
        } else if (n == "quit") quit = true;
    }

    void submit() {
        std::string visible = ed.text, full = ed.expanded();
        // typed or dropped input that is exactly file paths attaches them
        {
            auto paths = detect_paths(full, cwd);
            if (!paths.empty()) { ed.clear(); attach_paths(paths); return; }
        }
        Parsed p = parse_input(visible);
        if (p.kind == Parsed::Empty) {
            if (pending.empty() || visible.find_first_not_of(" \t\r\n") != std::string::npos) return;
            ed.clear();
            send_with_pending("", "");
            return;
        }
        hist.add(visible.size() > 4000 ? visible : full.size() < 20000 ? full : visible);
        ed.clear();
        if (p.kind == Parsed::Slash) run_slash(p);
        else if (p.kind == Parsed::Shell) run_shell(p.arg);
        else send_with_pending(visible, full);
    }

    void send_with_pending(const std::string & visible, const std::string & full) {
        attach_errs.clear();
        std::vector<std::string> ids, chips;
        for (auto & a : pending) {
            ids.push_back(a.id);
            chips.push_back(a.kind == "image" ? std::string("\xF0\x9F\x96\xBC ") + a.name
                                              : std::string("\xF0\x9F\x93\x8E ") + a.name + " (" + a.kind + ", " + std::to_string(a.text_chars) + " chars)");
        }
        auto keep = pending;
        pending.clear();
        if (!send_text(visible, full, ids, chips)) pending = keep;
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

    // ---- approvals (src/tui/approvals.h; protocol in docs/API.md, Approvals) -------------------------------
    // Print the request at the front of the queue once; later ones print when they reach the front.
    void appr_announce() {
        const ApprovalReq * c = appr.current();
        if (!c) { appr_announced.clear(); appr_viewing = false; return; }
        if (c->id == appr_announced) return;
        appr_announced = c->id;
        appr_viewing = false;
        appr_pos = 0;
        appr_body = approval_body_rows(*c, pal, cols - 1);
        stream.commit(approval_rows(*c, pal, cols - 1));
    }
    void appr_refresh() {
        if (sid.empty()) { appr.clear(); return; }
        auto r = api.get("/api/sessions/" + sid + "/approvals");
        if (r.status != 200 || !r.body.contains("approvals") || !r.body["approvals"].is_array()) return;
        std::vector<ApprovalReq> v;
        for (auto & j : r.body["approvals"]) v.push_back(approval_from_json(j));
        appr.sync(v);
    }
    void appr_event(const json & e) {
        const std::string type = jstr(e, "type");
        if (type == "approval_request") appr.add(approval_from_json(e));
        else if (type == "approval_resolved") {
            const json d = e.contains("data") && e["data"].is_object() ? e["data"] : e;
            if (appr.remove(jstr(d, "id"))) {
                bool yes = d.contains("approved") && d["approved"].is_boolean() && d["approved"].get<bool>();
                std::string by = jstr(d, "by");
                say(std::string("  approval ") + (yes ? "approved" : "declined") + (by.empty() || by == "user" ? " elsewhere" : " (" + by + ")"), 'd');
            }
        } else if (type == "gap") appr_refresh();
    }
    void appr_answer(bool yes) {
        const ApprovalReq * c = appr.current();
        if (!c || sid.empty()) return;
        ApprovalReq req = *c;
        auto r = api.post("/api/sessions/" + sid + "/approvals/" + req.id, {{"approve", yes}});
        if (r.status != 200 && r.status != 404 && r.status != 409) { set_flash("could not send the answer: " + err_text(r), 4); return; }
        appr.remove(req.id);
        if (r.status == 200) {
            std::string first = req.what.substr(0, req.what.find('\n'));
            stream.commit({"  " + (yes ? pal.ok : pal.err) + (yes ? "approved: " : "denied: ") + pal.reset +
                           truncate_width(one_line(first), cols - 16)});
        } else {
            say("  that approval was already resolved", 'd');
        }
    }
    void appr_key(const Key & k) {
        switch (approval_key(appr_viewing, k)) {
            case ApprovalAct::Approve: return appr_answer(true);
            case ApprovalAct::Deny: return appr_answer(false);
            case ApprovalAct::StopView: appr_viewing = false; return;
            case ApprovalAct::ViewMore: {
                if (appr_body.empty()) { say("  (no more text)", 'd'); return; }
                if (appr_viewing && appr_pos >= appr_body.size()) { appr_viewing = false; say("  (end of text)", 'd'); return; }
                appr_viewing = true;
                size_t next = appr_pos;
                stream.commit(approval_page(appr_body, appr_pos, std::max(5, rows - 8), &next));
                appr_pos = next;
                if (appr_pos >= appr_body.size()) { appr_viewing = false; say("  (end of text)", 'd'); }
                return;
            }
            default: return;   // typing is paused while a request waits
        }
    }
    void show_approvals() {
        appr_refresh();
        appr_announce();
        stream.commit(approval_list_rows(appr, pal, cols - 1));
    }

    void on_key(const Key & k) {
        // key priority: a pending approval, then /cancel-agent y/n, then revert y/n, then the snapshot pager
        // (typing is paused while a request waits; Ctrl+C still cancels the turn, which declines it)
        if (appr.current() && !k.is_ctrl('c')) return appr_key(k);
        if (!cancel_agent.empty()) {   // /cancel-agent confirmation
            bool yes = k.type == Key::Char && !k.ctrl && !k.alt && (k.text == "y" || k.text == "Y");
            bool no = (k.type == Key::Char && !k.ctrl && !k.alt && (k.text == "n" || k.text == "N")) || k.type == Key::Esc;
            if (yes || no) confirm_cancel_agent(yes);
            return;
        }
        // revert confirmation: only an explicit y reverts; every other key cancels
        if (!revert_id.empty()) {
            answer_revert(k.type == Key::Char && !k.ctrl && !k.alt && (k.text == "y" || k.text == "Y"));
            return;
        }
        // snapshot diff pager
        if (!pager.empty()) {
            bool more = k.type == Key::Enter || k.type == Key::Down || (k.type == Key::Char && !k.ctrl && !k.alt && k.text == " ");
            if (more) pager_next();
            else { pager.clear(); pager_pos = 0; }
            return;
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
                if (!picking) handle_paste(k.text);
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
                    else if (kind == "theme") apply_theme(it.value);
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
                if (team_watch) { stop_team_watch(); break; }
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
        for (auto & e : evs) {
            comp.on_event(e, now_s());
            const std::string et = jstr(e, "type");
            if (et == "approval_request" || et == "approval_resolved" || et == "gap") appr_event(e);
            if (et != "approval_request" && et != "approval_resolved") stream.on_event(e);
            team_event(e);
        }
        appr_announce();
        team_tick();
        if (!was && stream.running()) turn_start = now_s();
        if (was && !stream.running() && !queued.empty()) {
            Queued next = queued.front();
            queued.erase(queued.begin());
            send_text(next.text, next.text, next.ids, next.chips);
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
        load_companions();
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
            int base_timeout = dec.pending() ? 40 : (stream.running() ? 100 : 1000);
            int timeout = base_timeout;
            if (mascot_visible()) timeout = std::min(timeout, std::max(10, (int) ((sprite_next - now_s()) * 1000)));
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
            if (pr == 0 && t - last_paint >= base_timeout / 1000.0 - 0.02) dirty = true;
            if (dirty && !quit) repaint();
            else if (!quit && mascot_visible() && t >= sprite_next) repaint_sprites();
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
