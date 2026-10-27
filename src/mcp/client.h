// One MCP server over stdio: a child process speaking newline-delimited JSON-RPC 2.0.
//
// Protocol eras (https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning):
//   modern (2026-07-28 and later): no handshake; every request carries its version and capabilities in _meta.
//   legacy (2025-11-25 and earlier): initialize / notifications/initialized, then requests.
// "auto" probes with server/discover (as the stdio binding recommends) and falls back to the legacy handshake on
// any other outcome. Judas is a tools-only client: no roots, sampling or elicitation are offered.
//
// The child is started with posix_spawn (never a bare fork in this multithreaded process), in its own process
// group, with a reader thread for stdout and one for stderr. A call that outlives its timeout is cancelled
// (notifications/cancelled). If the child dies, the next call restarts it, with exponential backoff.
#pragma once

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/json.h"

namespace judas::mcp {

struct ToolDef {
    std::string name, title, description;
    json input_schema = json::object();
    json annotations = json::object();  // readOnlyHint, destructiveHint, ... (untrusted hints)
};

struct ClientOptions {
    std::string name;                         // for messages
    std::vector<std::string> argv;            // full command (a sandbox wrapper is already applied)
    std::vector<std::string> env;             // "K=V" entries; the child gets exactly these
    std::string cwd;
    std::string protocol = "auto";            // auto | legacy | modern
    int timeout_s = 60;                       // per tools/call
    int startup_timeout_s = 30;               // handshake + first list
    int probe_timeout_ms = 4000;              // server/discover probe in auto mode
    int shutdown_grace_ms = 2000;             // after closing stdin, then SIGTERM, then SIGKILL
    int backoff_initial_ms = 500;             // restart delay after a crash: initial * 2^(failures-1), capped
    int backoff_max_ms = 30000;
};

struct RpcResult {
    bool ok = false;
    json result;                              // when ok
    int code = 0;                             // JSON-RPC error code when !ok (0 = transport problem)
    std::string message;                      // error text when !ok
    bool timed_out = false;
    bool dead = false;                        // the server process went away
};

struct CallOutcome {
    bool ok = false;                          // transport + protocol success (the tool may still set isError)
    json result;                              // the tools/call result object
    std::string error;                        // when !ok
};

class Client {
public:
    explicit Client(ClientOptions o);
    ~Client();
    Client(const Client &) = delete;
    Client & operator=(const Client &) = delete;

    // Spawn and handshake. Blocks up to startup_timeout_s. On failure the process is stopped and err is set.
    bool start(std::string & err);
    // Graceful: close stdin, wait, SIGTERM, wait, SIGKILL. Safe to call twice; start() can follow.
    void stop();
    void shutdown();                          // stop() for good: later calls fail instead of restarting
    bool running() const { return running_.load(); }

    // tools/list across all pages. Starts or restarts the server first if needed (honouring backoff).
    bool list_tools(std::vector<ToolDef> & out, std::string & err);
    // tools/call with the configured timeout. Restarts a dead server first; a crash during the call is an error.
    CallOutcome call_tool(const std::string & name, const json & args);

    // Called (on a helper thread, never the reader) after notifications/tools/list_changed and after a restart.
    void on_tools_changed(std::function<void()> cb);

    std::string protocol_version() const;     // negotiated version ("" before start)
    std::string server_name() const;
    bool server_announces_list_changed() const { return list_changed_cap_.load(); }
    std::string stderr_tail() const;          // last few KB the server wrote to stderr
    int restarts() const { return restarts_.load(); }
    int consecutive_failures() const { return failures_.load(); }

private:
    struct Pending {
        bool done = false;
        RpcResult res;
    };

    bool spawn(std::string & err);            // caller holds life_mu_
    bool start_locked(std::string & err);     // caller holds start_mu_
    bool handshake(std::string & err);
    bool ensure_running(std::string & err);
    RpcResult request(const std::string & method, json params, int timeout_ms, bool with_meta = true);
    void notify(const std::string & method, json params = json::object());
    bool write_line(const std::string & line);
    void reader_loop(int fd);
    void stderr_loop(int fd);
    void handle_message(const json & m);
    void mark_dead();
    void note_failure();
    void notifier_loop();
    json meta() const;

    ClientOptions o_;
    mutable std::mutex mu_;                   // pending_, proto_, state
    std::condition_variable cv_;
    std::map<long long, Pending *> pending_;
    long long next_id_ = 1;

    std::mutex life_mu_;                      // serializes start/stop (spawn, reap)
    std::mutex start_mu_;                     // one (re)start at a time
    std::atomic<bool> reader_quit_{false};
    std::atomic<bool> closed_{false};
    std::mutex write_mu_;
    int pid_ = -1;
    int in_fd_ = -1;
    std::thread reader_, err_reader_;
    std::atomic<bool> running_{false};        // process alive and handshaken
    std::atomic<bool> eof_{false};            // reader saw EOF on the current process
    std::string proto_ = "";
    bool modern_ = false;
    std::string server_name_;
    std::atomic<bool> list_changed_cap_{false};

    mutable std::mutex err_mu_;
    std::string err_tail_;

    std::atomic<int> failures_{0};
    std::atomic<int> restarts_{0};
    std::atomic<long long> next_attempt_ms_{0};  // steady-clock ms
    std::atomic<bool> ever_started_{false};

    std::mutex cb_mu_;
    std::condition_variable cb_cv_;
    std::function<void()> cb_;
    bool cb_dirty_ = false;
    bool cb_stop_ = false;
    std::thread notifier_;
};

}  // namespace judas::mcp
