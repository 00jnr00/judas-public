// Judas configuration: ~/.config/judas/config.toml, overridden by ./.judas/config.toml, then by env
// (JUDAS_CONFIG = explicit file, JUDAS_PORT, JUDAS_DATA_DIR). Every number lives here, nothing buried in code.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace judas {

struct ModelConfig {
    std::string name;                // key in [models.<name>]
    std::string path;                // GGUF path (~ expanded)
    int gpu_layers = 99;
    bool cpu_moe = false;            // keep expert tensors on the CPU
    std::string kv_type = "q8_0";    // q8_0 | f16 | q5_1 | q5_0 | q4_1 | q4_0 (K and V)
    std::string kv_type_k, kv_type_v;  // optional per-tensor override of kv_type ("" = use kv_type)
    int ctx_total = 49152;          // unified KV pool shared by all agents
    int ctx_per_agent = 24576;       // soft cap for one agent's sequence (and the compaction trigger)
    // Hard cap used only while borrowing: a single prompt over ctx_per_agent may start if it fits under this
    // and the free KV pool can hold it without preempting anyone. 0 = off (today's behavior). Compaction still
    // triggers at ctx_per_agent, so conversations do not grow just because borrowing is enabled.
    int ctx_per_agent_max = 0;
    int threads = 4;
    int ubatch = 256;
    int batch = 2048;
    bool flash_attn = true;
    // sampling defaults for this model
    float temp = 1.0f, top_p = 0.95f, min_p = 0.0f;
    int top_k = 20;
    std::vector<std::pair<int, float>> logit_bias;  // token id -> bias (-inf bans a token)
    std::string bias_file;                          // optional file: "id-inf,id-inf,..." (token id, then a float or -inf)
    std::map<std::string, std::string> template_kwargs;  // chat template kwargs, e.g. reasoning_effort
    std::string chat_template_file;                 // optional jinja override
    // Remote model: when url is set, nothing is loaded in-process; chat goes to an OpenAI-compatible server
    // (e.g. a big model served on http://127.0.0.1:8081). unit = systemd user unit started on load and stopped when a
    // local model loads (so it frees the GPU). remote_model = model name sent upstream.
    std::string url, unit, remote_model;
};

struct EngineConfig {
    std::string model = "main";   // which [models.x] is loaded
    int max_agents = 8;           // live sequences (1..8); more agents queue
    int prefill_chunk = 256;      // tokens per prefill step
    double prefill_duty = 1.0;    // fraction of busy decode steps that may carry a prefill chunk
    int prefill_chunk_busy = 0;   // tokens per chunk while others generate; 0 = follow prefill_chunk
    int prefill_max_wait_ms = 2000;  // priority-0 prefill starvation guard
    int max_tokens = 8192;        // default generation cap per turn
    std::string park_dir = "/dev/shm/judas";
    int prefix_cache = 4;         // RAM snapshots of shared prompt prefixes (LRU)
    int kv_reserve = 512;         // KV tokens held back per live request for generation (admission)

    // prefill_chunk_busy defaults to prefill_chunk, so changing the common chunk size also moves the busy one.
    int busy_prefill_chunk() const { return prefill_chunk_busy > 0 ? prefill_chunk_busy : prefill_chunk; }
};

struct ServerConfig {
    std::string bind = "127.0.0.1";
    int port = 8079;              // OpenAI-compatible /v1 and the Judas API
    std::vector<std::string> extra_binds;  // extra addresses (no auth: trusted networks only)
    // Browser safety (there is no auth): requests must name this machine in Host (localhost, the bind
    // addresses, this hostname, or one listed here), and only these origins may call the API cross-origin.
    std::vector<std::string> allowed_hosts;  // e.g. "box.tailnet.ts.net"
    std::vector<std::string> cors_origins;   // e.g. "http://box:8010"; "*" allows any (not recommended)
};

struct PermissionsConfig {
    std::string mode = "auto-edit";                 // suggest | auto-edit | full-auto
    std::vector<std::string> allow_paths = {"~"};   // writes outside these are refused
    std::vector<std::string> deny_paths;            // always refused (globs, ~ expanded)
    std::vector<std::string> deny_commands;         // shell substrings refused
    // Never readable or writable by any tool, and hidden (empty) inside the sandbox. A plain entry covers the
    // directory and everything under it; an entry with * is an fnmatch glob on the full path.
    std::vector<std::string> secret_paths = {"~/.ssh", "~/.gnupg"};
    int shell_timeout_s = 120;
    // Shell confinement: none (run directly), or bwrap (bubblewrap namespace: root read-only, allow_paths
    // bound read-write, deny_paths hidden, a fresh /tmp). bwrap confines what a shell command can touch, so
    // a small model cannot scribble outside the allowed tree even in full-auto.
    std::string sandbox = "bwrap";                  // none | bwrap
    bool sandbox_net = true;                         // allow network inside the sandbox (web tools, package fetch)
    // MCP fail closed: a server that should be sandboxed (its own sandbox = true, the default) but cannot be
    // (bwrap missing, permissions.sandbox not "bwrap") refuses to start, unless this opt-in is set.
    bool allow_unsandboxed_mcp = false;
};

struct SearchConfig {
    std::string searxng = "http://127.0.0.1:8888";
    std::string fetch_command = "webfetch";         // ~/.local/bin/webfetch (falls back to Chromium)
};

struct AgentsConfig {
    int max_depth = 2;            // lead -> worker -> helper
    int max_steps = 100;          // model steps per agent turn; with open plan items the turn pauses here
    int max_auto_continues = 8;   // harness nudges per turn when a reply has no tool call and is not a final answer
    int loop_repeats = 3;         // same tool call or same tool error this many times in a row = looping
    int loop_pause_after = 2;     // loop warnings (close together) before the turn pauses
    int read_streak_nudge = 8;    // code/cowork: read-only calls in a row before a "write now" harness message (0 = off)
    int read_streak_force = 14;   // read-only calls in a row after which further reads are refused until a write/build (0 = off)
    int delegate_after_edits = 8; // lead: edits in a row, with 2+ open plan items, before a delegate reminder (0 = off)
    std::vector<std::string> role_dirs = {"~/.config/judas/roles"};  // plus built-in roles
    bool auto_resume = false;     // resume paused lead turns by itself (a session can override it)
    int auto_resume_max = 20;     // auto-resumes per session per day
    int auto_resume_delay_s = 30; // wait this long after a pause before resuming
};

// git-per-change: inside a git repo, each agent step's file changes also land as a commit on a task branch,
// so undo is one git command and runs are reviewable.
struct GitConfig {
    bool commit_per_edit = false;                   // off by default; on inside a repo if the user enables it
    std::string branch_prefix = "judas/";           // commits go on refs/<prefix><session-short>, never the user's branch
};

// Skills: Markdown files with YAML frontmatter (name, description, triggers). Only the one-line description is
// in the prompt; the body loads when a trigger matches or the model asks (the load_skill tool).
struct SkillsConfig {
    bool enabled = true;
    std::vector<std::string> paths = {"~/.config/judas/skills"};
    std::string autoload = "trigger";              // off | trigger | on
    bool manage = true;                            // register the skill_manage tool (agent-authored skills)
    bool track_usage = true;                       // record loads/patches in <data_dir>/skills.db
};

// Learning loop: after a lead turn, a hidden review agent looks for lessons worth keeping in memory or
// skills. Counters are per session and reset when a review fires. A threshold of 0 disables that nudge.
// The curator then keeps agent-authored skills healthy while the engine is idle.
struct LearningConfig {
    bool review = true;
    int review_tool_steps = 10;    // tool-calling steps since the last review (skill nudge)
    int review_memory_turns = 10;  // lead turns since the last review (memory nudge)
    int review_max_steps = 12;     // model steps the review agent may take
    bool curator = true;                     // master switch for the skills curator
    int curator_interval_hours = 168;        // min hours between automatic runs (a week)
    int curator_min_idle_minutes = 30;       // no active agent for this long before a run may start
    int stale_after_days = 30;               // days unused before an agent skill is marked stale
    int archive_after_days = 90;             // days unused before it moves into .archive/ (never deleted)
    int curator_backups = 5;                 // newest skills backups kept in <data_dir>/skill-backups/
};

// Hooks: user commands run at agent lifecycle events. Each entry: event + command (+ timeout, blocking).
struct Hook {
    std::string event;    // pre-tool | post-tool | pre-edit | post-edit | turn-end | task-done
    std::string command;  // shell command; receives JSON about the event on stdin
    int timeout_s = 30;
    bool blocking = false;  // if true and it exits non-zero on a pre-* event, the action is refused
    std::string match;      // optional: only fire when the tool name or path contains this substring
};
struct HooksConfig {
    bool enabled = false;
    std::vector<Hook> hooks;
};

// Context management: what to do as an agent's conversation approaches its context cap.
//   append  - never compact (fail when full); baseline for the A/B
//   compact - hybrid: drop/condense old tool output, replace big results with a one-line digest + a recall id
//             (the full text stays in the store, fetchable with the recall tool), then a staged summary of the
//             oldest turns only as a last resort. Keeps the last keep_turns turns and the first user message.
//   clm     - the model edits its own context (experimental; on a hybrid model this rolls the KV back to a
//             prompt checkpoint and re-prefills the kept tail)
struct ContextConfig {
    std::string mode = "compact";   // append | compact | clm
    double trigger_pct = 0.75;      // compact when ctx_used / ctx_per_agent crosses this
    double target_pct = 0.45;       // compact down to about this
    int keep_turns = 3;             // always keep the last N user/assistant turns verbatim
    bool recall = true;             // keep compacted tool output addressable via the recall tool
    int clm_max_edits = 4;          // clm: max self-edits per turn
};

// Prompt budget: keep the model's prompt lean.
struct PromptConfig {
    int max_memory_tokens = 3000;                   // cap on an appended memory block
    std::string tool_schema = "full";               // full | lazy (only the tools a step is likely to need)
};

// Memory: one Markdown file with stable "## " sections. Other assistants can share the same file.
struct MemoryConfig {
    std::string path = "~/.local/share/judas/memory.md";
    std::vector<std::string> inject_sections = {"About me", "Rules"};  // put in the system prompt
    bool reject_dashes = false;   // memory_append refuses facts with em or en dashes
};

// A notification channel (ntfy topic, or a user command that receives the message on stdin).
struct Channel {
    std::string name;
    std::string kind = "ntfy";   // ntfy | command
    std::string target;          // ntfy: topic URL (e.g. https://ntfy.sh/mytopic); command: the command
    bool enabled = true;
};
struct ChannelsConfig {
    std::vector<Channel> channels;
    std::string def;             // default channel name for heartbeat alerts
};

// A heartbeat collector: a named, read-only fact source. Built-ins by kind, plus kind "command" for a custom
// read-only script whose stdout becomes the fact text.
struct Collector {
    std::string name;
    std::string kind;            // units | http | cpu | ram | gpu | disk | dirgrowth | backups | command
    std::string arg;             // kind-specific: http url, disk path, dirgrowth path, command line, units glob
};
// Heartbeat: every interval, gather collectors read-only, ask a model to summarize and flag anything wrong,
// and push to a channel only when something needs attention. The model may be the local engine or a remote
// OpenAI-compatible endpoint (so it can run off the laptop when the GPU is busy).
struct HeartbeatConfig {
    bool enabled = false;
    int interval_s = 900;                  // 15 min
    std::string endpoint = "local";        // "local" (this daemon) or an OpenAI-compatible base URL
    std::string model = "";                // model name to send to a remote endpoint
    std::string checklist;                 // optional Markdown file of what to watch
    std::string quiet_hours;               // e.g. "23:00-07:00" (no pushes then unless critical)
    std::string channel;                   // channel name to push alerts to (else channels.def)
    std::vector<Collector> collectors;
};

// MCP client: tools that come from MCP servers (stdio) instead of being compiled in. See docs/MCP.md.
struct McpServerConfig {
    std::string name;                         // key in [mcp.servers.<name>]
    std::vector<std::string> command;         // argv; command[0] is searched in PATH, ~/.local/bin, /usr/local/bin
    std::map<std::string, std::string> env;   // extra environment (${VAR} and ~ expand); nothing else is inherited
    std::string cwd;                          // working directory ("" = the daemon's)
    bool enabled = true;
    int timeout_s = 60;                       // per tools/call
    int startup_timeout_s = 30;               // spawn + handshake + first tools/list
    std::vector<std::string> tools_allow;     // globs on the server's own tool names; empty = all
    std::vector<std::string> tools_deny;      // globs, win over tools_allow
    std::string prefix;                       // registry name prefix; "" with prefix_set=false means "<name>__"
    bool prefix_set = false;
    bool sandbox = true;                      // run under bwrap when permissions.sandbox = "bwrap" (read-only root)
    bool network = false;                     // allow network inside that sandbox
    std::vector<std::string> writable;        // paths the sandboxed server may write (everything else is read-only)
    bool trusted = false;                     // skip the approval prompt for tools that may change things
    std::string protocol = "auto";            // auto | legacy (initialize handshake) | modern (2026-07-28, stateless)
};
struct McpConfig {
    bool enabled = true;                      // master switch
    int max_tools_per_agent = 12;             // MCP tools one agent sees (0 = no cap); small models degrade with many
    int max_result_chars = 30000;             // tool result cap (head and tail kept, like the shell tool)
    std::vector<McpServerConfig> servers;     // alphabetical by name
    std::vector<std::string> ignored_project_servers;  // [mcp.servers.*] found in ./.judas/config.toml and dropped
};

struct Config {
    EngineConfig engine;
    std::map<std::string, ModelConfig> models;
    ServerConfig server;
    PermissionsConfig permissions;
    SearchConfig search;
    AgentsConfig agents;
    GitConfig git;
    SkillsConfig skills;
    LearningConfig learning;
    HooksConfig hooks;
    PromptConfig prompt;
    MemoryConfig memory;
    ContextConfig context;
    HeartbeatConfig heartbeat;
    ChannelsConfig channels;
    McpConfig mcp;
    std::string data_dir = "~/.local/share/judas";  // sessions.db, snapshots/, logs/
    std::vector<std::string> sources;               // files actually loaded, for `judas config show`

    const ModelConfig & active_model() const;       // throws if engine.model is not in models
};

// Load defaults <- user file <- project file <- env. Throws std::runtime_error with file:line on bad input.
Config load_config(const std::string & project_dir = ".");
// Problems that are not parse errors (missing GGUF, max_agents out of 1..8, unknown mode). Empty = valid.
std::vector<std::string> validate_config(const Config & cfg);
// Default config.toml text with comments (written by `judas init`).
std::string default_config_toml();
// Render the effective config as TOML (for `judas config show`).
std::string dump_config(const Config & cfg);

// "~/x" -> "$HOME/x"; leaves other paths alone.
std::string expand_home(const std::string & path);
// The user config file (JUDAS_CONFIG wins, else ~/.config/judas/config.toml).
std::string user_config_path();
// Set `model = "<name>"` in [engine] of a config file, keeping comments; atomic. False on I/O failure.
bool set_engine_model_in_file(const std::string & path, const std::string & model);

}  // namespace judas
