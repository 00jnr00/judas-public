// Agent manager: each agent runs its turns on its own thread and calls Engine::chat, so the engine batches
// every live agent together. Workers report back with their final answer; the lead waits, reviews, decides.

#include "agent/agent.h"

#include <algorithm>
#include "agent/gitstep.h"
#include "agent/policy.h"
#include "channels/channels.h"
#include "context/compactor.h"
#include "hooks/hooks.h"
#include "learning/review.h"
#include "mcp/filter.h"
#include "mcp/manager.h"
#include "skills/skill_tools.h"
#include "skills/skills.h"
#include "skills/usage.h"
#include "tools/memory_tools.h"
#include "tools/session_tools.h"

#include <unistd.h>

#include <set>

#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace judas {

const char * version();

namespace fs = std::filesystem;

namespace {

const char * BASE_PROMPT = R"(You are Judas, a local agent harness running on the user's own machine with one shared model.
Working directory: {cwd}
Date: {date}

How you work:
- Use tools to look before you act. Read files before editing them. Prefer edit_file over rewriting whole files.
- Every file change is recorded in a snapshot journal. snapshot_list, snapshot_diff and snapshot_revert let you
  inspect and undo your own steps; use them when a change breaks something.
- Shell commands may have side effects a snapshot cannot undo (services, packages, network). Say so when you run one.
- You do not have root and must not seek it. sudo, su and credential commands are blocked. Never guess or brute
  force a password. If a task seems to need root, stop and tell the user; do not try to get around the block.
- Be direct and concise.
- Keep going until the task is finished: every reply is a tool call until then. Do not stop to announce what you
  will do next ("Let me look at X:"); make the call instead.
- Finish by calling done with a summary (it is your final answer). A plain-text reply with no tool call also ends
  your turn, but only when your plan has no open items.
- If the same command or error keeps coming back, stop repeating it: change your approach or ask for help.
- Ground every factual claim. Never explain a name, product, feature or file you have not verified: look it up
  first (memory_search, grep, read_file, web_search), and if you still cannot find it, say you do not know it and
  ask what the user means. A likely typo ("agenticx" for agentic, "supar" for super) is fine to read charitably,
  but say which reading you used. Never invent components, features or numbers.
- Questions about Judas itself: your source of truth is ~/src/judas (README.md, docs/DESIGN.md, docs/API.md,
  docs/MCP.md). Read the relevant doc before answering; do not describe Judas from general knowledge.
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
        std::thread thread;
    };
    // How one turn ended. status is the agent row status it maps to.
    struct TurnOutcome {
        std::string answer;
        std::string status = "done";   // done | paused | failed
        std::string reason;
    };
    struct Session {
        std::string lead;
        bool busy = false;
        std::string last_answer;
        ReviewCounters review;       // per-session nudge counters and the in-flight flag
        std::string review_id;       // the review agent currently running, if any
    };

    const Config & cfg;
    Engine & engine;
    Store & store;
    Journal & journal;
    std::map<std::string, Role> roles;
    ToolRegistry tools;
    Skills skills;
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

    Impl(const Config & c, Engine & e, Store & s, Journal & j)
        : cfg(c), engine(e), store(s), journal(j), skills(c.skills), hooks(c.hooks) {
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
        register_memory_tools(tools);
        register_session_tools(tools, store);
        register_agent_tools();
        register_skill_tool();
        if (cfg.skills.enabled && cfg.skills.manage) register_skill_tools(tools, skills, usage.get());
        register_notify_tool();
        register_recall_tool();
        register_plan_tool();
        register_context_tools();
        register_done_tool();
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

    std::string system_prompt(const Agent & a) {
        std::string p = replace_all(replace_all(BASE_PROMPT, "{cwd}", a.cwd), "{date}", today());
        p += "\n\n" + a.role.prompt;
        std::string sk = skills.index_block();
        if (!sk.empty()) p += "\n\n" + sk;
        std::string mem = memory_prompt_block(cfg);
        if (!mem.empty()) p += "\n\n" + mem;
        return p;
    }

    // load_skill: the model reads a skill's full body on demand (only descriptions are in the prompt).
    void register_skill_tool() {
        if (!cfg.skills.enabled) return;
        Tool t;
        t.name = "load_skill";
        t.description = "Read the full instructions of a named skill (see the skills list in your prompt).";
        t.parameters = {{"type", "object"}, {"properties", {{"name", {{"type", "string"}}}}}, {"required", {"name"}}};
        t.run = [this](const json & args, ToolContext &) -> ToolResult {
            auto s = skills.by_name(args.value("name", ""));
            if (!s) return {false, "no such skill: " + args.value("name", "")};
            if (usage) usage->record_load(s->name, false);
            return {true, "# skill: " + s->name + "\n" + s->body};
        };
        tools.add(t);
    }

    // Autoload: skill bodies whose triggers match the user's message, prepended as a system note for this turn.
    std::string autoload_skills(const std::string & user_text) {
        if (cfg.skills.autoload != "trigger") return "";
        std::string out;
        for (const auto & s : skills.matching(user_text)) {
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
        for (const Tool * t : tools.list()) {
            bool agent_tool = t->name.rfind("spawn_agent", 0) == 0 || t->name == "wait_agents" ||
                              t->name == "send_agent" || t->name == "list_agents";
            if (agent_tool && !can_spawn) continue;
            // the context-editing tools exist only in clm mode (compact prunes automatically instead)
            if ((t->name == "drop_context" || t->name == "note_context") && cfg.context.mode != "clm") continue;
            if (!a.role.tools.empty() && !agent_tool && t->name != "done" && !mcp::role_allows(a.role.tools, *t))
                continue;
            if (!t->source.empty()) {   // MCP tool: at most max_tools_per_agent per agent (small models degrade with many)
                if (cfg.mcp.max_tools_per_agent > 0 && mcp_seen >= cfg.mcp.max_tools_per_agent) continue;
                mcp_seen++;
            }
            out.push_back(t);
        }
        return out;
    }

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

    void run(Agent * a) {
        while (true) {
            std::string input;
            {
                std::lock_guard lk(mu);
                if (a->inbox.empty() || stopping) {
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
            if (!a->review) {
                skills.reload_if_changed();   // pick up curator/human edits to the skills dir
                if (std::string sk = autoload_skills(input); !sk.empty())
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
            {
                std::lock_guard lk(mu);
                a->row.result = answer;
                // done | paused (a cap was hit with work left; resumable) | failed (engine error) | killed
                a->row.status = a->cancel ? "killed" : out.status;
                a->reason = a->cancel ? "cancelled" : out.reason;
                a->row.updated_ms = now_ms();
                store.update_agent(a->row);
                auto s = sessions.find(a->row.session);
                if (s != sessions.end() && s->second.lead == a->row.id && a->inbox.empty()) {
                    s->second.last_answer = answer;
                    s->second.busy = false;
                }
            }
            emit(event(*a, "done", {{"result", answer}, {"status", a->row.status}, {"reason", a->reason}}));
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
            cap = cfg.active_model().ctx_per_agent;
        } catch (...) {
            return;  // model not resolvable (e.g. a test with no models); nothing to size against
        }
        if (cap <= 0) return;
        auto count = [this](const std::string & s) { return engine.count_tokens(s); };
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
            return engine.chat(r).message.content;
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
            bool last = step >= max_steps;
            if (last && plan_state(a->plan).open > 0)
                return paused(a, "step limit (" + std::to_string(max_steps) + ") reached with plan items still open");
            if (!last) note_tool_step(a);   // a model step with tools enabled counts toward the skill nudge
            ChatRequest req;
            req.messages = a->messages;
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
            auto res = engine.chat(
                req,
                [&](const ChatDelta & d) {
                    if (!d.reasoning.empty()) emit(event(*a, "reasoning", {{"text", d.reasoning}}));
                    if (!d.content.empty()) emit(event(*a, "delta", {{"text", d.content}}));
                    return !a->cancel.load();
                },
                &a->cancel);
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
            if (!a->review) store.add_message(a->row.session, a->row.id, "assistant", message_to_oai(msg));
            emit(event(*a, "message", {{"message", message_to_oai(msg)},
                                        {"usage", {{"prompt", res.prompt_tokens}, {"cached", res.cached_tokens},
                                                   {"completion", res.completion_tokens},
                                                   {"prefill_s", res.prefill_s}, {"generate_s", res.generate_s}}}}));
            if (res.finish_reason == "cancelled") return {msg.content, "done", ""};
            if (last) {
                finish_hooks(a, msg.content);
                return {msg.content, "done", ""};
            }
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
                std::string label = tc.name + " " + (args.contains("path") ? args["path"].dump()
                                                     : args.contains("command") ? args["command"].dump()
                                                                                : std::string());
                ToolContext ctx;
                ctx.cwd = a->cwd;
                ctx.cfg = &cfg;
                ctx.journal = &journal;
                ctx.session = a->row.session;
                ctx.agent = a->row.id;
                ctx.autonomous = a->review;
                ctx.checkpoint = journal.begin(a->row.session, a->row.id, label.substr(0, 200));
                if (!a->review) ctx.approve = [this, a](const std::string & what) {
                    std::function<bool(const std::string &, const std::string &)> f;
                    {
                        std::lock_guard lk(mu);
                        f = approver;
                    }
                    if (cfg.permissions.mode == "full-auto") return true;
                    return f ? f(a->row.session, what) : false;
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
                    int cap = cfg.active_model().ctx_per_agent;
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
    std::lock_guard lk(I.mu);
    for (auto & [id, a] : I.agents)
        if (a->row.session == session_id) {
            a->cancel = true;
            a->inbox.clear();
        }
    I.cv.notify_all();
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
