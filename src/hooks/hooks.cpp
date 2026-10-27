// Hooks runner: fork/exec /bin/bash -lc per hook, payload JSON on stdin, process-group kill on timeout.
#include "hooks/hooks.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <string>

namespace judas {

namespace {

struct HookRun {
    int code = -1;
    bool timed_out = false;
    bool start_failed = false;
    std::string output;  // stdout+stderr, capped
};

const size_t kOutCap = 8192;

std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

HookRun run_hook(const std::string & event, const std::string & command, const std::string & input, int timeout_s) {
    HookRun r;
    int out[2], in[2];
    if (pipe(out) != 0) { r.start_failed = true; r.output = "pipe failed"; return r; }
    if (pipe(in) != 0) {
        close(out[0]); close(out[1]);
        r.start_failed = true; r.output = "pipe failed"; return r;
    }
    signal(SIGPIPE, SIG_IGN);  // a hook that ignores stdin must not kill us
    pid_t pid = fork();
    if (pid < 0) {
        close(out[0]); close(out[1]); close(in[0]); close(in[1]);
        r.start_failed = true; r.output = "fork failed"; return r;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(in[0], 0);
        dup2(out[1], 1);
        dup2(out[1], 2);
        close(in[0]); close(in[1]); close(out[0]); close(out[1]);
        setenv("JUDAS_HOOK_EVENT", event.c_str(), 1);
        execl("/bin/bash", "bash", "-lc", command.c_str(), (char *)nullptr);
        _exit(127);
    }
    setpgid(pid, pid);
    close(in[0]);
    close(out[1]);
    int wfd = in[1], rfd = out[0];
    fcntl(wfd, F_SETFL, fcntl(wfd, F_GETFL) | O_NONBLOCK);
    size_t woff = 0;
    if (input.empty()) { close(wfd); wfd = -1; }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(1, timeout_s));
    char buf[4096];
    int status = 0;
    bool exited = false;
    while (true) {
        struct pollfd pf[2];
        int n = 0;
        pf[n++] = {rfd, POLLIN, 0};
        if (wfd >= 0) pf[n++] = {wfd, POLLOUT, 0};
        int pr = poll(pf, n, 50);
        bool progressed = false;
        if (pr > 0) {
            if (pf[0].revents & (POLLIN | POLLHUP)) {
                ssize_t k = read(rfd, buf, sizeof buf);
                if (k > 0) {
                    if (r.output.size() < kOutCap) r.output.append(buf, std::min<size_t>(k, kOutCap - r.output.size()));
                    progressed = true;
                } else if (k == 0) {
                    break;  // EOF
                }
            }
            if (wfd >= 0 && n > 1 && (pf[1].revents & (POLLOUT | POLLERR | POLLHUP))) {
                ssize_t k = write(wfd, input.data() + woff, input.size() - woff);
                if (k > 0) woff += k;
                if (k < 0 && errno != EAGAIN && errno != EINTR) woff = input.size();  // reader gone
                if (woff >= input.size()) { close(wfd); wfd = -1; }
                progressed = true;
            }
        }
        if (!exited && waitpid(pid, &status, WNOHANG) == pid) exited = true;
        if (exited && !progressed && pr <= 0) break;  // child gone and nothing left to drain
        if (std::chrono::steady_clock::now() > deadline) {
            r.timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            break;
        }
    }
    if (wfd >= 0) close(wfd);
    close(rfd);
    if (!exited) waitpid(pid, &status, 0);
    if (!r.timed_out) {
        if (WIFEXITED(status)) r.code = WEXITSTATUS(status);
        else if (WIFSIGNALED(status)) r.code = 128 + WTERMSIG(status);
    }
    return r;
}

}  // namespace

HookOutcome Hooks::run(const std::string & event, const std::string & selector, const json & payload) const {
    HookOutcome out;
    if (!enabled()) return out;
    const bool is_pre = event.rfind("pre-", 0) == 0;
    std::string input;
    try { input = payload.dump(); } catch (...) { input = "{}"; }
    std::string note;  // exit codes of non-gating blocking hooks, kept for the caller
    for (const auto & h : cfg_.hooks) {
        if (h.event != event) continue;
        if (!h.match.empty() && selector.find(h.match) == std::string::npos) continue;
        HookRun r;
        try {
            r = run_hook(event, h.command, input, h.timeout_s);
        } catch (const std::exception & e) {
            r.start_failed = true;
            r.output = e.what();
        } catch (...) {
            r.start_failed = true;
            r.output = "hook failed to start";
        }
        if (!h.blocking) continue;
        bool failed = r.start_failed || r.timed_out || r.code != 0;
        if (!failed) continue;
        std::string text = trim(r.output);
        if (r.timed_out) {
            if (!text.empty()) text += "\n";
            text += "[hook timed out after " + std::to_string(h.timeout_s) + "s]";
        }
        if (is_pre) {
            out.allow = false;
            out.reason = !text.empty() ? text
                                       : "hook " + event + " blocked (exit " + std::to_string(r.code) + ")";
            return out;
        }
        if (!note.empty()) note += "; ";
        note += "hook " + event + " exited " + (r.timed_out ? "timeout" : std::to_string(r.code));
    }
    out.reason = note;  // allow stays true
    return out;
}

}  // namespace judas
