// See client.h.
#include "mcp/client.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

extern char ** environ;

namespace judas {
const char * version();
}

namespace judas::mcp {

namespace {

const char * const kModernVersions[] = {"2026-07-28"};
const char * const kLegacyVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};

bool is_modern_version(const std::string & v) {
    for (auto m : kModernVersions) if (v == m) return true;
    return false;
}
bool is_legacy_version(const std::string & v) {
    for (auto m : kLegacyVersions) if (v == m) return true;
    return false;
}

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string dump_line(const json & j) {
    return j.dump(-1, ' ', false, json::error_handler_t::replace);
}

// JUDAS_MCP_DEBUG=1 logs every message's size and method to stderr (never the content).
bool debug_wire() {
    static const bool on = [] { const char * e = getenv("JUDAS_MCP_DEBUG"); return e && *e && std::string(e) != "0"; }();
    return on;
}

// bwrap --die-with-parent (and any PR_SET_PDEATHSIG) fires when the *thread* that created the child exits, not
// when the process does. Servers are started from short-lived connect threads, so spawn on one thread that lives
// as long as the process (found the hard way: a sandboxed server died right after tools/list).
class Spawner {
public:
    static Spawner & get() {
        static Spawner * s = new Spawner();  // never destroyed: the thread must outlive every Client
        return *s;
    }
    void run(const std::function<void()> & fn) {
        std::lock_guard one_at_a_time(call_mu_);
        std::unique_lock lk(mu_);
        task_ = &fn;
        done_ = false;
        cv_.notify_all();
        cv_.wait(lk, [&] { return done_; });
    }

private:
    Spawner() {
        std::thread([this] {
            std::unique_lock lk(mu_);
            while (true) {
                cv_.wait(lk, [&] { return task_ != nullptr; });
                const std::function<void()> * f = task_;
                lk.unlock();
                (*f)();
                lk.lock();
                task_ = nullptr;
                done_ = true;
                cv_.notify_all();
            }
        }).detach();
    }
    std::mutex call_mu_, mu_;
    std::condition_variable cv_;
    const std::function<void()> * task_ = nullptr;
    bool done_ = true;
};

void ignore_sigpipe_once() {
    static std::once_flag f;
    std::call_once(f, [] { signal(SIGPIPE, SIG_IGN); });
}

}  // namespace

Client::Client(ClientOptions o) : o_(std::move(o)) {
    ignore_sigpipe_once();
    notifier_ = std::thread([this] { notifier_loop(); });
}

Client::~Client() {
    {
        std::lock_guard lk(cb_mu_);
        cb_stop_ = true;
        cb_ = nullptr;
    }
    cb_cv_.notify_all();
    if (notifier_.joinable()) notifier_.join();
    shutdown();
}

void Client::on_tools_changed(std::function<void()> cb) {
    std::lock_guard lk(cb_mu_);
    cb_ = std::move(cb);
}

void Client::notifier_loop() {
    std::unique_lock lk(cb_mu_);
    while (true) {
        cb_cv_.wait(lk, [&] { return cb_stop_ || cb_dirty_; });
        if (cb_stop_) return;
        cb_dirty_ = false;
        auto cb = cb_;
        lk.unlock();
        if (cb) {
            try { cb(); } catch (...) {}
        }
        lk.lock();
    }
}

std::string Client::protocol_version() const {
    std::lock_guard lk(mu_);
    return proto_;
}
std::string Client::server_name() const {
    std::lock_guard lk(mu_);
    return server_name_;
}
std::string Client::stderr_tail() const {
    std::lock_guard lk(err_mu_);
    return err_tail_;
}

json Client::meta() const {
    return {{"io.modelcontextprotocol/protocolVersion", proto_},
            {"io.modelcontextprotocol/clientCapabilities", json::object()},
            {"io.modelcontextprotocol/clientInfo", {{"name", "judas"}, {"version", judas::version()}}}};
}

// ---------- process ----------

bool Client::spawn(std::string & err) {
    int in[2], out[2], er[2];
    if (pipe2(in, O_CLOEXEC) != 0) { err = std::string("pipe: ") + strerror(errno); return false; }
    if (pipe2(out, O_CLOEXEC) != 0) { close(in[0]); close(in[1]); err = std::string("pipe: ") + strerror(errno); return false; }
    if (pipe2(er, O_CLOEXEC) != 0) {
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        err = std::string("pipe: ") + strerror(errno);
        return false;
    }

    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, in[0], 0);
    posix_spawn_file_actions_adddup2(&fa, out[1], 1);
    posix_spawn_file_actions_adddup2(&fa, er[1], 2);
    if (!o_.cwd.empty()) posix_spawn_file_actions_addchdir_np(&fa, o_.cwd.c_str());

    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
    sigset_t none, dflt;
    sigemptyset(&none);
    sigemptyset(&dflt);
    sigaddset(&dflt, SIGPIPE);
    posix_spawnattr_setsigmask(&at, &none);
    posix_spawnattr_setsigdefault(&at, &dflt);
    posix_spawnattr_setpgroup(&at, 0);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF);

    std::vector<char *> av, ev;
    for (auto & a : o_.argv) av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);
    for (auto & e : o_.env) ev.push_back(const_cast<char *>(e.c_str()));
    ev.push_back(nullptr);

    pid_t pid = -1;
    int rc = 0;
    Spawner::get().run([&] { rc = posix_spawnp(&pid, av[0], &fa, &at, av.data(), ev.data()); });
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    close(in[0]);
    close(out[1]);
    close(er[1]);
    if (rc != 0) {
        close(in[1]); close(out[0]); close(er[0]);
        err = "cannot start '" + o_.argv[0] + "': " + strerror(rc);
        return false;
    }
    fcntl(in[1], F_SETFL, fcntl(in[1], F_GETFL) | O_NONBLOCK);
    {
        std::lock_guard lk(err_mu_);
        err_tail_.clear();
    }
    eof_ = false;
    reader_quit_ = false;
    pid_ = pid;
    in_fd_ = in[1];
    reader_ = std::thread([this, fd = out[0]] { reader_loop(fd); });
    err_reader_ = std::thread([this, fd = er[0]] { stderr_loop(fd); });
    return true;
}

void Client::reader_loop(int fd) {
    std::string buf;
    char tmp[16384];
    while (!reader_quit_) {
        struct pollfd pf{fd, POLLIN, 0};
        int pr = poll(&pf, 1, 100);
        if (pr < 0 && errno != EINTR) break;
        if (pr <= 0) continue;
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) break;  // EOF or error: the server is gone
        buf.append(tmp, (size_t) n);
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            json m = json::parse(line, nullptr, false);
            if (debug_wire())
                std::fprintf(stderr, "mcp[%s] <- %zu bytes %s\n", o_.name.c_str(), line.size(),
                             m.is_discarded() ? "(not JSON)" : m.contains("method") ? m["method"].dump().c_str() : "response");
            if (m.is_discarded() || !m.is_object()) continue;  // not an MCP message: ignore
            handle_message(m);
        }
        if (buf.size() > (64u << 20)) buf.clear();  // a runaway line without a newline
    }
    close(fd);
    if (debug_wire()) std::fprintf(stderr, "mcp[%s] stdout closed (quit=%d)\n", o_.name.c_str(), (int) reader_quit_.load());
    eof_ = true;
    mark_dead();
}

void Client::stderr_loop(int fd) {
    char tmp[4096];
    while (!reader_quit_) {
        struct pollfd pf{fd, POLLIN, 0};
        int pr = poll(&pf, 1, 100);
        if (pr < 0 && errno != EINTR) break;
        if (pr <= 0) continue;
        ssize_t n = read(fd, tmp, sizeof tmp);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) break;
        if (debug_wire()) std::fprintf(stderr, "mcp[%s] stderr: %.*s", o_.name.c_str(), (int) n, tmp);
        std::lock_guard lk(err_mu_);
        err_tail_.append(tmp, (size_t) n);
        if (err_tail_.size() > 4096) err_tail_.erase(0, err_tail_.size() - 4096);
    }
    close(fd);
}

void Client::mark_dead() {
    running_ = false;
    std::lock_guard lk(mu_);
    for (auto & [id, p] : pending_) {
        if (!p->done) {
            p->done = true;
            p->res = RpcResult{};
            p->res.dead = true;
            p->res.message = "the server process exited";
        }
    }
    cv_.notify_all();
}

void Client::stop() {
    std::lock_guard life(life_mu_);
    if (pid_ < 0) return;
    running_ = false;
    {
        std::lock_guard wl(write_mu_);
        if (in_fd_ >= 0) { close(in_fd_); in_fd_ = -1; }
    }
    pid_t pid = pid_;
    auto wait_exit = [&](int ms) {
        long long end = now_ms() + ms;
        while (now_ms() < end) {
            int st;
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid || (r < 0 && errno == ECHILD)) return true;
            usleep(10000);
        }
        return false;
    };
    bool gone = wait_exit(o_.shutdown_grace_ms);
    if (!gone) {
        kill(-pid, SIGTERM);
        kill(pid, SIGTERM);
        gone = wait_exit(o_.shutdown_grace_ms);
    }
    if (!gone) {
        kill(-pid, SIGKILL);
        kill(pid, SIGKILL);
        int st;
        waitpid(pid, &st, 0);
    } else {
        kill(-pid, SIGKILL);  // stragglers in the group (children of the server)
    }
    reader_quit_ = true;
    if (reader_.joinable()) reader_.join();
    if (err_reader_.joinable()) err_reader_.join();
    pid_ = -1;
    mark_dead();
}

void Client::shutdown() {
    closed_ = true;
    stop();
}

bool Client::write_line(const std::string & line) {
    std::lock_guard wl(write_mu_);
    if (in_fd_ < 0) return false;
    std::string data = line + "\n";
    size_t off = 0;
    int stalls = 0;
    while (off < data.size()) {
        ssize_t n = write(in_fd_, data.data() + off, data.size() - off);
        if (n > 0) { off += (size_t) n; stalls = 0; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) {
            struct pollfd pf{in_fd_, POLLOUT, 0};
            poll(&pf, 1, 1000);
            if (++stalls > 30) return false;  // the server is not reading its stdin
            continue;
        }
        return false;
    }
    return true;
}

// ---------- JSON-RPC ----------

RpcResult Client::request(const std::string & method, json params, int timeout_ms, bool with_meta) {
    Pending p;
    long long id;
    bool modern;
    {
        std::lock_guard lk(mu_);
        id = next_id_++;
        modern = modern_;
        pending_[id] = &p;
    }
    if (!params.is_object()) params = json::object();
    if (with_meta && modern) {
        json m = meta();
        if (params.contains("_meta") && params["_meta"].is_object()) m.update(params["_meta"]);
        params["_meta"] = m;
    }
    json msg = {{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    if (debug_wire()) std::fprintf(stderr, "mcp[%s] -> %s id=%lld\n", o_.name.c_str(), method.c_str(), id);
    if (!write_line(dump_line(msg))) {
        std::lock_guard lk(mu_);
        pending_.erase(id);
        RpcResult r;
        r.dead = true;
        r.message = "cannot write to the server (it exited or is not reading)";
        return r;
    }
    std::unique_lock lk(mu_);
    bool done = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return p.done; });
    pending_.erase(id);
    if (!done) {
        lk.unlock();
        notify("notifications/cancelled", {{"requestId", id}, {"reason", "timeout"}});
        RpcResult r;
        r.timed_out = true;
        r.message = "no response within " + std::to_string(timeout_ms / 1000.0) + "s";
        return r;
    }
    return p.res;
}

void Client::notify(const std::string & method, json params) {
    json msg = {{"jsonrpc", "2.0"}, {"method", method}};
    if (params.is_object() && !params.empty()) msg["params"] = params;
    write_line(dump_line(msg));
}

void Client::handle_message(const json & m) {
    bool has_method = m.contains("method") && m["method"].is_string();
    bool has_id = m.contains("id") && !m["id"].is_null();
    if (has_method) {
        const std::string & method = m["method"].get_ref<const std::string &>();
        if (has_id) {
            // The server asked us something. We offer no client features; answer so it never waits on us.
            json reply = {{"jsonrpc", "2.0"}, {"id", m["id"]}};
            if (method == "ping") reply["result"] = json::object();
            else reply["error"] = {{"code", -32601}, {"message", "Judas is a tools-only MCP client: " + method + " is not supported"}};
            write_line(dump_line(reply));
        } else if (method == "notifications/tools/list_changed") {
            {
                std::lock_guard lk(cb_mu_);
                cb_dirty_ = true;
            }
            cb_cv_.notify_all();
        }
        return;
    }
    if (!has_id || !m["id"].is_number_integer()) return;
    long long id = m["id"].get<long long>();
    std::lock_guard lk(mu_);
    auto it = pending_.find(id);
    if (it == pending_.end() || it->second->done) return;  // late reply to a cancelled request
    RpcResult r;
    if (m.contains("error") && m["error"].is_object()) {
        r.code = m["error"].value("code", -32603);
        r.message = m["error"].value("message", std::string("unknown error"));
        if (m["error"].contains("data")) r.result = m["error"]["data"];
    } else if (m.contains("result")) {
        r.ok = true;
        r.result = m["result"];
        if (r.result.is_object() && r.result.value("resultType", std::string("complete")) == "input_required") {
            r.ok = false;
            r.code = -32000;
            r.message = "the server asked for more input (multi round-trip requests), which Judas does not support";
        }
    } else {
        return;
    }
    it->second->res = std::move(r);
    it->second->done = true;
    cv_.notify_all();
}

// ---------- lifecycle ----------

bool Client::handshake(std::string & err) {
    long long deadline = now_ms() + (long long) o_.startup_timeout_s * 1000;
    auto left = [&] { return (int) std::max<long long>(1000, deadline - now_ms()); };
    {
        std::lock_guard lk(mu_);
        modern_ = false;
        proto_.clear();
        server_name_.clear();
    }
    list_changed_cap_ = false;

    if (o_.protocol != "legacy") {
        {
            std::lock_guard lk(mu_);
            modern_ = true;
            proto_ = kModernVersions[0];
        }
        bool strict = o_.protocol == "modern";
        RpcResult r = request("server/discover", json::object(), strict ? left() : std::min(left(), o_.probe_timeout_ms));
        std::string chosen;
        if (r.ok) {
            if (r.result.contains("supportedVersions") && r.result["supportedVersions"].is_array())
                for (auto & v : r.result["supportedVersions"])
                    if (v.is_string() && is_modern_version(v.get<std::string>())) { chosen = v.get<std::string>(); break; }
            if (chosen.empty() && strict) {
                err = std::string("server/discover: the server supports no modern protocol version this client knows (") +
                      kModernVersions[0] + ")";
                return false;
            }
            if (!chosen.empty()) {
                std::lock_guard lk(mu_);
                proto_ = chosen;
                if (r.result.contains("serverInfo") && r.result["serverInfo"].is_object())
                    server_name_ = r.result["serverInfo"].value("name", std::string());
                else if (r.result.contains("_meta") && r.result["_meta"].is_object() &&
                         r.result["_meta"].contains("io.modelcontextprotocol/serverInfo") &&
                         r.result["_meta"]["io.modelcontextprotocol/serverInfo"].is_object())
                    server_name_ = r.result["_meta"]["io.modelcontextprotocol/serverInfo"].value("name", std::string());
                if (r.result.contains("capabilities") && r.result["capabilities"].is_object() &&
                    r.result["capabilities"].contains("tools") && r.result["capabilities"]["tools"].is_object())
                    list_changed_cap_ = r.result["capabilities"]["tools"].value("listChanged", false);
                return true;
            }
        } else if (r.code == -32022 && r.result.is_object() && r.result.contains("supported") &&
                   r.result["supported"].is_array()) {
            // A recognized modern error: the server is modern, so no fallback to initialize.
            for (auto & v : r.result["supported"])
                if (v.is_string() && is_modern_version(v.get<std::string>())) chosen = v.get<std::string>();
            if (chosen.empty()) {
                err = "the server supports protocol versions this client does not: " + r.result["supported"].dump();
                return false;
            }
            std::lock_guard lk(mu_);
            proto_ = chosen;
            return true;
        } else if (strict) {
            err = "server/discover failed: " + (r.message.empty() ? std::string("no answer") : r.message);
            return false;
        }
        if (r.dead) {
            err = "the server exited during the handshake";
            return false;
        }
        std::lock_guard lk(mu_);
        modern_ = false;
        proto_.clear();
    }

    json params = {{"protocolVersion", kLegacyVersions[0]},
                   {"capabilities", json::object()},
                   {"clientInfo", {{"name", "judas"}, {"version", judas::version()}}}};
    RpcResult r = request("initialize", params, left(), false);
    if (!r.ok) {
        err = r.dead ? "the server exited during the handshake"
                     : "initialize failed: " + (r.message.empty() ? std::string("no answer") : r.message);
        return false;
    }
    std::string pv = r.result.value("protocolVersion", std::string());
    if (!is_legacy_version(pv)) {
        err = "the server answered initialize with protocol version '" + pv + "', which this client does not speak";
        return false;
    }
    {
        std::lock_guard lk(mu_);
        proto_ = pv;
        if (r.result.contains("serverInfo") && r.result["serverInfo"].is_object())
            server_name_ = r.result["serverInfo"].value("name", std::string());
    }
    if (r.result.contains("capabilities") && r.result["capabilities"].is_object() &&
        r.result["capabilities"].contains("tools") && r.result["capabilities"]["tools"].is_object())
        list_changed_cap_ = r.result["capabilities"]["tools"].value("listChanged", false);
    notify("notifications/initialized");
    return true;
}

bool Client::start(std::string & err) {
    std::lock_guard serial(start_mu_);
    return start_locked(err);
}

bool Client::start_locked(std::string & err) {
    if (closed_) { err = "MCP server '" + o_.name + "' is shut down"; return false; }
    if (running_ && !eof_) return true;
    stop();  // reap a dead predecessor
    if (o_.argv.empty()) { err = "no command configured"; return false; }
    bool spawned;
    {
        std::lock_guard life(life_mu_);
        spawned = spawn(err);
    }
    if (!spawned) {
        note_failure();
        return false;
    }
    bool ok = handshake(err);
    if (!ok) {
        std::string tail = stderr_tail();
        if (!tail.empty()) {
            while (!tail.empty() && (tail.back() == '\n' || tail.back() == ' ')) tail.pop_back();
            if (tail.size() > 400) tail = tail.substr(tail.size() - 400);
            err += " [stderr: " + tail + "]";
        }
        stop();
        note_failure();
        return false;
    }
    running_ = true;
    bool again = ever_started_;
    ever_started_ = true;
    if (again) {
        restarts_++;
        {
            std::lock_guard lk(cb_mu_);
            cb_dirty_ = true;
        }
        cb_cv_.notify_all();
    }
    return true;
}

void Client::note_failure() {
    int n = ++failures_;
    long long delay = (long long) o_.backoff_initial_ms << std::min(n - 1, 16);
    delay = std::min<long long>(delay, o_.backoff_max_ms);
    next_attempt_ms_ = now_ms() + delay;
}

bool Client::ensure_running(std::string & err) {
    if (running_ && !eof_) return true;
    if (closed_) { err = "MCP server '" + o_.name + "' is shut down"; return false; }
    std::lock_guard serial(start_mu_);  // one restart at a time; later callers find it running
    if (running_ && !eof_) return true;
    long long wait = next_attempt_ms_ - now_ms();
    if (wait > 0) {
        err = "MCP server '" + o_.name + "' is down (" + std::to_string(failures_.load()) + " failure(s) in a row); " +
              "it will be restarted on a call after " + std::to_string((wait + 999) / 1000) + "s";
        std::string tail = stderr_tail();
        while (!tail.empty() && (tail.back() == '\n' || tail.back() == ' ')) tail.pop_back();
        if (!tail.empty()) err += ". Last stderr: " + tail.substr(tail.size() > 300 ? tail.size() - 300 : 0);
        return false;
    }
    return start_locked(err);
}

// ---------- tools ----------

bool Client::list_tools(std::vector<ToolDef> & out, std::string & err) {
    if (!ensure_running(err)) return false;
    out.clear();
    std::string cursor;
    for (int page = 0; page < 50; page++) {
        json params = json::object();
        if (!cursor.empty()) params["cursor"] = cursor;
        RpcResult r = request("tools/list", params, o_.startup_timeout_s * 1000);
        if (!r.ok) {
            err = r.timed_out ? "tools/list timed out" : "tools/list failed: " + r.message;
            if (r.dead) note_failure();
            return false;
        }
        if (r.result.contains("tools") && r.result["tools"].is_array()) {
            for (auto & t : r.result["tools"]) {
                if (!t.is_object() || !t.contains("name") || !t["name"].is_string()) continue;
                ToolDef d;
                d.name = t["name"].get<std::string>();
                d.title = t.value("title", std::string());
                d.description = t.contains("description") && t["description"].is_string() ? t["description"].get<std::string>() : "";
                if (t.contains("inputSchema") && t["inputSchema"].is_object()) d.input_schema = t["inputSchema"];
                if (t.contains("annotations") && t["annotations"].is_object()) d.annotations = t["annotations"];
                out.push_back(std::move(d));
            }
        }
        cursor = r.result.contains("nextCursor") && r.result["nextCursor"].is_string() ? r.result["nextCursor"].get<std::string>() : "";
        if (cursor.empty()) break;
    }
    failures_ = 0;
    return true;
}

CallOutcome Client::call_tool(const std::string & name, const json & args) {
    CallOutcome o;
    if (!ensure_running(o.error)) return o;
    RpcResult r = request("tools/call", {{"name", name}, {"arguments", args.is_object() ? args : json::object()}},
                          o_.timeout_s * 1000);
    if (r.ok) {
        failures_ = 0;
        o.ok = true;
        o.result = std::move(r.result);
        return o;
    }
    if (r.timed_out) {
        o.error = "MCP tool '" + name + "' timed out after " + std::to_string(o_.timeout_s) + "s and was cancelled";
    } else if (r.dead) {
        note_failure();
        std::string tail = stderr_tail();
        while (!tail.empty() && (tail.back() == '\n' || tail.back() == ' ')) tail.pop_back();
        o.error = "MCP server '" + o_.name + "' crashed during the call (it restarts on the next call)";
        if (!tail.empty()) o.error += ". Last stderr: " + tail.substr(tail.size() > 300 ? tail.size() - 300 : 0);
    } else {
        o.error = "MCP error " + std::to_string(r.code) + ": " + r.message;
    }
    return o;
}

}  // namespace judas::mcp
