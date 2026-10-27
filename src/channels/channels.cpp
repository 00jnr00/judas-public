// Channels: ntfy (HTTP POST) and command (stdin + env) delivery. Never throws.
#include "channels/channels.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstdio>

#include "core/json.h"
#include "cpp-httplib/httplib.h"

namespace judas {
namespace {

std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// Split "scheme://host[:port]/path" into the client base ("scheme://host[:port]") and the path.
bool split_url(const std::string & url, std::string & base, std::string & path) {
    size_t p = url.find("://");
    if (p == std::string::npos) return false;
    std::string scheme = url.substr(0, p);
    if (scheme != "http" && scheme != "https") return false;
    size_t slash = url.find('/', p + 3);
    if (slash == std::string::npos) { base = url; path = "/"; }
    else { base = url.substr(0, slash); path = url.substr(slash); }
    return base.size() > p + 3;
}

// Matrix: target is "<homeserver_base>|<room_id>" (e.g. https://matrix.example.org|!room:example.org).
// The access token comes from the env var JUDAS_MATRIX_TOKEN (never inline in config). Sends an m.text message.
ChannelResult send_matrix(const Channel & ch, const std::string & title, const std::string & body) {
    size_t bar = ch.target.find('|');
    if (bar == std::string::npos) return {false, "bad matrix target (need <homeserver>|<room_id>)"};
    std::string base = trim(ch.target.substr(0, bar)), room = trim(ch.target.substr(bar + 1));
    const char * tok = std::getenv("JUDAS_MATRIX_TOKEN");
    if (!tok || !*tok) return {false, "JUDAS_MATRIX_TOKEN is not set"};
    std::string host, path0;
    if (!split_url(base, host, path0)) return {false, "bad matrix homeserver URL"};
    httplib::Client cli(host);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);
    cli.set_write_timeout(10);
    std::string txn = std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                         std::chrono::system_clock::now().time_since_epoch())
                                         .count());
    // url-encode the room id (contains ! and :)
    std::string enc;
    for (unsigned char c : room) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') enc += (char) c;
        else { char b[4]; snprintf(b, sizeof b, "%%%02X", c); enc += b; }
    }
    std::string text = title.empty() ? body : title + "\n" + body;
    json msg = {{"msgtype", "m.text"}, {"body", text}};
    httplib::Headers h = {{"Authorization", std::string("Bearer ") + tok}};
    std::string p = "/_matrix/client/v3/rooms/" + enc + "/send/m.room.message/" + txn;
    auto res = cli.Put(p, h, msg.dump(), "application/json");
    if (!res) return {false, "matrix request failed: " + httplib::to_string(res.error())};
    if (res->status < 200 || res->status >= 300) return {false, "matrix returned HTTP " + std::to_string(res->status)};
    return {true, ""};
}

ChannelResult send_ntfy(const Channel & ch, const std::string & title, const std::string & body) {
    std::string base, path;
    if (!split_url(ch.target, base, path)) return {false, "bad ntfy target (need http(s)://host/topic)"};
    httplib::Client cli(base);
    cli.set_connection_timeout(10);
    cli.set_read_timeout(10);
    cli.set_write_timeout(10);
    httplib::Headers h;
    if (!title.empty()) { h.emplace("Title", title); h.emplace("X-Title", title); }
    auto res = cli.Post(path, h, body, "text/plain");
    if (!res) return {false, "ntfy request failed: " + httplib::to_string(res.error())};
    if (res->status < 200 || res->status >= 300) return {false, "ntfy returned HTTP " + std::to_string(res->status)};
    return {true, ""};
}

// fork/exec bash -lc with body on stdin; the child leads its own process group so a timeout kills the tree.
ChannelResult send_command(const Channel & ch, const std::string & title, const std::string & body) {
    int in[2], out[2];
    if (pipe(in) != 0) return {false, "pipe failed"};
    if (pipe(out) != 0) { close(in[0]); close(in[1]); return {false, "pipe failed"}; }
    pid_t pid = fork();
    if (pid < 0) { close(in[0]); close(in[1]); close(out[0]); close(out[1]); return {false, "fork failed"}; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        setenv("JUDAS_NOTIFY_TITLE", title.c_str(), 1);
        execl("/bin/bash", "bash", "-lc", ch.target.c_str(), (char *) nullptr);
        _exit(127);
    }
    setpgid(pid, pid);
    close(in[0]); close(out[1]);
    int wfd = in[1], rfd = out[0];
    fcntl(wfd, F_SETFL, fcntl(wfd, F_GETFL) | O_NONBLOCK);
    signal(SIGPIPE, SIG_IGN);
    size_t off = 0;
    if (body.empty()) { close(wfd); wfd = -1; }

    std::string output;
    int status = 0;
    bool exited = false, timed_out = false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    char buf[4096];
    while (true) {
        struct pollfd pf[2] = {{rfd, POLLIN, 0}, {wfd, POLLOUT, 0}};
        int pr = poll(pf, wfd >= 0 ? 2 : 1, 100);
        if (pr > 0) {
            if (wfd >= 0 && (pf[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
                ssize_t n = write(wfd, body.data() + off, body.size() - off);
                if (n > 0) off += (size_t) n;
                if (n < 0 && errno != EAGAIN && errno != EINTR) off = body.size();
                if (off >= body.size()) { close(wfd); wfd = -1; }
            }
            if (pf[0].revents & (POLLIN | POLLHUP)) {
                ssize_t n = read(rfd, buf, sizeof buf);
                if (n > 0) { if (output.size() < (1u << 20)) output.append(buf, (size_t) n); continue; }
                if (n == 0) break;  // EOF
            }
        }
        if (!exited && waitpid(pid, &status, WNOHANG) == pid) exited = true;
        if (exited && pr <= 0) break;
        if (std::chrono::steady_clock::now() > deadline) {
            timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            break;
        }
    }
    if (wfd >= 0) close(wfd);
    close(rfd);
    if (!exited) waitpid(pid, &status, 0);
    if (timed_out) return {false, "command timed out after 15s"};
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    if (code != 0) {
        std::string msg = trim(output);
        if (msg.size() > 500) msg.resize(500);
        return {false, msg.empty() ? "command exited with code " + std::to_string(code) : msg};
    }
    return {true, ""};
}

}  // namespace

ChannelResult Channels::send(const std::string & name, const std::string & title, const std::string & body) const {
    try {
        if (cfg_.channels.empty()) return {false, "no channels configured"};
        const Channel * ch = nullptr;
        const std::string & want = name.empty() ? cfg_.def : name;
        if (!want.empty())
            for (auto & c : cfg_.channels) if (c.name == want) { ch = &c; break; }
        if (!ch && name.empty() && cfg_.channels.size() == 1) ch = &cfg_.channels[0];
        if (!ch) return {false, "no such channel"};
        if (!ch->enabled) return {false, "channel disabled"};
        if (ch->kind == "ntfy") return send_ntfy(*ch, title, body);
        if (ch->kind == "command") return send_command(*ch, title, body);
        if (ch->kind == "matrix") return send_matrix(*ch, title, body);
        return {false, "unknown channel kind"};
    } catch (const std::exception & e) {
        return {false, std::string("channel error: ") + e.what()};
    } catch (...) {
        return {false, "channel error"};
    }
}

std::vector<std::string> Channels::names() const {
    std::vector<std::string> v;
    for (auto & c : cfg_.channels) v.push_back(c.name);
    return v;
}

}  // namespace judas
