// Agent manager: each agent runs its turns on its own thread and calls Engine::chat, so the engine batches
// every live agent together. Workers report back with their final answer; the lead waits, reviews, decides.

#include "agent/agent.h"

#include <algorithm>
#include "agent/gitstep.h"
#include "attach/attach.h"
#include "agent/policy.h"
#include "agent/teams.h"
#include "engine/remote_engine.h"
#include "channels/channels.h"
#include "coach/coach.h"
#include "context/compactor.h"
#include "hooks/hooks.h"
#include "learning/review.h"
#include "mcp/filter.h"
#include "mcp/manager.h"
#include "skills/skill_tools.h"
#include "skills/skills.h"
#include "skills/usage.h"
#include "tools/memory_tools.h"
#include "tutor/tutor.h"
#include "tools/swarm_tools.h"
#include "tools/session_tools.h"
#ifdef JUDAS_MACH12
#include "mach12/mach12_tools.h"
#endif

#include <unistd.h>

#include <optional>
#include <set>

#include <chrono>
#include <cmath>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace judas {

const char * version();

namespace fs = std::filesystem;

namespace {

// Appended to the lead's prompt in a Swarm-mode session.
const char * SWARM_PROMPT = R"(Swarm mode: bulk work is done by a swarm of small, fast models on the CPU through the swarm_map tool.
- Use the swarm for wide, shallow work: research (save the fetched pages to files, then have the swarm pull the facts and quotes relevant to the question from each), big documents (pass the file; it is chunked automatically, each chunk summarized or searched in parallel), folders of many files (summarize, tag, classify, extract fields), triage (score relevance, then read only the top items yourself) and record cleanup.
- Break bulk work (many files, records or snippets) into many small, independent items and send them with one swarm_map call: an instruction plus a list of files, globs or "text:..." strings. Items are answered in parallel and independently, so each must make sense alone.
- Write the swarm instruction like a precise spec: say exactly what to extract or decide and the exact output format (for example one JSON object per item). Try it on 3 to 5 items first, check the replies, then run the rest.
- Keep your own work to planning, spot-checking and merging the results. Full replies are in the JSONL file swarm_map names; read it in parts instead of pasting it into your context.
- Small models are good at extraction, short summaries, classification and tagging. They are not good at coding, long reasoning or anything that needs the whole picture; do those yourself.
- If swarm_map is missing or reports the swarm is off or down, tell the user to turn the swarm on with the switch in the Swarm tab or in Settings, or with `judas swarm on`.)";

// Appended to the lead's prompt in a Swarm-mode session when [teams] is on.
const char * TEAMS_PROMPT = R"(Teams: you are the only agent the user talks to. For big jobs, organize the work into teams of agents and run them.
- team_create makes a team: a name, a brief (goal, constraints, output format), house rules and members (id, role, count, model gpu or cpu, skills). Put rules that live in a file in rule_files as "FILE#Heading" so they are copied to every member verbatim; you never restate them in hand-offs. {spec: "name"} loads a saved team from the teams folder.
- A member with role lead is the team lead: you give it the team's whole task with team_assign and it splits the work among its members. Without a team lead you team_assign each member yourself (member "reader" picks a free copy, or name a copy like "reader.2").
- Give every assignment a complete, self-contained task: which files or items, what to produce, where to write it. Members cannot see your conversation.
- team_wait collects the reports (members also report automatically when you stop). Check the work, merge it, then answer the user with done. team_status shows who is doing what; team_read shows a member's latest output; team_message sends a note along an edge; team_disband stops a team.
- Saved maps: when a team finishes well (every copy done, and the user is satisfied or the result passed its checks), offer to save its map with a short name (team_save {name, note}); the note says what the team did and how it went. When a new job resembles a saved one, call team_list_saved first and start from it with team_create {spec: "name"}, then adjust the brief for the new job.
- Use model cpu only for wide, shallow reading (extraction, short summaries, triage); code, reasoning and merging stay on gpu members.
- Small jobs need no team: do them yourself.)";

const char * TEAM_LEAD_PROMPT = R"(Role: team lead. The session lead gave your team a task. Split it into pieces, team_assign one complete piece to each of your members, team_wait for their reports, check and merge them, and report up with done: what was done, what was checked, what is left.
- Members see the brief and house rules pinned by the harness; your assignment only needs the piece itself (files, items, output path).
- If a member's report is wrong or incomplete, team_assign it again with what to fix.
- Do the merge yourself; keep your own reading to what the merge needs.)";

const char * BASE_PROMPT = R"(You are Judas, a helpful assistant and agent running locally on the user's own machine.
Nothing the user shares leaves this computer.
Working directory: {cwd}
Date: {date}

Talking with the user:
- Match the request. A question, an explanation, writing help or a quick calculation needs no tools: answer
  directly in plain text. Use tools when the task needs files, the shell, the web or real checking.
- Be direct, warm and concise. Lead with the answer, then the detail that matters. Plain words over jargon
  unless the user writes technically.
- Homework and learning: teach, do not just hand over answers. Work step by step, show why each step holds, check
  the result (plug it back in, estimate, units), and point out the common mistake. If the user only wants the
  final answer, give it.
- Math: write formulas in LaTeX, $...$ inline and $$...$$ for display; the chat renders them.
- Office work (emails, letters, summaries, reports, tables): produce text ready to paste, in the user's voice and
  language. Use Markdown tables for tabular data. Ask one short question only when a missing detail would change
  the result (recipient, length, tone); otherwise make a sensible choice and say which.
- Never pad with disclaimers or restate the question.

Files the user attaches:
- <attachment name=".." kind="..">...</attachment> blocks hold the extracted text of a file the user attached (PDF,
  Word, Excel, PowerPoint, text, code). Treat it as the document itself. Quote or cite the page, sheet or section
  you rely on. If the text says it was cut, say what part you could not see.
- [[image:..|name]] marks an image or screenshot. When you can see images, look at it carefully: read all text in
  it, describe only what is there, and say when something is too small or blurry to read. When a note says the
  model cannot see the image, work from the OCR text given and say that you could not see the picture itself.
- Scanned PDF pages arrive as page images in order; read them as one document.
- view_image shows you an image file on disk; screenshot shows you the user's screen (ask first unless asked).

Working with tools:
- Look before you act. Read files before editing them. Prefer edit_file over rewriting whole files.
- Every file change is recorded in a snapshot journal. snapshot_list, snapshot_diff and snapshot_revert let you
  inspect and undo your own steps; use them when a change breaks something.
- Shell commands may have side effects a snapshot cannot undo (services, packages, network). Say so when you run one.
- You do not have root and must not seek it. sudo, su and credential commands are blocked. Never guess or brute
  force a password. If a task seems to need root, stop and tell the user; do not try to get around the block.
- Once you start a task with tools, keep going until it is finished: every reply is a tool call until then. Do not
  stop to announce what you will do next ("Let me look at X:"); make the call instead.
- Finish a tool task by calling done with a summary (it is your final answer). A plain-text reply with no tool call
  also ends your turn, but only when your plan has no open items.
- If the same command or error keeps coming back, stop repeating it: change your approach or ask for help.
- Ground every factual claim. Never explain a name, product, feature or file you have not verified: look it up
  first (memory_search, grep, read_file, web_search), and if you still cannot find it, say you do not know it and
  ask what the user means. A likely typo ("agenticx" for agentic, "supar" for super) is fine to read charitably,
  but say which reading you used. Never invent components, features, quotes or numbers.
- Questions about Judas itself: your source of truth is ~/src/judas (README.md, docs/DESIGN.md, docs/API.md,
  docs/MCP.md, docs/ATTACHMENTS.md). Read the relevant doc before answering; do not describe Judas from general
  knowledge.
- Your replies have an output limit. Write big files in parts: write_file the first part, then write_file with
  append=true for each next part (a few thousand characters each), or use edit_file in chunks.)";


const char * LEAD_PROMPT = R"(Role: lead agent. You own the user's request from start to finish.
- Small tasks: do them yourself.
- Larger tasks with independent parts: plan with update_plan, then spawn_agent one worker per independent
  subtask, then wait_agents for their reports. Spawn as many as the plan has independent parts; the engine
  queues workers when memory is tight, so you do not need to count slots. Give each worker a complete,
  self-contained task (files, constraints, how to check it). Workers start in your working directory: name
  files by relative path (src/x.py), never by retyping the absolute project path.
- Delegate, do not code module by module yourself: when the plan has two or more independent open items, your
  job is to spawn workers, review their reports and integrate. Keep edits for glue code and small fixes.
- After workers finish, check their work (read the files, run the tests, or spawn a reviewer) before you answer.
- For a multi-step task, keep a checklist with update_plan and tick items off as you go; it survives
  context compaction, so it is how you stay on track over a long task.
- Your final answer goes to the user: what was done, what was verified, what is left. Give it by calling done.)";

const char * WORKER_PROMPT = R"(Role: worker agent. You were given one self-contained task by a lead agent.
- Do the task with the tools you have. Do not ask questions; make reasonable choices and state them.
- Stay inside the task you were given. Do not expand its scope to reach a goal.
- If you hit a wall (a permission denied, a missing tool, a locked path, a command refused), STOP and report the
  wall to the lead in your final answer. Do not try to work around it, escalate privileges, or guess credentials.
  A refusal is a boundary to respect, not an obstacle to defeat. The lead or the user decides what happens next.
- Your final answer is your report to the lead: what you changed (files), what you checked, anything unfinished,
  and any wall you hit. Give it by calling done (or a plain-text reply with no tool call).)";

const char * REVIEWER_PROMPT = R"(Role: reviewer agent. Check work against the task you are given.
- Read the files, look at snapshot diffs, run read-only checks. Do not modify anything.
- Final answer: PASS or FAIL on the first line, then the concrete problems found, most serious first.)";

std::string replace_all(std::string s, const std::string & from, const std::string & to) {
    for (size_t p = 0; (p = s.find(from, p)) != std::string::npos; p += to.size()) s.replace(p, from.size(), to);
    return s;
}

std::string today() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d", std::localtime(&t));
    return buf;
}

std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

std::vector<std::string> split_list(std::string v) {
    v = trim(v);
    if (!v.empty() && v.front() == '[') v = v.substr(1, v.size() - 2);
    std::vector<std::string> out;
    std::stringstream ss(v);
    for (std::string item; std::getline(ss, item, ',');)
        if (auto t = trim(item); !t.empty()) out.push_back(t);
    return out;
}

const std::vector<std::string> READ_ONLY_TOOLS = {"read_file", "list_dir", "glob", "grep",
                                                  "snapshot_list", "snapshot_diff", "web_search", "web_fetch"};

// The review agent sees only these tools. session_search is included only when it is registered.
const std::vector<std::string> REVIEW_TOOLS = {"skill_manage", "load_skill",  "memory_read",
                                               "memory_search", "memory_append", "session_search"};
// Larger than any role priority: a review must never crowd out a user turn.
constexpr int REVIEW_PRIORITY = 1000000;

bool review_tool_allowed(const std::string & name) {
    return std::find(REVIEW_TOOLS.begin(), REVIEW_TOOLS.end(), name) != REVIEW_TOOLS.end();
}

}  // namespace

std::map<std::string, Role> load_roles(const Config & cfg) {
    std::map<std::string, Role> roles;
    roles["lead"] = {"lead", "Owns the request, plans, spawns workers, verifies", LEAD_PROMPT, {}, 0, -1};
    roles["worker"] = {"worker", "Does one self-contained task and reports", WORKER_PROMPT, {}, 1, -1};
    roles["team-lead"] = {"team-lead", "Leads one team: splits its task among the members, merges, reports up", TEAM_LEAD_PROMPT, {}, 1, -1};
    roles["reviewer"] = {"reviewer", "Checks work, read-only, PASS or FAIL", REVIEWER_PROMPT, READ_ONLY_TOOLS, 1, 20};
    roles["review"] = {"review", "Hidden background review of a finished lead turn", "", REVIEW_TOOLS, REVIEW_PRIORITY, -1};
    for (const auto & d : cfg.agents.role_dirs) {
        std::error_code ec;
        fs::path dir = expand_home(d);
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto & e : fs::directory_iterator(dir, ec)) {
            if (e.path().extension() != ".md") continue;
            std::ifstream f(e.path());
            std::string text((std::istreambuf_iterator<char>(f)), {});
            Role r;
            r.name = e.path().stem().string();
            if (text.rfind("---", 0) == 0) {
                size_t end = text.find("\n---", 3);
                if (end != std::string::npos) {
                    std::stringstream fm(text.substr(3, end - 3));
                    for (std::string line; std::getline(fm, line);) {
                        size_t c = line.find(':');
                        if (c == std::string::npos) continue;
                        std::string k = trim(line.substr(0, c)), v = trim(line.substr(c + 1));
                        if (k == "name") r.name = v;
                        else if (k == "description") r.description = v;
                        else if (k == "tools") r.tools = split_list(v);
                        else if (k == "priority") r.priority = std::stoi(v);
                        else if (k == "max_steps") r.max_steps = std::stoi(v);
                    }
                    text = text.substr(text.find('\n', end + 1) + 1);
                }
            }
            r.prompt = trim(text);
            roles[r.name] = r;
        }
    }
    return roles;
}

struct AgentManager::Impl {
    struct Agent {
        AgentRow row;
        Role role;
        int depth = 0;
        std::string cwd;
        std::vector<ChatMessage> messages;
        std::deque<std::string> inbox;   // user or lead messages waiting for the next turn
        bool running = false;
        bool clm_over = false;   // clm mode: a prune nudge has been issued for the current overflow
        std::string plan;        // the agent's running checklist, kept outside the chat so it survives compaction
        std::string reason;      // why the last turn paused or failed (empty otherwise)
        bool review = false;     // hidden background-review agent: no events, no hooks, restricted tools
        std::vector<std::string> review_writes;  // successful memory/skill writes, for the review event summary
        std::atomic<bool> cancel{false};
        std::set<std::string> delivered;  // child agents whose reports this agent has already received
        // Swarm teams. team/member/copy are empty for agents outside a team.
        std::string team, member, copy;
        std::string model = "gpu";       // gpu | cpu (runs on `eng`, the swarm instance)
        bool cpu_fallback = false;       // a cpu member running on the main model because the swarm is off
        Engine * eng = nullptr;          // null = the main engine
        int ctx_cap_override = 0;        // > 0: context cap for an agent on another engine
        std::string activity = "idle";   // queued | generating | tool | waiting | blocked | done | failed | idle (under mu)
        std::string activity_tool;       // the tool running while activity is tool (under mu)
        std::string last_text;           // tail of the latest assistant output, for team_read and the UI (under mu)
        double tok_s = 0;                // generation speed of the last model call
        double ctx_pct = -1;             // last prompt's share of this agent's context cap (-1 = unknown)
        bool pause_req = false;          // a cancel that should leave the agent paused, not killed
        std::deque<std::string> mail;    // team messages delivered at the next model step of a running agent
        std::thread thread;
    };
    // How one turn ended. status is the agent row status it maps to.
    struct TurnOutcome {
        std::string answer;
        std::string status = "done";   // done | paused | failed
        std::string reason;
    };
    // One team in a session (docs/SWARM-TEAMS.md). Lives in memory; a daemon restart forgets it.
    struct TeamRt {
        TeamSpec spec;
        std::string owner;                             // the session lead's agent id
        std::string cwd;
        std::string rules_text;                        // resolved house rules, pinned verbatim
        std::map<std::string, std::string> agents;     // copy -> agent id (created on first assignment)
        std::vector<std::string> warnings;             // map lines that were skipped when the team was loaded
        bool disbanded = false;
    };
    struct Session {
        std::string lead;
        std::string request;                           // the lead's current turn input, pinned for team members
        std::map<std::string, TeamRt> teams;
        int team_copies = 0;                           // member copies of live teams, against teams.max_members
        bool busy = false;
        std::string last_answer;
        ReviewCounters review;       // per-session nudge counters and the in-flight flag
        std::string review_id;       // the review agent currently running, if any
        struct Queued { std::string id, text, suffix; };
        std::deque<Queued> queued;   // user messages sent mid-turn, drained one turn at a time
        int next_q = 1;
    };

    const Config & cfg;
    Engine & engine;
    Store & store;
    Journal & journal;
    AttachmentStore attachments;
    std::map<std::string, Role> roles;
    ToolRegistry tools;
    Skills skills;
    // Per-session extras (coach probes): overlay text and an extra skill source. Own mutex; never held with mu.
    struct Extras {
        std::string overlay;
        std::shared_ptr<Skills> skills;
    };
    mutable std::mutex extras_mu;
    std::map<std::string, Extras> extras;
    std::shared_ptr<Skills> session_skills(const std::string & sid) const {
        std::lock_guard lk(extras_mu);
        auto it = extras.find(sid);
        return it == extras.end() ? nullptr : it->second.skills;
    }
    std::string session_overlay(const std::string & sid) const {
        std::lock_guard lk(extras_mu);
        auto it = extras.find(sid);
        return it == extras.end() ? std::string() : it->second.overlay;
    }
    bool has_extras(const std::string & sid) const {
        std::lock_guard lk(extras_mu);
        return extras.count(sid) > 0;
    }
    std::unique_ptr<SkillUsage> usage;   // null when skills.track_usage is off
    Hooks hooks;
    std::unique_ptr<mcp::Manager> mcp_;   // after `tools`: it removes its tools from the registry when it goes

    mutable std::mutex mu;
    std::condition_variable cv;
    std::map<std::string, std::unique_ptr<Agent>> agents;
    std::map<std::string, Session> sessions;
    std::map<int, AgentListener> listeners;
    int next_listener = 1;
    std::function<bool(const std::string &, const std::string &)> approver;
    bool stopping = false;
    // Remote approvals: pending and recently resolved, guarded by mu (cv wakes the waiting agent).
    struct PendingApproval {
        AgentManager::Approval info;
        int state = 0;   // 0 pending, 1 approved, 2 declined
        std::string by;  // user | cancelled | timeout | shutdown
    };
    bool remote_approvals = false;
    int approval_timeout_s = 600;
    int next_approval = 1;
    std::map<std::string, std::shared_ptr<PendingApproval>> approval_map;
    // Asks the user (or the approver hook) about one action. strict: the permission mode never answers.
    bool ask_user(Agent * a, const std::string & tool, const std::string & what, bool strict) {
        std::function<bool(const std::string &, const std::string &)> f;
        std::shared_ptr<PendingApproval> pa;
        {
            std::lock_guard lk(mu);
            f = approver;
            if (remote_approvals) {
                pa = std::make_shared<PendingApproval>();
                pa->info = {"ap-" + std::to_string(next_approval++), a->row.session, a->row.id, a->row.role, tool, what,
                            strict, now_ms()};
                approval_map[pa->info.id] = pa;
            }
        }
        if (!pa) {
            if (!f) return false;
            set_activity(a, "blocked", "approval");
            bool ok = f(a->row.session, what);
            set_activity(a, "tool");
            return ok;
        }
        set_activity(a, "blocked", "approval");
        emit(event(*a, "approval_request", {{"id", pa->info.id}, {"tool", tool}, {"what", what}, {"strict", strict},
                                            {"created_ms", pa->info.created_ms}}));
        bool announce = false;
        {
            std::unique_lock lk(mu);
            auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(approval_timeout_s, 0));
            while (pa->state == 0) {
                if (stopping) { pa->state = 2; pa->by = "shutdown"; announce = true; break; }
                if (a->cancel) { pa->state = 2; pa->by = "cancelled"; announce = true; break; }
                if (approval_timeout_s > 0 && std::chrono::steady_clock::now() >= deadline) {
                    pa->state = 2; pa->by = "timeout"; announce = true; break;
                }
                cv.wait_for(lk, std::chrono::milliseconds(200));
            }
        }
        if (announce)
            emit(event(*a, "approval_resolved", {{"id", pa->info.id}, {"approved", false}, {"by", pa->by}}));
        set_activity(a, "tool");
        std::lock_guard lk(mu);
        // keep the last few resolved ones so a late answer gets "already resolved" instead of "unknown"
        size_t resolved = 0;
        for (auto & [id, p] : approval_map) if (p->state != 0) resolved++;
        for (auto it = approval_map.begin(); resolved > 50 && it != approval_map.end();)
            if (it->second->state != 0) { it = approval_map.erase(it); resolved--; } else ++it;
        return pa->state == 1;
    }
    // The engine cpu team members run on (the swarm instance), made on first use. Tests inject a factory.
    std::function<std::unique_ptr<Engine>(const std::string & url)> remote_factory;
    std::unique_ptr<Engine> remote;
    std::mutex remote_mu;

    Impl(const Config & c, Engine & e, Store & s, Journal & j)
        : cfg(c), engine(e), store(s), journal(j), attachments(c), skills(c.skills), hooks(c.hooks) {
        roles = load_roles(cfg);
        if (cfg.skills.track_usage) {
            try {
                fs::path db = fs::path(expand_home(cfg.data_dir)) / "skills.db";
                usage = std::make_unique<SkillUsage>(db.string());
            } catch (...) {
                usage.reset();   // telemetry must never stop the agent from starting
            }
        }
        register_builtin_tools(tools);
        register_attach_tools(tools);
        register_memory_tools(tools);
        register_learn_tools(tools);
        register_session_tools(tools, store);
        register_agent_tools();
        register_team_tools();
        register_skill_tool();
        if (cfg.skills.enabled && cfg.skills.manage) register_skill_tools(tools, skills, usage.get());
        if (cfg.swarm.enabled) register_swarm_tool(tools);
        register_notify_tool();
        register_recall_tool();
        register_plan_tool();
        register_context_tools();
        register_done_tool();
#ifdef JUDAS_MACH12
        mach12::register_mach12_tools(tools);
#endif
        if (cfg.mcp.enabled && !cfg.mcp.servers.empty()) {
            mcp_ = std::make_unique<mcp::Manager>(cfg, tools);
            mcp_->start();   // servers connect in the background; their tools appear as they come up
        }
    }

    // notify: an agent flags you on a channel (ntfy, Matrix, or a command), e.g. when a long job is done.
    void register_notify_tool() {
        if (cfg.channels.channels.empty()) return;
        Tool t;
        t.name = "notify";
        t.description = "Send the user a short notification (e.g. when a long task is done or needs a decision). "
                        "Optional channel name; default channel is used otherwise.";
        t.parameters = {{"type", "object"},
                        {"properties",
                         {{"message", {{"type", "string"}}},
                          {"title", {{"type", "string"}}},
                          {"channel", {{"type", "string"}}}}},
                        {"required", {"message"}}};
        t.may_be_irreversible = true;  // it leaves the machine
        t.run = [this](const json & a, ToolContext &) -> ToolResult {
            Channels ch(cfg.channels);
            auto r = ch.send(a.value("channel", ""), a.value("title", "Judas"), a.value("message", ""));
            return {r.ok, r.ok ? "sent" : "notify failed: " + r.error};
        };
        tools.add(t);
    }

    ~Impl() {
        {
            std::lock_guard lk(mu);
            stopping = true;
            for (auto & [id, a] : agents) a->cancel = true;
        }
        cv.notify_all();
        for (auto & [id, a] : agents)
            if (a->thread.joinable()) a->thread.join();
    }

    void emit(const AgentEvent & ev) {
        // A review is hidden: it reports once with a "review" event and never streams its own turn.
        if (ev.role == "review" && ev.type != "review") return;
        std::vector<AgentListener> ls;
        {
            std::lock_guard lk(mu);
            for (auto & [h, f] : listeners) ls.push_back(f);
        }
        for (auto & f : ls) f(ev);
    }

    AgentEvent event(const Agent & a, const std::string & type, json data = json::object()) {
        return {type, a.row.session, a.row.id, a.row.parent, a.row.role, std::move(data)};
    }

    bool is_learn_session(const Agent & a) {
        if (a.review) return false;
        auto sess = store.get_session(a.row.session);
        return sess && sess->mode == "learn";
    }

    std::string system_prompt(const Agent & a) {
        std::string p = replace_all(replace_all(BASE_PROMPT, "{cwd}", a.cwd), "{date}", today());
        p += "\n\n" + a.role.prompt;
        if (a.role.name == "lead" && !a.review) {
            auto sess = store.get_session(a.row.session);
            if (sess && sess->mode == "swarm") {
                p += "\n\n" + std::string(SWARM_PROMPT);
                if (cfg.teams.enabled) p += "\n\n" + std::string(TEAMS_PROMPT);
            }
        }
        auto ps = session_skills(a.row.session);
        std::string sk = ps ? ps->index_block() : skills.index_block();
        if (!sk.empty()) p += "\n\n" + sk;
        std::string mem = memory_prompt_block(cfg);
        if (!mem.empty()) p += "\n\n" + mem;
        if (!a.review) {
            std::string co = coach_overlay_block(cfg);   // adopted coach proposals; "" unless [coach] enabled
            if (!co.empty()) p += "\n\n" + co;
            std::string xo = session_overlay(a.row.session);   // this session only (coach probe variant)
            if (!xo.empty()) p += "\n\n" + xo;
        }
        if (is_learn_session(a)) {
            p += "\n\n" + std::string(learn_prompt());
            std::string dg = learn_context_digest(cfg);
            if (!dg.empty()) p += "\n\nLearner context (read only, from their own files):\n\n" + dg;
        }
        return p;
    }

    // load_skill: the model reads a skill's full body on demand (only descriptions are in the prompt).
    void register_skill_tool() {
        if (!cfg.skills.enabled) return;
        Tool t;
        t.name = "load_skill";
        t.description = "Read the full instructions of a named skill (see the skills list in your prompt).";
        t.parameters = {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}, {"required", {"name"}}};
        t.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            auto ps = session_skills(ctx.session);
            auto s = ps ? ps->by_name(args.value("name", "")) : skills.by_name(args.value("name", ""));
            if (!s) return {false, "no such skill: " + args.value("name", "")};
            if (usage) usage->record_load(s->name, false);
            return {true, "# skill: " + s->name + "\n" + s->body};
        };
        tools.add(t);
    }

    // Autoload: skill bodies whose triggers match the user's message, prepended as a system note for this turn.
    std::string autoload_skills(const std::string & user_text, const std::string & sid) {
        if (cfg.skills.autoload != "trigger") return "";
        std::string out;
        auto ps = session_skills(sid);
        for (const auto & s : (ps ? ps->matching(user_text) : skills.matching(user_text))) {
#ifdef JUDAS_MACH12
            // The planner skill is only for sessions that have the planner tools (lead of chat or cowork, planner on).
            // There the day's context rides along, so the model does not need a first tool call to find its footing.
            if (s.name == "mach12") {
                auto sess = store.get_session(sid);
                if (!cfg.mach12.enabled || !cfg.mach12.tools || !sess || (sess->mode != "chat" && sess->mode != "cowork")) continue;
                if (usage) usage->record_load(s.name, true);
                out += "# skill: " + s.name + "\n" + s.body + "\n\nPlanner context right now (from mach12_today):\n" +
                       mach12::today_text(expand_home(cfg.mach12.data_dir), "today") + "\n\n";
                continue;
            }
#endif
            if (usage) usage->record_load(s.name, true);
            out += "# skill: " + s.name + "\n" + s.body + "\n\n";
        }
        return out;
    }

    // git-per-change: if enabled and the agent's cwd is in a repo, record the file the step changed as a commit
    // on refs/<prefix><session-short>. The user's branch, index and working tree are never touched (see gitstep.h).
    void record_git_step(const Agent & a, const std::string & label, const json & args) {
        if (!cfg.git.commit_per_edit) return;
        std::vector<std::string> paths;
        if (args.contains("path") && args["path"].is_string()) {
            fs::path p = expand_home(args["path"].get<std::string>());
            paths.push_back(p.is_absolute() ? p.string() : (fs::path(a.cwd) / p).string());
        }
        const std::string & sid = a.row.session;
        judas::git_commit_step(a.cwd, cfg.git.branch_prefix, sid.substr(sid.size() > 8 ? sid.size() - 8 : 0),
                               "judas: " + label, paths);
    }

    static std::string shq(const std::string & s) {
        std::string o = "'";
        for (char c : s) o += (c == '\'') ? "'\\''" : std::string(1, c);
        return o + "'";
    }

    std::vector<const Tool *> tools_for(const Agent & a) {
        std::vector<const Tool *> out;
        if (a.review) {
            for (const Tool * t : tools.list())
                if (review_tool_allowed(t->name)) out.push_back(t);
            return out;
        }
        bool can_spawn = a.depth < cfg.agents.max_depth;
        int mcp_seen = 0;
        TeamAccess ta = team_access(a);
        bool learn = is_learn_session(a);
        bool sealed;   // coach probe sessions (they carry session extras) must not write global skills or memory
        {
            std::lock_guard lk(extras_mu);
            sealed = extras.count(a.row.session) > 0;
        }
        for (const Tool * t : tools.list()) {
            if (sealed && (t->name == "skill_manage" || t->name == "memory_append")) continue;
#ifdef JUDAS_MACH12
            if (t->name.rfind("mach12_", 0) == 0 && !mach12_tools_allowed(a)) continue;
#else
            if (t->name.rfind("mach12_", 0) == 0) continue;
#endif
            bool team_tool = t->name.rfind("team_", 0) == 0;
            if (team_tool && !ta.allows(t->name)) continue;
            // cpu members run a small model: read-only tools, team tools and done only
            if (a.model == "cpu" && !a.cpu_fallback && !team_tool && t->name != "done" && t->name != "update_plan" &&
                std::find(READ_ONLY_TOOLS.begin(), READ_ONLY_TOOLS.end(), t->name) == READ_ONLY_TOOLS.end())
                continue;
            // Learn sessions get a read-only toolset; the learner tools exist only there
            if (learn ? !learn_tool_allowed(t->name) : (t->name == "learner_context" || t->name == "learn_progress")) continue;
            bool agent_tool = t->name.rfind("spawn_agent", 0) == 0 || t->name == "wait_agents" ||
                              t->name == "send_agent" || t->name == "list_agents";
            if (agent_tool && !can_spawn) continue;
            // the context-editing tools exist only in clm mode (compact prunes automatically instead)
            if ((t->name == "drop_context" || t->name == "note_context") && cfg.context.mode != "clm") continue;
            if (!a.role.tools.empty() && !agent_tool && !team_tool && t->name != "done" && !mcp::role_allows(a.role.tools, *t))
                continue;
            if (!t->source.empty()) {   // MCP tool: at most max_tools_per_agent per agent (small models degrade with many)
                if (cfg.mcp.max_tools_per_agent > 0 && mcp_seen >= cfg.mcp.max_tools_per_agent) continue;
                mcp_seen++;
            }
            out.push_back(t);
        }
        return out;
    }

#ifdef JUDAS_MACH12
    // The planner tools: the lead of a chat or cowork session, only while the module is enabled.
    bool mach12_tools_allowed(const Agent & a) {
        if (!cfg.mach12.enabled || !cfg.mach12.tools || a.review || a.role.name != "lead" || !a.row.parent.empty()) return false;
        auto sess = store.get_session(a.row.session);
        return sess && (sess->mode == "chat" || sess->mode == "cowork");
    }
#endif

    bool advertised(const Agent & a, const std::string & name) {
        for (const Tool * t : tools_for(a))
            if (t->name == name) return true;
        return false;
    }

    // A restricted role (non-empty tool list) may only spawn roles whose tools are a subset of its own, so
    // delegation can never widen what an agent is allowed to do. "" when allowed, else the reason.
    std::string spawn_denied(const Agent & parent, const std::string & role_name) {
        auto it = roles.find(role_name);
        if (it == roles.end()) return "unknown role '" + role_name + "'";
        if (parent.role.tools.empty()) return "";
        const Role & child = it->second;
        if (child.tools.empty()) return "role " + parent.role.name + " cannot spawn " + role_name + " (it has every tool)";
        for (const auto & t : child.tools)
            if (std::find(parent.role.tools.begin(), parent.role.tools.end(), t) == parent.role.tools.end())
                return "role " + parent.role.name + " cannot spawn " + role_name + " (it can use " + t + ")";
        return "";
    }

    Agent * create_agent(const std::string & session, const std::string & parent, const std::string & role_name,
                         const std::string & task, const std::string & cwd, int depth) {
        auto it = roles.find(role_name);
        Role role = it != roles.end() ? it->second : roles["worker"];
        auto a = std::make_unique<Agent>();
        a->row = store.create_agent(session, parent, role.name, task);
        a->role = role;
        a->depth = depth;
        a->cwd = cwd;
        a->messages.push_back({"system", system_prompt(*a)});
        store.add_message(session, a->row.id, "system", message_to_oai(a->messages.back()));
        Agent * raw = a.get();
        std::lock_guard lk(mu);
        agents[raw->row.id] = std::move(a);
        return raw;
    }

    // The review agent is a copy of the lead, not a fresh prompt: same system prompt and history means the
    // engine's prefix cache can reuse the lead's KV. The review prompt is the only new user message.
    Agent * create_review_agent(Agent * lead, const std::string & prompt) {
        auto it = roles.find("review");
        if (it == roles.end()) return nullptr;
        auto a = std::make_unique<Agent>();
        a->row = store.create_agent(lead->row.session, lead->row.id, "review", "background review");
        a->role = it->second;
        a->depth = lead->depth;
        a->cwd = lead->cwd;
        a->review = true;
        a->messages = lead->messages;
        a->messages.push_back({"user", prompt});
        Agent * raw = a.get();
        {
            std::lock_guard lk(mu);
            agents[raw->row.id] = std::move(a);
            raw->inbox.push_back(prompt);   // run() sees work; it does not append this one twice
        }
        return raw;
    }

    // One tool step with tools enabled counts toward the skill nudge. Only a lead's own steps count.
    void note_tool_step(Agent * a) {
        if (!cfg.learning.review || a->review || !a->row.parent.empty()) return;
        std::lock_guard lk(mu);
        sessions[a->row.session].review.tool_steps++;
    }

    void note_review_write(Agent * a, const std::string & what) {
        if (std::find(a->review_writes.begin(), a->review_writes.end(), what) == a->review_writes.end())
            a->review_writes.push_back(what);
    }

    std::string review_summary(const Agent & a, const TurnOutcome & out) {
        if (!a.review_writes.empty()) {
            std::string s;
            for (const auto & w : a.review_writes) s += (s.empty() ? "" : ", ") + w;
            return truncate_output(s, 300);
        }
        if (out.status == "failed") return "review failed";
        return "nothing to save";
    }

    // A completed lead turn may start one hidden review. The decision is pure (learning/review.h); this method
    // owns the per-session counters, the single-flight gate and the thread.
    void maybe_start_review(Agent * lead, const TurnOutcome & out) {
        if (!cfg.learning.review || lead->review || !lead->row.parent.empty()) return;
        if (has_extras(lead->row.session)) return;   // coach probe: no background review
        std::string prompt;
        {
            std::lock_guard lk(mu);
            auto & s = sessions[lead->row.session];
            if (out.status != "done" || lead->cancel) return;
            ReviewCounters c = s.review;
            c.turns++;
            ReviewDecision d = decide(c, cfg.learning, ReviewTurn::Done);
            if (!d.fire) {
                s.review = d.next;
                return;
            }
            // A user message already queued means the next turn cancels this review anyway. Keep the counters
            // so the nudge is not lost.
            if (!lead->inbox.empty()) {
                s.review = c;
                return;
            }
            prompt = review_prompt(d.memory, d.skills);
            if (prompt.empty()) return;
            s.review = d.next;
        }
        Agent * review = create_review_agent(lead, prompt);
        if (!review) {
            std::lock_guard lk(mu);
            sessions[lead->row.session].review.in_flight = false;
            return;
        }
        {
            std::lock_guard lk(mu);
            auto & s = sessions[lead->row.session];
            s.review_id = review->row.id;
            // The user may have sent a new turn while this agent was being built.
            if (!lead->inbox.empty() || lead->cancel) review->cancel = true;
        }
        start_turn(review);
    }

    // Start a turn on the agent's own thread if it is idle. Caller holds no lock.
    void start_turn(Agent * a) {
        std::lock_guard lk(mu);
        if (a->running || stopping) return;
        a->running = true;
        a->row.status = "running";
        a->row.updated_ms = now_ms();
        store.update_agent(a->row);  // the server's /events looks at the stored status
        if (a->thread.joinable()) a->thread.join();
        a->thread = std::thread([this, a] { run(a); });
    }

    json queue_json_locked(const std::string & sid) {
        json arr = json::array();
        auto s = sessions.find(sid);
        if (s != sessions.end())
            for (auto & q : s->second.queued) arr.push_back({{"id", q.id}, {"text", q.text}});
        return arr;
    }
    // Lead turn over: promote the next queued message into the lead's inbox (mu held). Not when the turn was
    // cancelled or paused (a pause keeps the queue). Returns the message to announce once mu is released.
    struct Drained { bool any = false; std::string content; };
    Drained drain_queue_locked(Agent * a) {
        Drained d;
        auto s = sessions.find(a->row.session);
        if (s == sessions.end() || s->second.lead != a->row.id || a->review || s->second.queued.empty()) return d;
        if (stopping || a->cancel || a->row.status == "paused") return d;
        auto q = s->second.queued.front();
        s->second.queued.pop_front();
        d.any = true;
        d.content = q.text + q.suffix;
        s->second.request = d.content;
        s->second.busy = true;
        for (auto & [id, ag] : agents)
            if (ag->row.session == a->row.session && ag->review) { ag->cancel = true; ag->inbox.clear(); }
        a->inbox.push_back(d.content);
        return d;
    }
    void announce_drained(Agent * a, const Drained & d) {
        if (!d.any) return;
        json q;
        { std::lock_guard lk(mu); q = queue_json_locked(a->row.session); }
        emit(event(*a, "queue", {{"queued", q}}));
        emit(event(*a, "spawned", {{"task", d.content}, {"role", "lead"}, {"from_queue", true}}));
    }

    void run(Agent * a) {
        while (true) {
            std::string input;
            Drained dq;
            {
                std::lock_guard lk(mu);
                if (a->inbox.empty()) dq = drain_queue_locked(a);   // a message queued after the last drain
                if (a->inbox.empty() || stopping || (a->cancel && a->pause_req)) {   // a paused agent keeps its inbox
                    a->running = false;
                    if (a->review) {
                        auto s = sessions.find(a->row.session);
                        if (s != sessions.end()) {
                            if (s->second.review_id == a->row.id) s->second.review_id.clear();
                            s->second.review.in_flight = false;
                        }
                    }
                    break;
                }
                input = a->inbox.front();
                a->inbox.pop_front();
            }
            announce_drained(a, dq);
            if (!a->review) {
                skills.reload_if_changed();   // pick up curator/human edits to the skills dir
                if (std::string sk = autoload_skills(input, a->row.session); !sk.empty())
                    a->messages.push_back({"system", "Relevant skill(s) for this request:\n\n" + sk});
                a->messages.push_back({"user", input});
                store.add_message(a->row.session, a->row.id, "user", message_to_oai(a->messages.back()));
            }
            bool canceled = a->cancel.load();
            TurnOutcome out = (a->review && canceled) ? TurnOutcome{"cancelled", "done", ""} : turn(a);
            std::string answer = out.answer;
            if (a->review) {
                canceled = a->cancel.load();
                std::string summary = review_summary(*a, out);
                {
                    std::lock_guard lk(mu);
                    a->row.result = summary;
                    a->row.status = canceled ? "killed" : out.status;
                    a->reason = canceled ? "cancelled" : out.reason;
                    a->row.updated_ms = now_ms();
                    store.update_agent(a->row);
                    auto s = sessions.find(a->row.session);
                    if (s != sessions.end()) {
                        if (s->second.review_id == a->row.id) s->second.review_id.clear();
                        s->second.review.in_flight = false;
                    }
                }
                if (!canceled) emit(event(*a, "review", {{"summary", summary}, {"status", out.status}}));
                // The agent row stays for the session board, but its copy of the lead's history is dead weight.
                std::vector<ChatMessage>().swap(a->messages);
                cv.notify_all();
                continue;
            }
            Drained end_dq;
            {
                std::lock_guard lk(mu);
                a->row.result = answer;
                // done | paused (a cap was hit with work left; resumable) | failed (engine error) | killed
                a->row.status = a->cancel ? (a->pause_req ? "paused" : "killed") : out.status;
                a->reason = a->cancel ? (a->pause_req ? "paused by the user" : "cancelled") : out.reason;
                a->row.updated_ms = now_ms();
                store.update_agent(a->row);
                a->activity = a->row.status == "done" ? "done" : a->row.status == "failed" ? "failed"
                            : a->row.status == "paused" ? "blocked" : "idle";
                a->activity_tool.clear();
                if (a->inbox.empty()) end_dq = drain_queue_locked(a);
                auto s = sessions.find(a->row.session);
                if (s != sessions.end() && s->second.lead == a->row.id && a->inbox.empty()) {
                    s->second.last_answer = answer;
                    s->second.busy = false;
                }
            }
            emit(event(*a, "activity", {{"activity", a->activity}}));
            emit(event(*a, "done", {{"result", answer}, {"status", a->row.status}, {"reason", a->reason}}));
            announce_drained(a, end_dq);
            if (!a->team.empty()) {   // a member's report travels up its edge (the map animates it)
                std::string to;
                {
                    std::lock_guard lk(mu);
                    auto s = sessions.find(a->row.session);
                    if (s != sessions.end())
                        if (auto t = s->second.teams.find(a->team); t != s->second.teams.end())
                            to = team_parent_of(t->second.spec, a->copy);
                }
                if (!to.empty())
                    emit(event(*a, "team", {{"action", "report"}, {"team", a->team}, {"copy", a->copy}, {"agent", a->row.id},
                                            {"from", a->copy}, {"to", to}, {"status", a->row.status}}));
            }
            maybe_start_review(a, out);
            cv.notify_all();
        }
        cv.notify_all();
    }

    // One turn: model call, tool calls, repeat until the model answers without tools.
    // Compact an agent's history when it nears the context cap (mode compact; append does nothing). The full
    // text of any tool result that gets pointer-dropped stays in the store, reachable with the recall tool.
    void maybe_compact(Agent * a, bool force = false) {
        if (cfg.context.mode == "append") return;
        int cap = 0;
        try {
            // window = auto: the cap is this agent's share of the pool right now (a lone agent gets all of it),
            // so a lead that grew big while alone is compacted down before its next request once workers start.
            int live = 0;
            {
                std::lock_guard lk(mu);
                for (auto & [id, other] : agents)
                    if (other->running) live++;
            }
            cap = a->ctx_cap_override > 0 ? a->ctx_cap_override
                                          : effective_ctx_cap(cfg.active_model(), cfg.context, std::max(1, live));
        } catch (...) {
            return;  // model not resolvable (e.g. a test with no models); nothing to size against
        }
        if (cap <= 0) return;
        auto count = [this, a](const std::string & s) { return eng_for(a).count_tokens(s); };
        int total = 0;
        // Count everything the rendered prompt carries, not just text: tool call arguments (whole files on a
        // write/edit), the tool schemas, and ~8 template tokens per message. Counting text only undercounted a
        // tool-heavy lead by 6k+, so compaction never fired and the turn died over ctx_per_agent.
        for (auto & m : a->messages) {
            total += count(m.content) + count(m.reasoning) + 8;
            for (auto & tc : m.tool_calls) total += count(tc.name) + count(tc.arguments) + 8;
        }
        for (const Tool * t : tools_for(*a)) total += count(t->name + t->description + t->parameters.dump());
        // clm mode (hybrid per the R&D): the model prunes its own context with drop_context/note_context, and
        // automatic compact stays the floor. Nudge once per overflow at the trigger; if it keeps climbing to the
        // hard ceiling (the model did not prune, or cannot), compact fires anyway so it never overflows.
        if (cfg.context.mode == "clm") {
            if (total < cfg.context.target_pct * cap) a->clm_over = false;
            if (total >= cfg.context.trigger_pct * cap && !a->clm_over) {
                int pct = (int) (100.0 * total / cap);
                a->messages.push_back(
                    {"system", "Your context is at " + std::to_string(pct) +
                                   "% of the limit. Prune it: call drop_context with the recall ids of earlier "
                                   "tool outputs you have already used (recoverable later with recall), and "
                                   "note_context to pin anything you must keep. Then continue."});
                a->clm_over = true;
            }
            if (!force && total < 0.9 * cap) return;  // below the ceiling: leave it to the model
        } else if (!force && total < cfg.context.trigger_pct * cap) {
            return;
        }
        auto summarize = [this, a](const std::vector<ChatMessage> & span) -> std::string {
            ChatRequest r;
            r.messages = span;
            r.messages.insert(r.messages.begin(),
                              {"system", "Summarize the earlier conversation below in under 200 words: the task, "
                                         "key decisions, files changed, and open threads."});
            r.messages.push_back({"user", "Write the summary now."});
            r.enable_thinking = false;
            r.sampling.max_tokens = 400;
            r.priority = a->row.parent.empty() ? 0 : 2;
            r.tag = "compact " + a->row.id;
            return eng_for(a).chat(r).message.content;
        };
        auto recall_ref = [](const ChatMessage & m) { return m.tool_call_id; };  // stored; recall tool fetches it
        auto res = compact(a->messages, cfg.context, cap, count, summarize, recall_ref);
        if (res.changed) {
            a->messages = res.messages;
            json d = json::object();
            d["compacted"] = true;
            d["before_tokens"] = res.before_tokens;
            d["after_tokens"] = res.after_tokens;
            d["recalled"] = res.recalled;
            d["summarized_turns"] = res.summarized_turns;
            emit(event(*a, "status", d));
        }
    }

    // Indices of messages that CLM must never drop: a leading system message, the first user message, and
    // everything from the last keep_turns user turns onward.
    std::set<size_t> protected_idx(const Agent & a) {
        std::set<size_t> keep;
        const auto & m = a.messages;
        if (!m.empty() && m[0].role == "system") keep.insert(0);
        std::vector<size_t> users;
        for (size_t i = 0; i < m.size(); i++)
            if (m[i].role == "user") users.insert(users.end(), i);
        if (!users.empty()) keep.insert(users.front());
        int kt = std::max(1, cfg.context.keep_turns);
        if ((int) users.size() > kt) {
            size_t tail = users[users.size() - kt];
            for (size_t i = tail; i < m.size(); i++) keep.insert(i);
        } else {
            for (size_t i = (users.empty() ? 0 : users.front()); i < m.size(); i++) keep.insert(i);
        }
        return keep;
    }

    // CLM: let the model prune its own context. drop_context removes tool results it no longer needs (their full
    // text stays in the store, recoverable with recall); note_context pins a short durable note. Guards keep the
    // system prompt, the first user message and the recent turns; edits are capped at context.clm_max_edits.
    void register_context_tools() {
        Tool drop;
        drop.name = "drop_context";
        drop.description = "Free up context by removing earlier tool outputs you no longer need, by their recall "
                           "id (tool_call_id). The full text stays recoverable with recall. Use when your context "
                           "is filling and you have already used those outputs.";
        drop.parameters = {{"type", "object"},
                           {"properties", {{"ids", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                           {"required", {"ids"}}};
        drop.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a) return {false, "no agent"};
            auto keep = protected_idx(*a);
            std::vector<std::string> ids = args.value("ids", std::vector<std::string>{});
            int dropped = 0, freed = 0;
            for (size_t i = 0; i < a->messages.size() && dropped < cfg.context.clm_max_edits;) {
                auto & msg = a->messages[i];
                bool hit = msg.role == "tool" &&
                           std::find(ids.begin(), ids.end(), msg.tool_call_id) != ids.end() && !keep.count(i);
                if (hit) {
                    freed += engine.count_tokens(msg.content);
                    a->messages.erase(a->messages.begin() + i);
                    keep = protected_idx(*a);  // indices shifted
                    dropped++;
                } else {
                    i++;
                }
            }
            return {true, "dropped " + std::to_string(dropped) + " output(s), ~" + std::to_string(freed) +
                              " tokens freed (recall by id to get them back)"};
        };
        tools.add(drop);

        Tool note;
        note.name = "note_context";
        note.description = "Pin a short durable note (a fact or decision) so it survives context pruning.";
        note.parameters = {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}}, {"required", {"text"}}};
        note.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a) return {false, "no agent"};
            a->messages.push_back({"system", "Pinned note: " + args.value("text", "")});
            return {true, "pinned"};
        };
        tools.add(note);
    }

    // done: the explicit way to end a turn. The summary is the agent's final answer (the lead's goes to the user,
    // a worker's is its report). The loop in turn() sees the call and finishes after the step's tool results.
    void register_done_tool() {
        Tool t;
        t.name = "done";
        t.description = "Finish your turn. Call it when the task is complete (or you cannot go on), with a summary "
                        "that is your final answer. Do not end a turn any other way while work is left.";
        t.parameters = {{"type", "object"}, {"properties", {{"summary", {{"type", "string"}}}}}, {"required", {"summary"}}};
        t.run = [](const json &, ToolContext &) -> ToolResult { return {true, "ok"}; };
        tools.add(t);
    }

    // update_plan: the agent keeps a running checklist that lives outside the chat and survives compaction.
    void register_plan_tool() {
        Tool t;
        t.name = "update_plan";
        t.description = "Set or update your running checklist for a multi-step task (markdown, one item per line, "
                        "mark done items). It stays visible and survives context compaction. Keep it current.";
        t.parameters = {{"type", "object"}, {"properties", {{"plan", {{"type", "string"}}}}}, {"required", {"plan"}}};
        t.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a) return {false, "no agent"};
            a->plan = args.value("plan", "");
            return {true, "plan updated"};
        };
        tools.add(t);
    }

    // recall: fetch the full text of a tool result that compaction replaced with a digest + id.
    void register_recall_tool() {
        Tool t;
        t.name = "recall";
        t.description = "Fetch the full output of an earlier tool call that was shortened, by its recall id "
                        "(the id shown in a 'recall id=...' note). Long outputs come in parts: pass offset to continue.";
        t.parameters = {{"type", "object"},
                        {"properties", {{"id", {{"type", "string"}}},
                                        {"offset", {{"type", "integer"}, {"description", "character offset to start at (default 0)"}}}}},
                        {"required", {"id"}}};
        t.run = [this](const json & a, ToolContext & ctx) -> ToolResult {
            auto out = store.tool_output(ctx.session, a.value("id", ""));
            if (!out) return {false, "no stored output for recall id " + a.value("id", "")};
            // One part fits well under the per-result cap, so a recall never comes back cut.
            size_t part = 20000;
            try {
                int cap = cfg.active_model().ctx_per_agent;
                if (cap > 0) part = std::clamp<size_t>((size_t) cap * 3 / 4, 2000, 20000);
            } catch (...) {}
            long long off = std::max(0LL, a.value("offset", 0LL));
            size_t from = std::min((size_t) off, out->size());
            while (from > 0 && from < out->size() && ((unsigned char) (*out)[from] & 0xC0) == 0x80) from--;
            if (from == 0 && out->size() <= part) return {true, *out};
            size_t to = std::min(out->size(), from + part);
            while (to < out->size() && ((unsigned char) (*out)[to] & 0xC0) == 0x80) to++;
            std::string r = out->substr(from, to - from);
            r += "\n[recall: chars " + std::to_string(from) + "-" + std::to_string(to) + " of " + std::to_string(out->size()) +
                 (to < out->size() ? "; call recall with offset=" + std::to_string(to) + " for more]" : "]");
            return {true, r};
        };
        tools.add(t);
    }

    // Keep the agent's plan as one pinned system message right after the base system prompt, refreshed every
    // step and re-created if compaction ever dropped it. This is a common coding-agent pattern: the plan
    // lives outside the chat so it survives compaction and keeps a long task on track.
    void sync_plan(Agent * a) {
        if (a->plan.empty()) return;
        std::string block = "[plan] current checklist (keep it updated with update_plan):\n" + a->plan;
        size_t at = (!a->messages.empty() && a->messages[0].role == "system") ? 1 : 0;
        for (size_t i = 0; i < a->messages.size(); i++)
            if (a->messages[i].role == "system" && a->messages[i].content.rfind("[plan] ", 0) == 0) {
                a->messages[i].content = block;  // refresh in place
                return;
            }
        a->messages.insert(a->messages.begin() + at, {"system", block});
    }

    // A turn runs until the model finishes explicitly (the done tool, or a plain answer while the plan has no open
    // items). A reply without a tool call that is not a finish gets a short harness nudge instead of ending the
    // turn; caps (auto-continues, steps, loops) end it as "paused" so a scheduler can resume it. The decisions
    // themselves are pure functions in policy.h.
    TurnOutcome paused(Agent * a, const std::string & reason) {
        TurnOutcome o;
        o.status = "paused";
        o.reason = reason;
        o.answer = "[paused] " + reason;
        if (!a->plan.empty()) o.answer += "\nPlan:\n" + a->plan;
        emit(event(*a, "status", {{"turn_status", "paused"}, {"reason", reason}}));
        return o;
    }

    // Children this agent spawned that are still working (running or with queued input).
    bool has_live_children(const Agent & a) {
        std::lock_guard lk(mu);
        for (auto & [id, c] : agents)
            if (c->row.parent == a.row.id && (c->running || !c->inbox.empty())) return true;
        return false;
    }

    // A lead that answers without a tool call while its workers still run would otherwise burn auto-continues and
    // pause. Instead the harness waits for the workers itself and hands over every report not yet delivered.
    // Returns false if nothing was waited for (no live children) or the agent was cancelled meanwhile.
    bool auto_wait_children(Agent * a) {
        if (!has_live_children(*a)) return false;
        emit(event(*a, "status", {{"waiting", "workers"}}));
        set_activity(a, "waiting");
        json reports = json::array();
        {
            std::unique_lock lk(mu);
            cv.wait(lk, [&] {
                if (stopping || a->cancel) return true;
                for (auto & [id, c] : agents)
                    if (c->row.parent == a->row.id && (c->running || !c->inbox.empty())) return false;
                return true;
            });
            if (stopping || a->cancel) return false;
            for (auto & [id, c] : agents) {
                if (c->row.parent != a->row.id || a->delivered.count(id)) continue;
                a->delivered.insert(id);
                reports.push_back({{"agent", id}, {"role", c->row.role}, {"status", c->row.status},
                                   {"report", c->row.result}});
            }
        }
        nudge(a, "[harness] Your worker agents have finished; their reports follow. Check their work (run the "
                 "tests), fix or reassign what is wrong, then call done.\n" + reports.dump(2),
              "auto-wait for workers");
        return true;
    }

    void nudge(Agent * a, const std::string & text, const std::string & why) {
        a->messages.push_back({"user", text});
        emit(event(*a, "status", {{"nudge", why}, {"text", text}}));
    }

    TurnOutcome turn(Agent * a) {
        int max_steps = a->review ? std::max(1, cfg.learning.review_max_steps)
                                  : (a->role.max_steps > 0 ? a->role.max_steps : cfg.agents.max_steps);
        auto tlist = tools_for(*a);
        bool can_spawn = a->depth < cfg.agents.max_depth;
        bool is_lead = a->row.parent.empty();
        TurnState ts;
        LoopGuard guard(cfg.agents.loop_repeats, cfg.agents.loop_pause_after);
        DelegationState dstate;
        ReadStreak rstreak;
        bool streak_on = false;   // read-streak policy: code/cowork sessions only, not chat
        if (!a->review)
            if (auto sess = store.get_session(a->row.session)) streak_on = sess->mode != "chat";
        bool forced_compact = false;
        for (int step = 0;; step++) {
            maybe_compact(a);
            sync_plan(a);
            sync_team(a);
            drain_mail(a);
            bool last = step >= max_steps;
            if (last && plan_state(a->plan).open > 0)
                return paused(a, "step limit (" + std::to_string(max_steps) + ") reached with plan items still open");
            if (!last) note_tool_step(a);   // a model step with tools enabled counts toward the skill nudge
            ChatRequest req;
            req.messages = a->messages;
            expand_request_images(attachments, req.messages, engine.vision());   // stored history keeps the markers
            if (!last)
                for (const Tool * t : tlist) {
                    // lazy tool schema: drop the occasional tools (web, snapshots) from the advertised set to
                    // keep the prompt small; the model still has the core edit/read/shell/agent tools.
                    if (cfg.prompt.tool_schema == "lazy" &&
                        (t->name.rfind("web_", 0) == 0 || t->name.rfind("snapshot_", 0) == 0))
                        continue;
                    req.tools.push_back({t->name, t->description, t->parameters});
                }
            if (last) req.messages.push_back({"user", "Step limit reached. Answer now with what you have, no tools."});
            req.priority = a->review ? REVIEW_PRIORITY : (a->row.parent.empty() ? 0 : a->role.priority);
            req.tag = a->row.role + " " + a->row.id;
            set_activity(a, "queued");
            bool streaming = false;
            auto res = eng_for(a).chat(
                req,
                [&](const ChatDelta & d) {
                    if (!streaming) {
                        streaming = true;
                        set_activity(a, "generating");
                    }
                    if (!d.reasoning.empty()) emit(event(*a, "reasoning", {{"text", d.reasoning}}));
                    if (!d.content.empty()) emit(event(*a, "delta", {{"text", d.content}}));
                    return !a->cancel.load();
                },
                &a->cancel);
            if (res.generate_s > 0) a->tok_s = res.completion_tokens / res.generate_s;
            try {
                int cap = a->ctx_cap_override > 0 ? a->ctx_cap_override : cfg.active_model().ctx_per_agent;
                if (cap > 0 && res.prompt_tokens > 0) a->ctx_pct = std::min(100.0, 100.0 * res.prompt_tokens / cap);
            } catch (...) {}
            if (!res.message.content.empty()) {
                std::lock_guard lk(mu);
                a->last_text = res.message.content.size() > 12000 ? res.message.content.substr(res.message.content.size() - 12000)
                                                                  : res.message.content;
            }
            a->row.prompt_tokens += res.prompt_tokens - res.cached_tokens;
            a->row.completion_tokens += res.completion_tokens;
            if (res.finish_reason == "error" && res.error.find("over ctx_per_agent") != std::string::npos &&
                !forced_compact) {
                forced_compact = true;  // backstop if the estimate still ran short: compact now and retry once
                maybe_compact(a, true);
                continue;
            }
            if (res.finish_reason != "error") forced_compact = false;  // the backstop may fire again later in the turn
            if (res.finish_reason == "error") {
                emit(event(*a, "status", {{"error", res.error}}));
                return {"error: " + res.error, "failed", res.error};
            }
            ChatMessage msg = res.message;
            msg.role = "assistant";
            ReplyKind kind = classify_reply(msg.content, !msg.tool_calls.empty(), res.finish_reason);
            bool cut_tool_call = kind == ReplyKind::Truncated &&
                                 (!msg.tool_calls.empty() || msg.content.find("<tool_call>") != std::string::npos ||
                                  msg.content.find("<function=") != std::string::npos);
            if (kind == ReplyKind::Truncated) {
                // Never run a cut-off call, and do not keep a huge half-written argument in the history.
                msg.tool_calls.clear();
                msg.reasoning.clear();
                if (msg.content.size() > 1500) msg.content = msg.content.substr(0, 1500) + "\n[...cut off]";
            }
            a->messages.push_back(msg);
            // Per-call stats ride along in the stored body (judas_stats; messages_from_oai ignores unknown keys) so the
            // web chat can show a turn footer after a reload, not only while the turn streams.
            json usage = {{"prompt", res.prompt_tokens}, {"cached", res.cached_tokens}, {"completion", res.completion_tokens},
                          {"prefill_s", res.prefill_s}, {"generate_s", res.generate_s}, {"queue_s", res.queue_s}};
            if (!a->review) {
                json body = message_to_oai(msg);
                body["judas_stats"] = usage;
                store.add_message(a->row.session, a->row.id, "assistant", body);
            }
            emit(event(*a, "message", {{"message", message_to_oai(msg)}, {"usage", usage}}));
            if (res.finish_reason == "cancelled") return {msg.content, "done", ""};
            if (last) {
                finish_hooks(a, msg.content);
                return {msg.content, "done", ""};
            }
            if (msg.tool_calls.empty() && kind != ReplyKind::Truncated && auto_wait_children(a)) continue;
            Decision d = should_continue(kind, plan_state(a->plan), ts, cfg.agents.max_auto_continues,
                                         res.completion_tokens);
            if (d.action == Action::Pause) return paused(a, d.reason);
            if (d.action == Action::Finish) {
                finish_hooks(a, msg.content);
                return {msg.content, "done", ""};
            }
            if (d.action == Action::Continue) {
                nudge(a, cut_tool_call ? truncation_nudge(true, res.completion_tokens) : d.nudge, d.reason);
                continue;
            }

            std::string finish_summary;
            bool finished = false;
            bool loop_pause = false;
            std::vector<std::string> notes;  // harness messages, added after all tool results of this step
            LoopGuard::Result loop_warn;
            for (const auto & tc : msg.tool_calls) {
                json args;
                bool bad_args = false;
                try {
                    args = tc.arguments.empty() ? json::object() : json::parse(tc.arguments);
                } catch (...) {
                    args = json::object();
                    bad_args = true;
                }
                emit(event(*a, "tool_call", {{"id", tc.id}, {"name", tc.name}, {"args", args}}));
                set_activity(a, "tool", tc.name);
                std::string label = tc.name + " " + (args.contains("path") ? args["path"].dump()
                                                     : args.contains("command") ? args["command"].dump()
                                                                                : std::string());
                ToolContext ctx;
                ctx.cwd = a->cwd;
                ctx.cfg = &cfg;
                ctx.journal = &journal;
                ctx.session = a->row.session;
                ctx.agent = a->row.id;
                ctx.autonomous = a->review || has_extras(a->row.session);   // probe sessions: no user to consent
                ctx.progress = [this, a](const json & d) { emit(event(*a, "swarm", d)); };
                ctx.checkpoint = journal.begin(a->row.session, a->row.id, label.substr(0, 200));
                if (!a->review) ctx.approve = [this, a, tool = tc.name](const std::string & what) {
                    if (cfg.permissions.mode == "full-auto") return true;
                    return ask_user(a, tool, what, false);
                };
                if (!a->review && !ctx.autonomous) ctx.approve_strict = [this, a, tool = tc.name](const std::string & what) {
                    return ask_user(a, tool, what, true);
                };
                bool writes = false;
                if (const Tool * td = tools.find(tc.name)) writes = td->writes;
                std::string hook_event = writes ? "pre-edit" : "pre-tool";
                std::string selector = tc.name + " " + (args.contains("path") ? args.value("path", "") : "");
                std::string hook_block;
                if (!a->review) {
                    auto h = hooks.run(hook_event, selector,
                                       {{"tool", tc.name}, {"args", args}, {"session", a->row.session},
                                        {"agent", a->row.id}, {"cwd", a->cwd}});
                    if (!h.allow) hook_block = "blocked by a " + hook_event + " hook: " + h.reason;
                }
                CallKind ckind = classify_call(tc.name, tc.name == "shell" ? args.value("command", "") : "", writes);
                ToolResult r;
                if (a->cancel) {
                    r = {false, "cancelled", false};
                } else if (a->review && !review_tool_allowed(tc.name)) {
                    r = {false, "a background review may only use memory and skill tools", false};
                } else if (!advertised(*a, tc.name)) {
                    // The schema list is not a permission check by itself: a model can name any registered tool.
                    r = {false, "the tool '" + tc.name + "' is not available to this agent (role " + a->row.role + ")", false};
                } else if (!hook_block.empty()) {
                    r = {false, hook_block, false};
                } else if (streak_on && ckind == CallKind::Read &&
                           read_streak_blocked(rstreak, cfg.agents.read_streak_force)) {
                    r = {false, "Refused: " + std::to_string(rstreak.count) +
                                    " read-only calls in a row. Reading is blocked until you write a file "
                                    "(write_file / edit_file) or run a build/test command. Nothing ran.", false};
                } else if (bad_args) {
                    r = {false, "Your arguments were not valid JSON (" + std::to_string(tc.arguments.size()) +
                                    " chars; likely too long or badly escaped). Nothing ran. For a big file, write a "
                                    "short version first, then add the rest with write_file append=true or several "
                                    "smaller edits.", false};
                } else {
                    r = tools.call(tc.name, args, ctx);
                    if (!a->review)
                        hooks.run(writes ? "post-edit" : "post-tool", selector,
                                  {{"tool", tc.name}, {"ok", r.ok}, {"session", a->row.session}, {"cwd", a->cwd}});
                }
                journal.commit(ctx.checkpoint);
                if (r.ok && writes && !a->review) record_git_step(*a, label.substr(0, 120), args);
                ChatMessage tm;
                tm.role = "tool";
                tm.content = r.output;
                tm.tool_call_id = tc.id;
                tm.tool_name = tc.name;
                // The store keeps the full output (recall returns it); the context gets at most ~30% of
                // ctx_per_agent, so one huge result cannot push the prompt over the cap where compaction,
                // which always keeps the newest message, could not bring it back.
                if (!a->review) store.add_message(a->row.session, a->row.id, "tool", message_to_oai(tm));
                try {
                    int cap = a->ctx_cap_override > 0 ? a->ctx_cap_override : cfg.active_model().ctx_per_agent;
                    if (cap > 0) tm.content = cap_tool_result(tm.content, (size_t) cap * 9 / 10, a->review ? "" : tc.id);
                } catch (...) {}
                a->messages.push_back(tm);
                emit(event(*a, "tool_result", {{"id", tc.id}, {"name", tc.name}, {"ok", r.ok},
                                               {"output", r.output.substr(0, 4000)}, {"checkpoint", ctx.checkpoint}}));

                if (a->review && r.ok) {
                    if (tc.name == "memory_append") {
                        note_review_write(a, "memory");
                    } else if (tc.name == "skill_manage") {
                        std::string action = args.value("action", "");
                        if (action == "create" || action == "patch" || action == "rewrite" || action == "archive" ||
                            action == "pin" || action == "unpin")
                            note_review_write(a, "skill " + action + " " + args.value("name", ""));
                    }
                }
                if (streak_on) {
                    auto ev = read_streak_observe(rstreak, ckind, cfg.agents.read_streak_nudge,
                                                  cfg.agents.read_streak_force);
                    if (ev != StreakEvent::None) notes.push_back(read_streak_message(ev, rstreak.count));
                }
                if (tc.name == "done" && r.ok) {
                    finished = true;
                    finish_summary = args.value("summary", "");
                }
                static const std::set<std::string> repeat_ok = {"done", "update_plan", "wait_agents", "list_agents",
                                                                "recall", "note_context", "drop_context"};
                auto lr = guard.observe(tc.name, tc.arguments, r.ok, writes, r.output, !repeat_ok.count(tc.name));
                if (lr.verdict == LoopGuard::Verdict::Pause) loop_pause = true;
                if (lr.verdict != LoopGuard::Verdict::Ok && loop_warn.verdict == LoopGuard::Verdict::Ok) loop_warn = lr;
                bool delegation = tc.name == "spawn_agent" || tc.name == "wait_agents" || tc.name == "send_agent";
                if (is_lead && delegation_nudge(dstate, writes && tc.name != "snapshot_revert", delegation,
                                                plan_state(a->plan).open, cfg.agents.delegate_after_edits, can_spawn))
                    notes.push_back("[harness] You have made several edits in a row yourself while your plan has "
                                    "open items. If those items are independent, delegate them now: spawn_agent one "
                                    "worker per item with a complete task, then wait_agents. Keep doing it yourself "
                                    "only if the remaining work is small or tightly coupled.");
            }
            if (a->cancel) return {"cancelled", "done", ""};
            if (finished) {
                if (finish_summary.empty()) finish_summary = msg.content;
                finish_hooks(a, finish_summary);
                return {finish_summary, "done", ""};
            }
            if (loop_pause)
                return paused(a, "looping: the same " + loop_warn.tool + " " +
                                     (loop_warn.kind == "call" ? "call" : "error") + " kept repeating after a warning");
            if (loop_warn.verdict == LoopGuard::Verdict::Warn) notes.push_back(loop_nudge(loop_warn, can_spawn));
            for (const auto & n : notes) nudge(a, n, "harness");
        }
    }

    void finish_hooks(Agent * a, const std::string & answer) {
        if (a->review) return;   // a hidden review must not fire task-done or turn-end
        std::string ev = a->row.parent.empty() ? "task-done" : "turn-end";
        hooks.run(ev, a->row.role,
                  {{"session", a->row.session}, {"agent", a->row.id}, {"role", a->row.role},
                   {"answer", answer}, {"cwd", a->cwd}});
    }

    Agent * find(const std::string & id) {
        std::lock_guard lk(mu);
        auto it = agents.find(id);
        return it == agents.end() ? nullptr : it->second.get();
    }

    // The tools that let an agent run other agents. Workers inherit the caller's session and cwd.
    void register_agent_tools() {
        Tool spawn;
        spawn.name = "spawn_agent";
        spawn.description = "Start a helper agent on the same model with its own task. Returns its id at once; "
                            "use wait_agents to get its report. Roles: worker (default), reviewer (read-only check).";
        spawn.parameters = {{"type", "object"},
                            {"properties",
                             {{"task", {{"type", "string"}, {"description", "complete, self-contained task"}}},
                              {"role", {{"type", "string"}, {"description", "worker | reviewer | custom role name"}}}}},
                            {"required", {"task"}}};
        spawn.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * parent = find(ctx.agent);
            if (!parent) return {false, "unknown calling agent"};
            if (std::string why = spawn_denied(*parent, args.value("role", "worker")); !why.empty()) return {false, why};
            Agent * child = create_agent(ctx.session, ctx.agent, args.value("role", "worker"), args.value("task", ""),
                                         ctx.cwd, parent->depth + 1);
            {
                std::lock_guard lk(mu);
                child->inbox.push_back(args.value("task", ""));
            }
            emit(event(*child, "spawned", {{"task", args.value("task", "")}, {"role", child->row.role}}));
            start_turn(child);
            return {true, json({{"agent", child->row.id}, {"role", child->row.role}, {"status", "running"}}).dump()};
        };
        tools.add(spawn);

        Tool wait;
        wait.name = "wait_agents";
        wait.description = "Wait until the given agents finish and return their reports.";
        wait.parameters = {{"type", "object"},
                           {"properties",
                            {{"agents", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                             {"timeout_s", {{"type", "integer"}, {"description", "default 1800"}}}}},
                           {"required", {"agents"}}};
        wait.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            std::vector<std::string> ids = args.value("agents", std::vector<std::string>{});
            int timeout = args.value("timeout_s", 1800);
            Agent * self = find(ctx.agent);
            if (self) set_activity(self, "waiting");
            std::unique_lock lk(mu);
            bool ok = cv.wait_for(lk, std::chrono::seconds(timeout), [&] {
                if (stopping || (self && self->cancel)) return true;
                for (auto & id : ids) {
                    auto it = agents.find(id);
                    if (it != agents.end() && (it->second->running || !it->second->inbox.empty())) return false;
                }
                return true;
            });
            json out = json::array();
            if (self)
                for (auto & id : ids) self->delivered.insert(id);
            for (auto & id : ids) {
                auto it = agents.find(id);
                if (it == agents.end()) out.push_back({{"agent", id}, {"error", "unknown agent"}});
                else
                    out.push_back({{"agent", id}, {"role", it->second->row.role}, {"status", it->second->row.status},
                                   {"report", it->second->row.result}});
            }
            return {ok, ok ? out.dump(2) : "timeout; partial: " + out.dump(2)};
        };
        tools.add(wait);

        Tool send;
        send.name = "send_agent";
        send.description = "Send a follow-up message to an agent you spawned (it starts another turn). "
                           "Use wait_agents afterwards for its new report.";
        send.parameters = {{"type", "object"},
                           {"properties", {{"agent", {{"type", "string"}}}, {"message", {{"type", "string"}}}}},
                           {"required", {"agent", "message"}}};
        send.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(args.value("agent", ""));
            if (!a || a->row.session != ctx.session) return {false, "unknown agent"};
            {
                std::lock_guard lk(mu);
                a->cancel = false;  // a worker cancelled earlier can take new work
                a->inbox.push_back(args.value("message", ""));
            }
            start_turn(a);
            return {true, "sent"};
        };
        tools.add(send);

        Tool list;
        list.name = "list_agents";
        list.description = "List agents in this session with status and role.";
        list.parameters = {{"type", "object"}, {"properties", json::object()}};
        list.run = [this](const json &, ToolContext & ctx) -> ToolResult {
            json out = json::array();
            std::lock_guard lk(mu);
            for (auto & [id, a] : agents)
                if (a->row.session == ctx.session)
                    out.push_back({{"agent", id}, {"role", a->row.role}, {"parent", a->row.parent},
                                   {"status", a->running ? "running" : a->row.status}});
            return {true, out.dump(2)};
        };
        tools.add(list);
    }

    // ---------- Swarm teams (docs/SWARM-TEAMS.md) ----------

    // Which team tools an agent sees. The session lead of a Swarm-mode session gets all of them; a team lead can
    // assign and wait inside its own team; every member can check status, message along an edge and read.
    struct TeamAccess {
        bool create = false, assign = false, member = false;
        bool allows(const std::string & n) const {
            if (n == "team_create" || n == "team_disband" || n == "team_save" || n == "team_list_saved") return create;
            if (n == "team_assign" || n == "team_wait") return assign;
            if (n == "team_status" || n == "team_message" || n == "team_read") return member || create;
            return false;
        }
    };
    TeamAccess team_access(const Agent & a) {
        TeamAccess t;
        if (!cfg.teams.enabled || a.review) return t;
        if (a.row.parent.empty()) {
            auto sess = store.get_session(a.row.session);
            if (sess && sess->mode == "swarm") t.create = t.assign = true;
            return t;
        }
        if (a.team.empty()) return t;
        t.member = true;
        t.assign = a.row.role == "team-lead" && a.depth < cfg.agents.max_depth;
        return t;
    }

    // Record what an agent is doing and tell the UI when it changes.
    void set_activity(Agent * a, const std::string & act, const std::string & tool = "") {
        if (a->review) return;
        {
            std::lock_guard lk(mu);
            if (a->activity == act && a->activity_tool == tool) return;
            a->activity = act;
            a->activity_tool = tool;
        }
        json d = {{"activity", act}};
        if (!tool.empty()) d["tool"] = tool;
        emit(event(*a, "activity", d));
    }

    Engine & eng_for(const Agent * a) { return a->eng ? *a->eng : engine; }

    // The engine cpu members run on, or null when the swarm is off or does not answer.
    Engine * remote_engine() {
        if (!swarm_is_on(cfg)) return nullptr;
        std::lock_guard lk(remote_mu);
        if (!remote) remote = remote_factory ? remote_factory(cfg.swarm.url) : std::make_unique<RemoteEngine>(cfg.swarm.url);
        if (!remote || !remote->state().loaded) return nullptr;
        return remote.get();
    }

    // The pinned team block, refreshed before every model step and put back if compaction dropped it.
    void sync_team(Agent * a) {
        if (a->team.empty()) return;
        TeamBlockInput in;
        TeamSpec spec;
        {
            std::lock_guard lk(mu);
            auto s = sessions.find(a->row.session);
            if (s == sessions.end()) return;
            auto t = s->second.teams.find(a->team);
            if (t == s->second.teams.end()) return;
            spec = t->second.spec;
            in.rules_text = t->second.rules_text;
            in.request = s->second.request;
        }
        in.spec = &spec;
        in.copy = a->copy;
        in.request_max = (size_t) std::max(0, cfg.teams.request_max_chars);
        in.cpu_fallback = a->cpu_fallback;
        std::string block = team_block(in);
        for (auto & m : a->messages)
            if (m.role == "system" && m.content.rfind(TEAM_BLOCK_PREFIX, 0) == 0) {
                m.content = block;
                return;
            }
        size_t at = (!a->messages.empty() && a->messages[0].role == "system") ? 1 : 0;
        a->messages.insert(a->messages.begin() + at, {"system", block});
    }

    // Team mail that arrived while the agent was running goes in before its next model step.
    void drain_mail(Agent * a) {
        std::deque<std::string> got;
        {
            std::lock_guard lk(mu);
            got.swap(a->mail);
        }
        for (auto & m : got) nudge(a, m, "team message");
    }

    // Who the calling agent is inside a team: "lead" for the session lead, else its copy id. Empty = not allowed.
    std::string team_identity(const Agent & a, const TeamRt & t) {
        if (a.row.parent.empty() && a.row.id == t.owner) return "lead";
        if (a.team == t.spec.name) return a.copy;
        return "";
    }

    // Resolve a team name for the caller: explicit, the caller's own team, or the only team of the session.
    // Caller holds mu.
    TeamRt * find_team(const std::string & sid, const Agent & a, const std::string & name, std::string & err) {
        auto s = sessions.find(sid);
        if (s == sessions.end() || s->second.teams.empty()) {
            err = "this session has no teams; create one with team_create";
            return nullptr;
        }
        std::string want = !name.empty() ? name : a.team;
        if (want.empty()) {
            int live = 0;
            for (auto & [n, t] : s->second.teams)
                if (!t.disbanded) {
                    live++;
                    want = n;
                }
            if (live != 1) {
                err = "name the team (teams:";
                for (auto & [n, t] : s->second.teams)
                    if (!t.disbanded) err += " " + n;
                err += ")";
                return nullptr;
            }
        }
        auto t = s->second.teams.find(want);
        if (t == s->second.teams.end()) {
            err = "no team named '" + want + "'";
            return nullptr;
        }
        if (!a.team.empty() && a.team != want) {
            err = "you can only use your own team (" + a.team + ")";
            return nullptr;
        }
        return &t->second;
    }

    json agent_live(const Agent & a, const std::map<std::string, std::string> & phases) {
        AgentRow r = a.row;
        if (a.running) r.status = "running";
        std::string ph;
        if (auto it = phases.find(a.row.role + " " + a.row.id); it != phases.end()) ph = it->second;
        std::string tail = a.last_text.size() > 600 ? a.last_text.substr(a.last_text.size() - 600) : a.last_text;
        while (!tail.empty() && ((unsigned char) tail[0] & 0xC0) == 0x80) tail.erase(0, 1);
        return {{"id", r.id}, {"session", r.session}, {"parent", r.parent}, {"role", r.role}, {"task", r.task},
                {"status", r.status}, {"result", r.result}, {"created_ms", r.created_ms}, {"updated_ms", r.updated_ms},
                {"prompt_tokens", r.prompt_tokens}, {"completion_tokens", r.completion_tokens},
                {"team", a.team}, {"member", a.member}, {"copy", a.copy},
                {"activity", a.eng ? team_status_merge(a.activity, "") : team_status_merge(a.activity, ph)},
                {"tool", a.activity_tool}, {"depth", a.depth}, {"model", a.model}, {"tok_s", a.tok_s},
                {"ctx_pct", a.ctx_pct < 0 ? json(nullptr) : json(std::round(a.ctx_pct * 10) / 10)},
                {"model_name", a.eng ? a.eng->model_name() : engine.model_name()},
                {"engine", a.eng ? "cpu" : "gpu"}, {"cpu_fallback", a.cpu_fallback},
                {"last_output", tail}};
    }

    // Engine phase per request tag: prefill | generate | queue.
    std::map<std::string, std::string> engine_phases() {
        std::map<std::string, std::string> out;
        EngineState st = engine.state();
        for (auto & q : st.queue) out[q.tag] = "queue";
        for (auto & sq : st.seqs)
            if (!sq.tag.empty() && sq.phase != "idle") out[sq.tag] = sq.phase;
        return out;
    }

    std::string resolve_rules(const TeamSpec & spec, const std::string & cwd, const ToolContext & ctx, std::string & err) {
        std::string out;
        for (auto & r : cfg.teams.house_rules) out += "- " + r + "\n";
        for (auto & r : spec.rules) out += "- " + r + "\n";
        for (auto & ref : spec.rule_files) {
            std::string resolved;
            ToolContext c = ctx;
            c.cwd = cwd;
            if (std::string why = check_read_path(c, team_rule_path(ref), resolved); !why.empty()) {
                err = "rule_files " + ref + ": " + why;
                return "";
            }
            std::ifstream f(resolved);
            if (!f) {
                err = "rule_files " + ref + ": cannot read " + team_rule_path(ref);
                return "";
            }
            std::string text((std::istreambuf_iterator<char>(f)), {});
            std::string sec = team_rule_text(ref, text);
            if (sec.empty()) {
                err = "rule_files " + ref + ": no heading '" + ref.substr(ref.find('#') + 1) + "' in " + team_rule_path(ref);
                return "";
            }
            out += (out.empty() ? "" : "\n") + sec + "\n";
        }
        while (!out.empty() && out.back() == '\n') out.pop_back();
        return out;
    }

    // Save a team's map into the user teams dir. `caller` empty = from the web (no agent). Returns "" and fills
    // `path_out` on success, else the reason.
    std::string save_team(const std::string & sid, const std::string & team, const std::string & name, const std::string & note,
                          bool overwrite, std::string & path_out, std::string & used_team) {
        TeamSpec spec;
        TeamSaveInfo info;
        {
            std::lock_guard lk(mu);
            auto s = sessions.find(sid);
            if (s == sessions.end() || s->second.teams.empty()) return "this session has no teams";
            const TeamRt * t = nullptr;
            if (!team.empty()) {
                auto it = s->second.teams.find(team);
                if (it == s->second.teams.end()) return "no team named '" + team + "'";
                t = &it->second;
            } else {
                int live = 0;
                for (auto & [n, r] : s->second.teams)
                    if (!r.disbanded) { live++; t = &r; }
                if (live != 1) {
                    std::string e = "name the team (teams:";
                    for (auto & [n, r] : s->second.teams)
                        if (!r.disbanded) e += " " + n;
                    return e + ")";
                }
            }
            spec = t->spec;
            used_team = spec.name;
            info.session = sid;
            long long first = 0, last = 0;
            for (auto & [copy, id] : t->agents) {
                auto it = agents.find(id);
                if (it == agents.end()) continue;
                const AgentRow & r = it->second->row;
                info.agents++;
                info.tokens += r.prompt_tokens + r.completion_tokens;
                if (r.status == "failed" || r.status == "killed") info.failed++;
                if (r.created_ms && (!first || r.created_ms < first)) first = r.created_ms;
                last = std::max(last, std::max(r.updated_ms, r.created_ms));
            }
            if (first && last > first) info.wall_s = (last - first) / 1000.0;
        }
        info.date = today();
        info.note = note;
        std::string dir = team_dir_resolve(cfg.teams.dir);
        std::string err = team_save_file(dir, name, team_save_text(spec, info), overwrite);
        if (!err.empty()) return err;
        path_out = (fs::path(dir) / (name + ".map")).string();
        return "";
    }

    void register_team_tools() {
        Tool create;
        create.name = "team_create";
        create.description = "Create a team of agents with a shared brief and house rules the harness pins into every "
                             "member (they survive compaction; never restate them in hand-offs). Members start on their "
                             "first team_assign. Easiest: pass map, the team as plain lines (team NAME / brief TEXT / "
                             "rules FILE#Heading / rule TEXT / lead ID / member ID x3 cpu / link A -> B observe / "
                             "place 1: ID ID). Or spec to load a saved team by name, or the JSON fields.";
        json member_schema = {{"type", "object"},
                              {"properties",
                               {{"id", {{"type", "string"}}},
                                {"role", {{"type", "string"}, {"description", "lead (team lead, at most one) | worker | reviewer"}}},
                                {"count", {{"type", "integer"}, {"description", "copies id.1..id.N (1 to 8)"}}},
                                {"model", {{"type", "string"}, {"description", "gpu (default) | cpu (small fast model, read-only work)"}}},
                                {"skills", {{"type", "array"}, {"items", {{"type", "string"}}}}}}},
                              {"required", {"id"}}};
        create.parameters = {
            {"type", "object"},
            {"properties",
             {{"name", {{"type", "string"}}},
              {"brief", {{"type", "string"}, {"description", "goal, constraints, output format"}}},
              {"rules", {{"type", "array"}, {"items", {{"type", "string"}}}}},
              {"rule_files", {{"type", "array"}, {"items", {{"type", "string"}}}, {"description", "\"FILE\" or \"FILE#Heading\", pinned verbatim"}}},
              {"members", {{"type", "array"}, {"items", member_schema}}},
              {"edges", {{"type", "array"}, {"items", {{"type", "object"}, {"properties", {{"from", {{"type", "string"}}}, {"to", {{"type", "string"}}}, {"kind", {{"type", "string"}, {"description", "message | observe"}}}}}}}}},
              {"spec", {{"type", "string"}, {"description", "load a saved team (teams folder, NAME.map or NAME.toml)"}}},
              {"map", {{"type", "string"}, {"description", "the team(s) as map lines, one fact per line"}}},
              {"place", {{"type", "array"}, {"items", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}}}};
        create.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a || !team_access(*a).create) return {false, "only the session lead of a Swarm session can create teams"};
            std::vector<TeamSpec> specs;
            std::vector<std::string> warnings;
            json j = args;
            auto from_map = [&](const std::string & text, const std::string & name) -> std::string {
                TeamMap m = parse_team_map(text, name);
                warnings = m.warnings;
                if (m.teams.empty()) return "the map has no team in it (start with: team NAME, then member lines)";
                specs = m.teams;
                return "";
            };
            if (j.contains("spec")) {
                std::string name = j.value("spec", "");
                bool ok = !name.empty() && name.size() <= 40;
                for (char c : name) ok = ok && (std::isalnum((unsigned char) c) || c == '-' || c == '_');
                if (!ok) return {false, "spec must be a saved team name (a-z, 0-9, _ or -)"};
                fs::path dir = team_dir_resolve(cfg.teams.dir);
                fs::path pm = dir / (name + ".map"), pt = dir / (name + ".toml");
                if (std::ifstream f(pm); f) {
                    std::string text((std::istreambuf_iterator<char>(f)), {});
                    if (auto e = from_map(text, name); !e.empty()) return {false, pm.string() + ": " + e};
                } else if (std::ifstream g(pt); g) {
                    std::string text((std::istreambuf_iterator<char>(g)), {});
                    TeamSpec one;
                    if (std::string e = team_spec_from_toml(text, one); !e.empty()) return {false, pt.string() + ": " + e};
                    specs.push_back(one);
                } else {
                    return {false, "no saved team " + pm.string() + " (or .toml)"};
                }
                if (specs.size() == 1 && j.contains("name") && j["name"].is_string()) specs[0].name = j["name"].get<std::string>();
            } else if (j.contains("map")) {
                if (!j["map"].is_string()) return {false, "map must be the map text (one fact per line)"};
                if (auto e = from_map(j["map"].get<std::string>(), j.value("name", std::string("team"))); !e.empty())
                    return {false, e};
            } else {
                TeamSpec one;
                if (std::string e = team_spec_from_json(j, one); !e.empty()) return {false, e};
                specs.push_back(one);
            }
            // validate every team first (names unique, room left), then create them all
            int used;
            {
                std::lock_guard lk(mu);
                auto & s = sessions[a->row.session];
                used = s.team_copies;
                for (auto & sp : specs)
                    if (s.teams.count(sp.name) && !s.teams[sp.name].disbanded)
                        return {false, "a team named '" + sp.name + "' already exists; disband it or pick another name"};
            }
            std::vector<std::string> rules(specs.size());
            for (size_t i = 0; i < specs.size(); i++) {
                for (size_t k = 0; k < i; k++)
                    if (specs[k].name == specs[i].name) return {false, "the map names team '" + specs[i].name + "' twice"};
                int left = std::max(0, cfg.teams.max_members - used);
                if (std::string e = validate_team_spec(specs[i], left, a->depth, cfg.agents.max_depth); !e.empty())
                    return {false, "team " + specs[i].name + ": " + e};
                used += (int) team_all_copies(specs[i]).size();
                std::string err;
                rules[i] = resolve_rules(specs[i], a->cwd, ctx, err);
                if (!err.empty()) return {false, "team " + specs[i].name + ": " + err};
            }
            std::string out;
            for (size_t i = 0; i < specs.size(); i++) {
                const TeamSpec & spec = specs[i];
                {
                    std::lock_guard lk(mu);
                    auto & s = sessions[a->row.session];
                    TeamRt t;
                    t.spec = spec;
                    t.owner = a->row.id;
                    t.cwd = a->cwd;
                    t.rules_text = rules[i];
                    t.warnings = warnings;
                    s.teams[spec.name] = std::move(t);
                    s.team_copies += (int) team_all_copies(spec).size();
                }
                emit(event(*a, "team", {{"action", "created"}, {"team", spec.name}, {"spec", team_spec_to_json(spec)}}));
                std::string tl = team_lead_of(spec);
                std::string copies_s;
                for (auto & c : team_all_copies(spec)) copies_s += (copies_s.empty() ? "" : ", ") + c;
                out += "team " + spec.name + " created: " + copies_s + ". Pinned rules: " + std::to_string(rules[i].size()) +
                       " characters. " +
                       (tl.empty() ? "Assign each member its piece with team_assign (member id or copy)."
                                   : "Give the team's task to its team lead with team_assign member=" + tl + ".") +
                       "\n";
            }
            out += "\nThe team as loaded (canonical map):\n";
            for (auto & sp : specs) out += team_map_text(sp) + "\n";
            if (!warnings.empty()) {
                out += "Skipped map lines (fix them next time):\n";
                for (auto & w : warnings) out += "- " + w + "\n";
            }
            return {true, out};
        };
        tools.add(create);

        Tool assign;
        assign.name = "team_assign";
        assign.description = "Give a team member a complete, self-contained task (it starts, or continues if it already "
                             "worked). member is a member id (picks a free copy) or a copy like reader.2.";
        assign.parameters = {{"type", "object"},
                             {"properties", {{"team", {{"type", "string"}}}, {"member", {{"type", "string"}}},
                                             {"task", {{"type", "string"}}}}},
                             {"required", {"member", "task"}}};
        assign.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a || !team_access(*a).assign) return {false, "you cannot assign team work"};
            std::string task = args.value("task", ""), want = args.value("member", "");
            if (task.empty()) return {false, "task is empty"};
            std::string copy, existing, team_name, cwd, role_name, model = "gpu", from;
            std::vector<std::string> skills;
            {
                std::lock_guard lk(mu);
                std::string err;
                TeamRt * t = find_team(a->row.session, *a, args.value("team", ""), err);
                if (!t) return {false, err};
                if (t->disbanded) return {false, "team " + t->spec.name + " was disbanded"};
                from = team_identity(*a, *t);
                if (from.empty()) return {false, "you are not in team " + t->spec.name};
                team_name = t->spec.name;
                cwd = t->cwd;
                // a member id with copies picks the first copy that is free (never started, or idle)
                std::vector<std::string> cands;
                if (const TeamMemberSpec * m = team_member(t->spec, want)) {
                    cands = team_copies(*m);
                } else if (!team_member_of(t->spec, want).empty()) {
                    cands = {want};
                } else {
                    std::string all;
                    for (auto & c : team_all_copies(t->spec)) all += " " + c;
                    return {false, "no member '" + want + "' in team " + team_name + " (copies:" + all + ")"};
                }
                for (auto & c : cands) {
                    auto it = t->agents.find(c);
                    if (it == t->agents.end()) { copy = c; break; }
                    auto ag = agents.find(it->second);
                    if (ag != agents.end() && !ag->second->running && ag->second->inbox.empty()) { copy = c; break; }
                }
                if (copy.empty()) copy = cands.front();   // all busy: queue it on the first copy
                if (!edge_allows(t->spec, from, copy, "message")) {
                    std::string tl = team_lead_of(t->spec);
                    return {false, "no edge from " + from + " to " + copy +
                                       (from == "lead" && !tl.empty() ? "; give the work to the team lead " + tl : "")};
                }
                const TeamMemberSpec * m = team_member(t->spec, team_member_of(t->spec, copy));
                role_name = m->role == "lead" ? "team-lead" : m->role;
                model = m->model;
                skills = m->skills;
                if (auto it = t->agents.find(copy); it != t->agents.end()) existing = it->second;
            }
            if (!existing.empty()) {
                Agent * c = find(existing);
                if (!c) return {false, "member agent is gone"};
                {
                    std::lock_guard lk(mu);
                    c->cancel = false;
                    c->pause_req = false;
                    c->inbox.push_back(task);
                    a->delivered.erase(c->row.id);   // its next report is new
                }
                emit(event(*c, "team", {{"action", "assigned"}, {"team", team_name}, {"copy", copy}, {"agent", c->row.id},
                                        {"from", from}, {"to", copy}}));
                start_turn(c);
                return {true, json({{"agent", c->row.id}, {"copy", copy}, {"status", "assigned (continues)"}}).dump()};
            }
            if (std::string why = spawn_denied(*a, role_name); !why.empty()) return {false, why};
            Engine * eng = nullptr;
            bool fallback = false;
            if (model == "cpu") {
                eng = remote_engine();
                fallback = eng == nullptr;
            }
            Agent * c = create_agent(ctx.session, a->row.id, role_name, task, cwd, a->depth + 1);
            {
                std::lock_guard lk(mu);
                c->team = team_name;
                c->copy = copy;
                c->model = model;
                c->eng = eng;
                c->cpu_fallback = fallback;
                auto & t = sessions[ctx.session].teams[team_name];
                c->member = team_member_of(t.spec, copy);
                t.agents[copy] = c->row.id;
            }
            if (eng) {
                EngineState st = eng->state();
                if (st.ctx_total > 0) c->ctx_cap_override = st.ctx_total / std::max(1, st.max_agents);
            }
            std::string sk;
            for (auto & n : skills)
                if (auto s = skills_lookup(ctx.session, n)) sk += "# skill: " + s->name + "\n" + truncate_output(s->body, 4000) + "\n\n";
            if (!sk.empty()) c->messages.push_back({"system", "Skills for your role:\n\n" + sk});
            sync_team(c);
            {
                std::lock_guard lk(mu);
                c->inbox.push_back(task);
            }
            emit(event(*c, "spawned", {{"task", task}, {"role", c->row.role}, {"team", team_name}, {"member", c->member},
                                       {"copy", copy}, {"model", model}}));
            emit(event(*c, "team", {{"action", "assigned"}, {"team", team_name}, {"copy", copy}, {"agent", c->row.id},
                                    {"from", from}, {"to", copy}}));
            start_turn(c);
            json out = {{"agent", c->row.id}, {"copy", copy}, {"status", "running"}};
            if (fallback) out["note"] = "the CPU swarm is off or down, so this cpu member runs on the main model";
            return {true, out.dump()};
        };
        tools.add(assign);

        Tool wait;
        wait.name = "team_wait";
        wait.description = "Wait until the team members you assigned finish, and get their reports.";
        wait.parameters = {{"type", "object"},
                           {"properties", {{"team", {{"type", "string"}}},
                                           {"timeout_s", {{"type", "integer"}, {"description", "default 1800"}}}}}};
        wait.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * self = find(ctx.agent);
            if (!self || !team_access(*self).assign) return {false, "you cannot wait on a team"};
            int timeout = std::clamp(args.value("timeout_s", 1800), 1, 7200);
            std::string tname = args.value("team", "");
            set_activity(self, "waiting");
            std::unique_lock lk(mu);
            auto mine = [&](const Agent & c) {
                return c.row.parent == self->row.id && !c.team.empty() && (tname.empty() || c.team == tname);
            };
            bool ok = cv.wait_for(lk, std::chrono::seconds(timeout), [&] {
                if (stopping || self->cancel) return true;
                for (auto & [id, c] : agents)
                    if (mine(*c) && (c->running || !c->inbox.empty())) return false;
                return true;
            });
            json out = json::array();
            for (auto & [id, c] : agents) {
                if (!mine(*c)) continue;
                bool live = c->running || !c->inbox.empty();
                if (!live && self->delivered.count(id)) continue;
                if (!live) self->delivered.insert(id);
                out.push_back({{"copy", c->copy}, {"team", c->team}, {"agent", id},
                               {"status", live ? "running" : c->row.status}, {"report", live ? "" : c->row.result}});
            }
            lk.unlock();
            if (out.empty()) return {true, "no new reports (nothing assigned, or all reports were already delivered)"};
            return {ok, ok ? out.dump(2) : "timeout; partial: " + out.dump(2)};
        };
        tools.add(wait);

        Tool status;
        status.name = "team_status";
        status.description = "Show the session's teams: each member copy, its status and the start of its latest output.";
        status.parameters = {{"type", "object"}, {"properties", {{"team", {{"type", "string"}}}}}};
        status.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a) return {false, "no agent"};
            auto ph = engine_phases();
            std::lock_guard lk(mu);
            auto s = sessions.find(ctx.session);
            if (s == sessions.end() || s->second.teams.empty()) return {true, "no teams yet"};
            std::string want = args.value("team", "");
            json out = json::array();
            for (auto & [n, t] : s->second.teams) {
                if (!want.empty() && n != want) continue;
                if (!a->team.empty() && n != a->team) continue;
                json ms = json::array();
                for (auto & c : team_all_copies(t.spec)) {
                    auto it = t.agents.find(c);
                    if (it == t.agents.end()) {
                        ms.push_back({{"copy", c}, {"status", "not started"}});
                        continue;
                    }
                    auto ag = agents.find(it->second);
                    if (ag == agents.end()) continue;
                    json live = agent_live(*ag->second, ph);
                    std::string head = ag->second->last_text.substr(0, 200);
                    ms.push_back({{"copy", c}, {"agent", it->second}, {"status", live["activity"]},
                                  {"tokens", ag->second->row.prompt_tokens + ag->second->row.completion_tokens},
                                  {"latest", head}});
                }
                out.push_back({{"team", n}, {"disbanded", t.disbanded}, {"members", ms}});
            }
            return {true, out.dump(2)};
        };
        tools.add(status);

        Tool msg;
        msg.name = "team_message";
        msg.description = "Send a short note to a teammate along an edge (to: a copy id, or lead). It reaches them at "
                          "their next step, or starts them if they are idle.";
        msg.parameters = {{"type", "object"},
                          {"properties", {{"team", {{"type", "string"}}}, {"to", {{"type", "string"}}},
                                          {"message", {{"type", "string"}}}}},
                          {"required", {"to", "message"}}};
        msg.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a) return {false, "no agent"};
            std::string to = args.value("to", ""), text = args.value("message", "");
            if (text.empty()) return {false, "message is empty"};
            if (text.size() > 4000) text = text.substr(0, 4000) + " [cut]";
            Agent * target = nullptr;
            bool start = false;
            std::string from, team_name;
            {
                std::lock_guard lk(mu);
                std::string err;
                TeamRt * t = find_team(a->row.session, *a, args.value("team", ""), err);
                if (!t) return {false, err};
                from = team_identity(*a, *t);
                if (from.empty()) return {false, "you are not in team " + t->spec.name};
                if (!edge_allows(t->spec, from, to, "message")) return {false, "no message edge from " + from + " to " + to};
                team_name = t->spec.name;
                std::string tid = to == "lead" ? t->owner : (t->agents.count(to) ? t->agents[to] : "");
                if (tid.empty()) return {false, to + " has not started; give it work with team_assign"};
                auto it = agents.find(tid);
                if (it == agents.end()) return {false, to + " is gone"};
                target = it->second.get();
                std::string note = "[team " + team_name + "] message from " + from + ": " + text;
                if (target->running || to == "lead") {
                    target->mail.push_back(note);   // a lead's idle turn is not restarted by its members
                } else {
                    target->cancel = false;
                    target->inbox.push_back(note);
                    start = true;
                }
            }
            emit(event(*a, "team", {{"action", "message"}, {"team", team_name}, {"from", from}, {"to", to}}));
            if (start) start_turn(target);
            return {true, "delivered to " + to};
        };
        tools.add(msg);

        Tool read;
        read.name = "team_read";
        read.description = "Read the latest output of a teammate you may observe (its tail, bounded).";
        read.parameters = {{"type", "object"},
                           {"properties", {{"team", {{"type", "string"}}}, {"agent", {{"type", "string"}, {"description", "copy id"}}},
                                           {"max_chars", {{"type", "integer"}, {"description", "default 4000"}}}}},
                           {"required", {"agent"}}};
        read.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a) return {false, "no agent"};
            std::string who = args.value("agent", "");
            size_t cap = (size_t) std::clamp(args.value("max_chars", 4000), 200, std::max(200, cfg.teams.read_max_chars));
            std::lock_guard lk(mu);
            std::string err;
            TeamRt * t = find_team(a->row.session, *a, args.value("team", ""), err);
            if (!t) return {false, err};
            std::string from = team_identity(*a, *t);
            if (from.empty()) return {false, "you are not in team " + t->spec.name};
            if (!edge_allows(t->spec, from, who, "observe")) return {false, "no observe edge from " + from + " to " + who};
            auto it = t->agents.find(who);
            if (it == t->agents.end()) return {true, who + " has not started"};
            auto ag = agents.find(it->second);
            if (ag == agents.end()) return {false, who + " is gone"};
            const Agent & c = *ag->second;
            std::string body = !c.running && !c.row.result.empty() ? c.row.result : c.last_text;
            bool cut = body.size() > cap;
            if (cut) {
                body = body.substr(body.size() - cap);
                while (!body.empty() && ((unsigned char) body[0] & 0xC0) == 0x80) body.erase(0, 1);
            }
            return {true, "[" + who + ", " + team_status_merge(c.activity, "") + (cut ? ", tail" : "") + "]\n" + body};
        };
        tools.add(read);

        Tool disband;
        disband.name = "team_disband";
        disband.description = "Stop a team: cancels its members and closes it.";
        disband.parameters = {{"type", "object"}, {"properties", {{"team", {{"type", "string"}}}}}, {"required", {"team"}}};
        disband.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a || !team_access(*a).create) return {false, "only the session lead can disband a team"};
            std::string name = args.value("team", "");
            {
                std::lock_guard lk(mu);
                auto s = sessions.find(ctx.session);
                if (s == sessions.end() || !s->second.teams.count(name)) return {false, "no team named '" + name + "'"};
                TeamRt & t = s->second.teams[name];
                if (t.disbanded) return {true, "already disbanded"};
                t.disbanded = true;
                s->second.team_copies = std::max(0, s->second.team_copies - (int) team_all_copies(t.spec).size());
                for (auto & [copy, id] : t.agents)
                    if (auto it = agents.find(id); it != agents.end()) {
                        it->second->cancel = true;
                        it->second->inbox.clear();
                        // members of a team lead belong to the team too (same team name)
                    }
            }
            cv.notify_all();
            emit(event(*a, "team", {{"action", "disbanded"}, {"team", name}}));
            return {true, "team " + name + " disbanded"};
        };
        tools.add(disband);

        Tool save;
        save.name = "team_save";
        save.description = "Save a team's map so a later job can start from it (team_create {spec: name}). Offer this after a "
                           "team finishes well. Writes ~/.config/judas/teams/NAME.map with a header (date, session, outcome, "
                           "stats). Never overwrites unless overwrite is true; the old file is kept as NAME.map.bak.";
        save.parameters = {{"type", "object"},
                           {"properties",
                            {{"name", {{"type", "string"}, {"description", "short name, a-z 0-9 and - only"}}},
                             {"team", {{"type", "string"}, {"description", "team to save (default: the only live team)"}}},
                             {"note", {{"type", "string"}, {"description", "one line: what the team did and how it went"}}},
                             {"overwrite", {{"type", "boolean"}, {"description", "replace an existing saved map (default false)"}}}}},
                           {"required", {"name"}}};
        save.run = [this](const json & args, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a || !team_access(*a).create) return {false, "only the session lead of a Swarm session can save a team"};
            std::string path, used;
            std::string err = save_team(ctx.session, args.value("team", std::string()), args.value("name", std::string()),
                                        args.value("note", std::string()), args.value("overwrite", false), path, used);
            if (!err.empty()) return {false, err};
            return {true, "saved team " + used + " as " + path + ". Start from it later with team_create {spec: \"" +
                              args.value("name", std::string()) + "\"}."};
        };
        tools.add(save);

        Tool listed;
        listed.name = "team_list_saved";
        listed.description = "List saved team maps (name, first comment line, date, last outcome). Check this when a new job "
                             "resembles an earlier one, then start from it with team_create {spec: name}.";
        listed.parameters = {{"type", "object"}, {"properties", json::object()}};
        listed.run = [this](const json &, ToolContext & ctx) -> ToolResult {
            Agent * a = find(ctx.agent);
            if (!a || !team_access(*a).create) return {false, "only the session lead of a Swarm session can list saved teams"};
            auto l = team_saved_list(team_dir_resolve(cfg.teams.dir));
            if (l.empty()) return {true, "no saved teams yet"};
            std::string out;
            for (auto & m : l)
                out += m.name + " | " + (m.title.empty() ? "(no description)" : m.title) + " | " +
                       (m.date.empty() ? "no date" : m.date) + " | " + (m.outcome.empty() ? "no outcome recorded" : m.outcome) + "\n";
            return {true, out};
        };
        tools.add(listed);
    }

    std::optional<Skill> skills_lookup(const std::string & sid, const std::string & name) {
        auto ps = session_skills(sid);
        return ps ? ps->by_name(name) : skills.by_name(name);
    }

    // Rebuild a lead agent from the store after a restart.
    Agent * restore_lead(const std::string & session_id, const std::string & cwd) {
        for (const auto & row : store.list_agents(session_id)) {
            if (!row.parent.empty()) continue;
            auto a = std::make_unique<Agent>();
            a->row = row;
            a->role = roles.count(row.role) ? roles[row.role] : roles["lead"];
            a->cwd = cwd;
            for (const auto & m : store.messages(session_id, row.id)) {
                auto v = messages_from_oai(json::array({m.body}));
                if (!v.empty()) a->messages.push_back(v[0]);
            }
            if (!a->messages.empty() && a->messages[0].role == "system") a->messages[0].content = system_prompt(*a);
            // the plan lives in memory only: rebuild it from the last update_plan call so a resume has it
            for (auto it = a->messages.rbegin(); it != a->messages.rend() && a->plan.empty(); ++it)
                for (auto & tc : it->tool_calls)
                    if (tc.name == "update_plan")
                        try {
                            a->plan = json::parse(tc.arguments).value("plan", "");
                        } catch (...) {
                        }
            Agent * raw = a.get();
            std::lock_guard lk(mu);
            agents[row.id] = std::move(a);
            return raw;
        }
        return nullptr;
    }
};

AgentManager::AgentManager(const Config & cfg, Engine & engine, Store & store, Journal & journal)
    : impl_(std::make_unique<Impl>(cfg, engine, store, journal)) {}

AgentManager::~AgentManager() = default;

void AgentManager::set_swarm_tool(bool on) {
    if (on) {
        if (!impl_->tools.find("swarm_map")) register_swarm_tool(impl_->tools);
    } else {
        impl_->tools.remove("swarm_map");
    }
}

void AgentManager::set_session_extras(const std::string & session_id, const SessionExtras & x) {
    auto & I = *impl_;
    Impl::Extras e;
    e.overlay = x.overlay;
    if (!x.skills_dir.empty()) {
        SkillsConfig sc = I.cfg.skills;
        sc.paths.push_back(x.skills_dir);   // last wins
        e.skills = std::make_shared<Skills>(sc);
    }
    std::lock_guard lk(I.extras_mu);
    I.extras[session_id] = std::move(e);
}

void AgentManager::clear_session_extras(const std::string & session_id) {
    auto & I = *impl_;
    std::lock_guard lk(I.extras_mu);
    I.extras.erase(session_id);
}

std::string AgentManager::submit(const std::string & session_id, const std::string & user_message,
                                 const std::string & cwd) {
    auto & I = *impl_;
    std::string sid = session_id;
    std::string dir = cwd.empty() ? fs::current_path().string() : cwd;
    if (sid.empty() || !I.store.get_session(sid)) {
        std::string title = user_message.substr(0, 60);
        sid = I.store.create_session(title, dir, I.engine.model_name()).id;
    }
    Impl::Agent * lead = nullptr;
    {
        std::lock_guard lk(I.mu);
        auto s = I.sessions.find(sid);
        if (s != I.sessions.end()) {
            auto it = I.agents.find(s->second.lead);
            if (it != I.agents.end()) lead = it->second.get();
        }
    }
    if (!lead) lead = I.restore_lead(sid, dir);
    if (!lead) lead = I.create_agent(sid, "", "lead", user_message, dir, 0);
    {
        std::lock_guard lk(I.mu);
        auto & s = I.sessions[sid];
        s.lead = lead->row.id;
        s.busy = true;
        s.request = user_message;
        // a new message clears an earlier cancel for the whole session, workers included
        for (auto & [id, ag] : I.agents)
            if (ag->row.session == sid) ag->cancel = false;
        // ... but a turn the user just sent always cancels an in-flight background review
        for (auto & [id, ag] : I.agents)
            if (ag->row.session == sid && ag->review) {
                ag->cancel = true;
                ag->inbox.clear();
            }
        lead->inbox.push_back(user_message);
    }
    I.emit(I.event(*lead, "spawned", {{"task", user_message}, {"role", "lead"}}));
    I.start_turn(lead);
    return sid;
}

std::string AgentManager::wait_session(const std::string & session_id) {
    auto & I = *impl_;
    std::unique_lock lk(I.mu);
    I.cv.wait(lk, [&] {
        auto s = I.sessions.find(session_id);
        return I.stopping || s == I.sessions.end() || !s->second.busy;
    });
    auto s = I.sessions.find(session_id);
    return s == I.sessions.end() ? "" : s->second.last_answer;
}

void AgentManager::cancel(const std::string & session_id) {
    auto & I = *impl_;
    Impl::Agent * lead = nullptr;
    bool had = false;
    {
        std::lock_guard lk(I.mu);
        for (auto & [id, a] : I.agents)
            if (a->row.session == session_id) {
                a->cancel = true;
                a->inbox.clear();
            }
        auto s = I.sessions.find(session_id);
        if (s != I.sessions.end()) {
            had = !s->second.queued.empty();
            s->second.queued.clear();
            auto it = I.agents.find(s->second.lead);
            if (it != I.agents.end()) lead = it->second.get();
        }
        I.cv.notify_all();
    }
    if (had && lead) I.emit(I.event(*lead, "queue", {{"queued", json::array()}}));
}

AgentManager::SubmitResult AgentManager::submit_queued(const std::string & session_id, const std::string & text,
                                                       const std::string & suffix, const std::string & cwd) {
    auto & I = *impl_;
    SubmitResult r;
    Impl::Agent * lead = nullptr;
    if (!session_id.empty()) {
        std::lock_guard lk(I.mu);
        auto s = I.sessions.find(session_id);
        if (s != I.sessions.end()) {
            auto it = I.agents.find(s->second.lead);
            if (it != I.agents.end()) lead = it->second.get();
        }
    }
    if (lead) {
        json q;
        {
            std::lock_guard lk(I.mu);
            // a lead that is stopping (cancelled) takes the message as a fresh turn, as submit always did
            if (lead->running && !lead->cancel && !I.stopping) {
                auto & s = I.sessions[session_id];
                if (s.queued.size() >= kMaxQueued) {
                    r.session = session_id;
                    r.error = "the queue is full (" + std::to_string(kMaxQueued) + " messages waiting); wait for the turn or remove one";
                    return r;
                }
                r.id = "q" + std::to_string(s.next_q++);
                s.queued.push_back({r.id, text, suffix});
                r.queued = true;
                r.session = session_id;
                q = I.queue_json_locked(session_id);
            }
        }
        if (r.queued) {
            I.emit(I.event(*lead, "queue", {{"queued", q}}));
            return r;
        }
    }
    r.session = submit(session_id, text + suffix, cwd);
    return r;
}

std::vector<AgentManager::QueuedMsg> AgentManager::queued(const std::string & session_id) const {
    auto & I = *impl_;
    std::vector<QueuedMsg> out;
    std::lock_guard lk(I.mu);
    auto s = I.sessions.find(session_id);
    if (s != I.sessions.end())
        for (auto & q : s->second.queued) out.push_back({q.id, q.text});
    return out;
}

bool AgentManager::queue_remove(const std::string & session_id, const std::string & qid) {
    auto & I = *impl_;
    Impl::Agent * lead = nullptr;
    json q;
    {
        std::lock_guard lk(I.mu);
        auto s = I.sessions.find(session_id);
        if (s == I.sessions.end()) return false;
        auto & dq = s->second.queued;
        auto it = std::find_if(dq.begin(), dq.end(), [&](const Impl::Session::Queued & x) { return x.id == qid; });
        if (it == dq.end()) return false;
        dq.erase(it);
        auto l = I.agents.find(s->second.lead);
        if (l != I.agents.end()) lead = l->second.get();
        q = I.queue_json_locked(session_id);
    }
    if (lead) I.emit(I.event(*lead, "queue", {{"queued", q}}));
    return true;
}

bool AgentManager::queue_edit(const std::string & session_id, const std::string & qid, const std::string & text) {
    auto & I = *impl_;
    Impl::Agent * lead = nullptr;
    json q;
    {
        std::lock_guard lk(I.mu);
        auto s = I.sessions.find(session_id);
        if (s == I.sessions.end()) return false;
        bool hit = false;
        for (auto & x : s->second.queued)
            if (x.id == qid) { x.text = text; hit = true; }
        if (!hit) return false;
        auto l = I.agents.find(s->second.lead);
        if (l != I.agents.end()) lead = l->second.get();
        q = I.queue_json_locked(session_id);
    }
    if (lead) I.emit(I.event(*lead, "queue", {{"queued", q}}));
    return true;
}

AgentManager::TurnInfo AgentManager::turn_info(const std::string & session_id) const {
    auto & I = *impl_;
    TurnInfo t;
    std::lock_guard lk(I.mu);
    auto s = I.sessions.find(session_id);
    const Impl::Agent * lead = nullptr;
    if (s != I.sessions.end()) {
        auto it = I.agents.find(s->second.lead);
        if (it != I.agents.end()) lead = it->second.get();
    }
    if (!lead) {
        for (auto & row : I.store.list_agents(session_id))
            if (row.parent.empty()) {
                t.status = row.status == "failed" ? "error" : row.status;
                return t;
            }
        return t;
    }
    t.status = lead->running ? "running" : lead->row.status == "failed" ? "error" : lead->row.status;
    t.reason = lead->reason;
    t.plan = lead->plan;
    t.open_items = plan_state(lead->plan).open;
    if (s != I.sessions.end())
        for (auto & q : s->second.queued) t.queued.push_back({q.id, q.text});
    return t;
}

bool AgentManager::resume(const std::string & session_id, const std::string & cwd) {
    auto & I = *impl_;
    if (!I.store.get_session(session_id)) return false;
    Impl::Agent * lead = nullptr;
    {
        std::lock_guard lk(I.mu);
        auto s = I.sessions.find(session_id);
        if (s != I.sessions.end()) {
            auto it = I.agents.find(s->second.lead);
            if (it != I.agents.end()) lead = it->second.get();
        }
    }
    if (!lead) lead = I.restore_lead(session_id, cwd.empty() ? fs::current_path().string() : cwd);
    if (!lead) return false;
    std::string msg;
    {
        std::lock_guard lk(I.mu);
        if (lead->running || lead->row.status != "paused") return false;
        msg = "Resume the task where you paused. Do not start over.";
        if (!lead->reason.empty()) msg += " You paused because: " + lead->reason + ".";
        if (!lead->plan.empty()) msg += "\nYour plan so far:\n" + lead->plan;
        msg += "\nCarry on with the next open item, and finish with done.";
        auto & s = I.sessions[session_id];
        s.lead = lead->row.id;
        s.busy = true;
        for (auto & [id, ag] : I.agents)
            if (ag->row.session == session_id) ag->cancel = false;
        for (auto & [id, ag] : I.agents)
            if (ag->row.session == session_id && ag->review) {
                ag->cancel = true;
                ag->inbox.clear();
            }
        lead->inbox.push_back(msg);
    }
    I.emit(I.event(*lead, "spawned", {{"task", msg}, {"role", "lead"}, {"resume", true}}));
    I.start_turn(lead);
    return true;
}

std::string AgentManager::team_save(const std::string & session, const std::string & team, const std::string & name,
                                    const std::string & note, bool overwrite, std::string & path) {
    std::string used;
    return impl_->save_team(session, team, name, note, overwrite, path, used);
}

json AgentManager::team_saved() const {
    json out = json::array();
    for (auto & m : team_saved_list(team_dir_resolve(impl_->cfg.teams.dir)))
        out.push_back({{"name", m.name}, {"title", m.title}, {"date", m.date}, {"outcome", m.outcome}});
    return out;
}

json AgentManager::team_view(const std::string & session) const {
    auto & I = *impl_;
    auto ph = I.engine_phases();
    std::lock_guard lk(I.mu);
    json teams = json::array(), ags = json::array(), lead = nullptr;
    auto s = I.sessions.find(session);
    if (s != I.sessions.end()) {
        for (auto & [n, t] : s->second.teams) {
            json members = json::array(), edges = json::array(), copies = json::array();
            for (auto & m : t.spec.members)
                members.push_back({{"id", m.id}, {"role", m.role}, {"count", m.count}, {"model", m.model}, {"skills", m.skills}});
            for (auto & e : t.spec.edges) edges.push_back({{"from", e.from}, {"to", e.to}, {"kind", e.kind}});
            for (auto & c : team_all_copies(t.spec)) {
                auto it = t.agents.find(c);
                copies.push_back({{"copy", c}, {"member", team_member_of(t.spec, c)}, {"agent", it == t.agents.end() ? "" : it->second}});
            }
            teams.push_back({{"name", n}, {"brief", t.spec.brief}, {"rules", t.spec.rules}, {"lead_member", team_lead_of(t.spec)},
                             {"owner", t.owner}, {"disbanded", t.disbanded}, {"members", members}, {"edges", edges},
                             {"copies", copies}, {"place", t.spec.place}, {"map", team_map_text(t.spec)},
                             {"warnings", t.warnings}});
        }
    }
    for (auto & [id, a] : I.agents) {
        if (a->row.session != session || a->review) continue;
        json j = I.agent_live(*a, ph);
        if (s != I.sessions.end() && id == s->second.lead) lead = j;
        ags.push_back(j);
    }
    return {{"session", session}, {"lead", lead}, {"teams", teams}, {"agents", ags}};
}

bool AgentManager::cancel_agent(const std::string & session, const std::string & agent) {
    auto & I = *impl_;
    std::lock_guard lk(I.mu);
    auto it = I.agents.find(agent);
    if (it == I.agents.end() || it->second->row.session != session) return false;
    it->second->pause_req = false;
    it->second->cancel = true;
    it->second->inbox.clear();
    I.cv.notify_all();
    return true;
}

bool AgentManager::pause_agent(const std::string & session, const std::string & agent) {
    auto & I = *impl_;
    std::lock_guard lk(I.mu);
    auto it = I.agents.find(agent);
    if (it == I.agents.end() || it->second->row.session != session) return false;
    it->second->pause_req = true;
    it->second->cancel = true;   // the inbox stays: resume continues with it
    I.cv.notify_all();
    return true;
}

bool AgentManager::resume_agent(const std::string & session, const std::string & agent) {
    auto & I = *impl_;
    Impl::Agent * a = nullptr;
    {
        std::lock_guard lk(I.mu);
        auto it = I.agents.find(agent);
        if (it == I.agents.end() || it->second->row.session != session || it->second->running) return false;
        a = it->second.get();
        if (a->row.status != "paused" && a->row.status != "killed") return false;
        a->cancel = false;
        a->pause_req = false;
        a->inbox.push_back("Continue your task where you stopped. Do not start over; finish with done.");
    }
    I.start_turn(a);
    return true;
}

void AgentManager::set_remote_engine_factory(std::function<std::unique_ptr<Engine>(const std::string &)> f) {
    std::lock_guard lk(impl_->remote_mu);
    impl_->remote_factory = std::move(f);
    impl_->remote.reset();
}

int AgentManager::add_listener(AgentListener fn) {
    std::lock_guard lk(impl_->mu);
    int h = impl_->next_listener++;
    impl_->listeners[h] = std::move(fn);
    return h;
}

void AgentManager::remove_listener(int handle) {
    std::lock_guard lk(impl_->mu);
    impl_->listeners.erase(handle);
}

void AgentManager::set_approver(std::function<bool(const std::string &, const std::string &)> fn) {
    std::lock_guard lk(impl_->mu);
    impl_->approver = std::move(fn);
}

void AgentManager::enable_remote_approvals(bool on) {
    std::lock_guard lk(impl_->mu);
    impl_->remote_approvals = on;
}

void AgentManager::set_approval_timeout(int seconds) {
    std::lock_guard lk(impl_->mu);
    impl_->approval_timeout_s = seconds;
}

std::vector<AgentManager::Approval> AgentManager::approvals(const std::string & session) const {
    std::vector<Approval> out;
    std::lock_guard lk(impl_->mu);
    for (auto & [id, p] : impl_->approval_map)
        if (p->state == 0 && (session.empty() || p->info.session == session)) out.push_back(p->info);
    std::sort(out.begin(), out.end(), [](const Approval & x, const Approval & y) {
        return x.created_ms != y.created_ms ? x.created_ms < y.created_ms : x.id.size() != y.id.size() ? x.id.size() < y.id.size() : x.id < y.id;
    });
    return out;
}

AgentManager::ApprovalResult AgentManager::answer_approval(const std::string & session, const std::string & id, bool approve) {
    std::shared_ptr<Impl::PendingApproval> pa;
    Impl::Agent * a = nullptr;
    {
        std::lock_guard lk(impl_->mu);
        auto it = impl_->approval_map.find(id);
        if (it == impl_->approval_map.end() || it->second->info.session != session) return ApprovalResult::Unknown;
        pa = it->second;
        if (pa->state != 0) return ApprovalResult::AlreadyResolved;
        pa->state = approve ? 1 : 2;
        pa->by = "user";
        auto ag = impl_->agents.find(pa->info.agent);
        if (ag != impl_->agents.end()) a = ag->second.get();
    }
    impl_->cv.notify_all();
    if (a) impl_->emit(impl_->event(*a, "approval_resolved", {{"id", id}, {"approved", approve}, {"by", "user"}}));
    return ApprovalResult::Ok;
}

std::vector<AgentRow> AgentManager::agents(const std::string & session) const {
    std::vector<AgentRow> out;
    std::lock_guard lk(impl_->mu);
    for (auto & [id, a] : impl_->agents)
        if (a->row.session == session) {
            AgentRow r = a->row;
            if (a->running) r.status = "running";
            out.push_back(r);
        }
    return out;
}

}  // namespace judas
