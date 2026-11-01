// swarm_map: fan a bulk job out to a second, CPU-only Judas instance running small models (docs/SWARM.md).
// The pure helpers (chunking, glob matching, the parallel runner) are exposed for tests.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "core/config.h"
#include "tools/tools.h"

namespace judas {

void register_swarm_tool(ToolRegistry & reg);

// Split text into pieces of at most max_chars bytes, cutting at a line break when one is near the end of the
// piece (never inside a UTF-8 sequence). Text that fits comes back as one piece; empty text as none.
std::vector<std::string> swarm_chunks(const std::string & text, size_t max_chars);

// Glob match of a relative path: * and ? stay inside a path segment, ** crosses segments, {a,b} alternates.
bool swarm_glob_match(const std::string & pattern, const std::string & path);

struct SwarmReply {
    bool ok = false;
    std::string text;    // the reply, or the error
};

// Run fn over items 0..n-1 with at most `parallel` in flight. results[i] is fn(i). progress(done, total) is
// called after each item, from the worker thread, under a lock (calls never overlap).
std::vector<SwarmReply> swarm_run(size_t n, int parallel, const std::function<SwarmReply(size_t)> & fn,
                                  const std::function<void(size_t done, size_t total)> & progress);

// One chat completion against the swarm: POST {url}/v1/chat/completions with the instruction as the system
// message and the item as the user message.
SwarmReply swarm_complete(const std::string & url, const std::string & instruction, const std::string & item);

// Cores of the swarm instance. `threads` is both llama thread counts; `pin` pins the process to the first
// `threads` CPUs of the pin order (whole physical cores, one CCD first). The setting lives in the instance's own
// config (cfg.swarm.config); a running instance also gets it live (POST /api/cores, llama_set_n_threads, no
// restart). get/set never throw; ok=false carries the reason in `error`.
struct SwarmCores {
    bool ok = false;
    std::string error;
    int threads = 0;          // from the instance when it is up, else from its config file
    int nproc = 0;
    bool pin = false;
    std::vector<int> cpus;    // CPUs the instance is bound to (live) or would be (planned, from the config)
    bool up = false;          // the instance answered
    bool live = false;        // the change reached the running instance without a restart
    std::string config_path;
};
SwarmCores swarm_cores_get(const Config & cfg);
// pin: -1 keep the configured value, 0 off, 1 on. n is clamped by validation to 1..nproc (an error otherwise).
SwarmCores swarm_cores_set(const Config & cfg, int n, int pin);

}  // namespace judas
