// Judas configuration: ~/.config/judas/config.toml, overridden by ./.judas/config.toml, then by env
// (JUDAS_CONFIG = explicit file, JUDAS_PORT, JUDAS_DATA_DIR). Every number lives here, nothing buried in code.
#pragma once

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace judas {

struct ModelConfig {
    std::string name;                // key in [models.<name>]
    std::string path;                // GGUF path (~ expanded)
    int gpu_layers = 99;
    // Where routed expert tensors live when the model does not fit VRAM: "auto" (default: as many of the LAST
    // layers on the GPU as fit, the rest in RAM; no effect on dense models or models that fit), "true" (all on
    // the CPU), "false" (all on the GPU, fails if too big), or a number N (experts of the first N layers on the CPU).
    std::string cpu_moe = "auto";
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
    // Vision. mmproj: projector GGUF path; "" = auto (an mmproj*.gguf next to the model), "none" = off.
    // mmproj_gpu: put the projector on the GPU (faster encode, costs VRAM). image_max_tokens caps tokens per image.
    // vision: for a remote model, whether the upstream accepts image_url parts.
    std::string mmproj;
    bool mmproj_gpu = false;
    int image_max_tokens = 1024;
    bool vision = false;
    // Backend. "llama" (default): libllama in-process. "strata": the Strata engine (docs/STRATA-BACKEND.md), run as
    // a child process; needs a build with -DJUDAS_STRATA=ON. `path` is then the first GGUF shard.
    std::string backend = "llama";
    // Strata only. Empty strings and zeros mean "use the default from docs/STRATA-BACKEND.md".
    std::string strata_pack;             // pack dir (dense.bin, tokenizer/, ...)
    std::string strata_mtp;              // MTP draft dir
    std::string strata_profile;          // expert profile; "" = the copy embedded in judas
    std::string strata_cwd;              // working dir of the engine (Strata's source tree)
    std::string strata_kv = "int8";      // KV cache type
    int strata_kv_resident = 32768;      // KV tokens kept in VRAM
    int strata_vram_reserve_mib = 3072;  // VRAM left free for the system
    int strata_resident_budget_gib = 0;  // RAM kept for experts the GPU cache does not hold (0 = Strata's default mode)
    int strata_spec = 4;                 // MTP draft length
    double strata_spec_min_p = 0.5;      // draft acceptance floor
    std::vector<std::string> strata_extra_args;  // appended to the engine command line verbatim
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
    bool pin_cores = false;       // pin the process to its first `threads` CPUs (whole cores of one CCD first)

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
    // Per-agent window. auto: a lone agent may use the whole pool (up to ctx_per_agent_max, else ctx_total) and the
    // budget splits only while other agents are live; compaction triggers against that effective cap. fixed: the
    // static ctx_per_agent, as before.
    std::string window = "auto";    // auto | fixed
    // Rule pinning: instruction files (AGENTS.md, CLAUDE.md, .judas/*.md, files named in the first user message)
    // keep their first pin_tokens tokens verbatim in one pinned message when compaction drops or summarizes them.
    int pin_tokens = 1200;          // per file
    int pin_total = 3000;           // all pinned files together; 0 = pinning off
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

// Learn mode: a tutoring session grounded in the learner's own files. context_files are READ ONLY for every
// agent; they are listed (paths or globs, ~ expanded) so the Learn lead gets a digest and a search tool.
struct LearnConfig {
    std::vector<std::string> context_files;   // empty = no grounding files
    int max_context_tokens = 3000;            // cap on the digest in the lead's prompt (~4 bytes per token)
    // Optional visual tutor (a separate web app, e.g. LearnHarness) shown inside the Learn screen. The web app
    // embeds visual_url; visual_unit, when set, is the systemd user unit Judas starts on demand.
    std::string visual_url;
    std::string visual_label = "Visual tutor";
    std::string visual_unit;
};

// Coach: propose-only review of a past session. A variant (prompt overlay, skills, tool notes) is written as
// plain files for the user to adopt by hand; adoption appends the overlay to the file below, which the agent
// prompt includes while enabled. Variants can never change permissions, the sandbox or the safety floor.
struct CoachConfig {
    bool enabled = false;
    std::string overlay;                  // adopted overlay file; "" = <data_dir>/coach/overlay.md
    int overlay_max_chars = 1200;         // cap on one variant's overlay text
    int tools_max_chars = 800;            // cap on one variant's tool notes (all together)
    int max_skills = 3;                   // skills one variant may carry
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

struct AttachmentsConfig {
    long long max_bytes = 50LL << 20;   // largest upload accepted
    int max_text_chars = 120000;        // extracted document text kept per attachment (head kept, rest cut)
    int pdf_max_pages = 6;              // scanned PDFs (no text layer): pages rendered to images for vision
    bool ocr = true;                    // run tesseract on images when it is installed (text for non-vision models)
};

struct UiConfig {
    bool mascot = true;                 // pixel-art companions in the TUI and web UI
};

// Swarm: bulk jobs fan out to a second, CPU-only Judas instance running small models (contrib/swarm.toml).
struct SwarmConfig {
    bool enabled = false;                           // registers the swarm_map tool
    std::string url = "http://127.0.0.1:8083";      // the swarm instance
    int max_parallel = 8;                           // concurrent requests to it
    int max_item_chars = 24000;                     // longer files are split into chunks of at most this size
    std::string config = "~/.config/judas/swarm.toml";  // the swarm instance's own config (cores are written there)
    std::string unit = "judas-swarm";               // systemd user unit the on/off switch starts and stops
};
// Swarm teams (docs/SWARM-TEAMS.md): the lead organizes members into teams with a pinned brief and house rules.
struct TeamsConfig {
    bool enabled = true;                            // team tools in Swarm-mode sessions
    std::string dir = "~/.config/judas/teams";      // saved team specs (*.toml), loaded with team_create {spec}
    int max_members = 16;                           // member copies per session, all teams together
    std::vector<std::string> house_rules;           // pinned into every member of every team
    int request_max_chars = 2000;                   // the user's request, pinned verbatim, is cut here
    int read_max_chars = 12000;                     // cap of one team_read
};
// Tools tab (web): each entry is a sub-tab. `url` is embedded in an iframe, `skill` adds a "Plan with Judas"
// side chat that loads that skill, `prompt_hint` is its placeholder.
struct ToolsTab {
    std::string name;         // id, [a-z0-9-]
    std::string label;        // shown on the sub-tab (defaults to name)
    std::string url;          // page to embed (optional)
    std::string skill;        // Judas skill to load in the side chat (optional)
    std::string prompt_hint;  // placeholder of the side chat (optional)
};
struct ToolsConfig {
    std::vector<ToolsTab> tabs;
};
// Built-in day planner server (compiled in with JUDAS_MACH12). Off unless enabled.
struct Mach12Config {
    bool enabled = false;                     // serve the page and API at /mach12/
    bool tools = true;                        // the lead agent's mach12_* planner tools (also need enabled)
    std::string data_dir = "~/.local/share/mach12";
    std::string merge_cmd = "mach12-merge";   // the plan-editing helper the mach12_* tools run (PATH lookup unless it has a slash)
};

struct Config {
    EngineConfig engine;
    AttachmentsConfig attachments;
    UiConfig ui;
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
    LearnConfig learn;
    CoachConfig coach;
    ContextConfig context;
    HeartbeatConfig heartbeat;
    ChannelsConfig channels;
    McpConfig mcp;
    SwarmConfig swarm;
    TeamsConfig teams;
    ToolsConfig tools;
    Mach12Config mach12;
    std::string data_dir = "~/.local/share/judas";  // sessions.db, snapshots/, logs/
    std::vector<std::string> sources;               // files actually loaded, for `judas config show`

    const ModelConfig & active_model() const;       // throws if engine.model is not in models
};

// Load defaults <- user file <- project file <- env. Throws std::runtime_error with file:line on bad input.
Config load_config(const std::string & project_dir = ".");
// Defaults plus one config file (no project file, no env); used to read another instance's config.
Config load_config_file(const std::string & file);
// Problems that are not parse errors (missing GGUF, max_agents out of 1..8, unknown mode). Empty = valid.
std::vector<std::string> validate_config(const Config & cfg);
// Default config.toml text with comments (written by `judas init`).
std::string default_config_toml();
// Render the effective config as TOML (for `judas config show`).
std::string dump_config(const Config & cfg);

// "~/x" -> "$HOME/x"; leaves other paths alone.
std::string expand_home(const std::string & path);
// True for a plain systemd unit name: [A-Za-z0-9@._-]+ (it ends up in a systemctl command line).
bool valid_unit_name(const std::string & s);
// The user config file (JUDAS_CONFIG wins, else ~/.config/judas/config.toml).
std::string user_config_path();
// Set `model = "<name>"` in [engine] of a config file, keeping comments; atomic. False on I/O failure.
bool set_engine_model_in_file(const std::string & path, const std::string & model);
// Same for any scalar: writes `key = raw_value` (raw TOML text, e.g. "true") under [section], creating it if absent.
bool set_toml_value_in_file(const std::string & path, const std::string & section, const std::string & key,
                            const std::string & raw_value);

}  // namespace judas

namespace judas {
// Hard per-request context cap: ctx_per_agent_max when set, else the whole pool while the window is auto,
// else 0 (borrowing off). The engine admits prompts up to this; the agent sizes compaction under it.
inline int ctx_hard_cap(const ModelConfig & m, const ContextConfig & c) {
    if (m.ctx_per_agent_max > 0) return m.ctx_per_agent_max;
    return c.window == "auto" ? m.ctx_total : 0;
}
// Effective per-agent cap given how many agents are live in the pool (this one included): the fair share of
// ctx_total, clamped between the soft ctx_per_agent and the hard cap. fixed = the soft cap.
inline int effective_ctx_cap(const ModelConfig & m, const ContextConfig & c, int live_agents) {
    int soft = m.ctx_per_agent;
    if (c.window != "auto") return soft;
    int hard = std::max(soft, ctx_hard_cap(m, c));
    int fair = m.ctx_total / std::max(1, live_agents);
    return std::clamp(fair, soft, hard);
}
}  // namespace judas
