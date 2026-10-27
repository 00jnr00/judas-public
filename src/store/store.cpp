// SQLite store for sessions, agents, messages and full-text search. One connection, guarded by mu_.
#include "store/store.h"

#include <sqlite3.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <random>
#include <stdexcept>

namespace judas {

long long now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string new_id(const char * prefix) {
    static std::mutex m;
    static std::mt19937 rng{std::random_device{}()};
    unsigned r;
    {
        std::lock_guard<std::mutex> lk(m);
        r = (unsigned) (rng() & 0xffff);
    }
    std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[64];
    std::snprintf(buf, sizeof buf, "-%04d%02d%02d-%02d%02d%02d-%04x", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                  tm.tm_hour, tm.tm_min, tm.tm_sec, r);
    return std::string(prefix) + buf;
}

namespace {

[[noreturn]] void db_error(sqlite3 * db, const char * what) {
    throw std::runtime_error(std::string("sqlite: ") + what + ": " + sqlite3_errmsg(db));
}

// Prepared statement with typed binders; finalized on scope exit.
class Stmt {
public:
    Stmt(sqlite3 * db, const char * sql) : db_(db) {
        if (sqlite3_prepare_v2(db, sql, -1, &s_, nullptr) != SQLITE_OK) db_error(db, "prepare");
    }
    ~Stmt() { sqlite3_finalize(s_); }
    Stmt(const Stmt &) = delete;
    Stmt & operator=(const Stmt &) = delete;

    Stmt & text(int i, const std::string & v) {
        if (sqlite3_bind_text(s_, i, v.data(), (int) v.size(), SQLITE_TRANSIENT) != SQLITE_OK) db_error(db_, "bind");
        return *this;
    }
    Stmt & i64(int i, long long v) {
        if (sqlite3_bind_int64(s_, i, v) != SQLITE_OK) db_error(db_, "bind");
        return *this;
    }
    // true while a row is available
    bool step() {
        int rc = sqlite3_step(s_);
        if (rc == SQLITE_ROW) return true;
        if (rc == SQLITE_DONE) return false;
        db_error(db_, "step");
    }
    void run() { step(); }
    std::string str(int c) const {
        const unsigned char * p = sqlite3_column_text(s_, c);
        return p ? std::string((const char *) p, (size_t) sqlite3_column_bytes(s_, c)) : std::string();
    }
    long long num(int c) const { return sqlite3_column_int64(s_, c); }

private:
    sqlite3 * db_;
    sqlite3_stmt * s_ = nullptr;
};

void exec(sqlite3 * db, const char * sql) {
    char * err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("sqlite: " + m);
    }
}

const char * kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS sessions (
    id TEXT PRIMARY KEY,
    title TEXT NOT NULL,
    cwd TEXT NOT NULL,
    model TEXT NOT NULL,
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL,
    mode TEXT NOT NULL DEFAULT 'chat'
);
CREATE INDEX IF NOT EXISTS sessions_updated ON sessions(updated_ms DESC);
CREATE TABLE IF NOT EXISTS agents (
    id TEXT PRIMARY KEY,
    session TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    parent TEXT NOT NULL DEFAULT '',
    role TEXT NOT NULL,
    task TEXT NOT NULL,
    status TEXT NOT NULL,
    result TEXT NOT NULL DEFAULT '',
    created_ms INTEGER NOT NULL,
    updated_ms INTEGER NOT NULL,
    prompt_tokens INTEGER NOT NULL DEFAULT 0,
    completion_tokens INTEGER NOT NULL DEFAULT 0
);
CREATE INDEX IF NOT EXISTS agents_session ON agents(session, created_ms);
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    session TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE,
    agent TEXT NOT NULL,
    role TEXT NOT NULL,
    body TEXT NOT NULL,
    content TEXT NOT NULL DEFAULT '',
    created_ms INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS messages_agent ON messages(session, agent, id);
CREATE TABLE IF NOT EXISTS tasks (
    id TEXT PRIMARY KEY,
    title TEXT NOT NULL,
    prompt TEXT NOT NULL,
    cwd TEXT NOT NULL DEFAULT '',
    kind TEXT NOT NULL,
    run_at_ms INTEGER NOT NULL DEFAULT 0,
    interval_s INTEGER NOT NULL DEFAULT 0,
    next_ms INTEGER NOT NULL DEFAULT 0,
    enabled INTEGER NOT NULL DEFAULT 1,
    last_session TEXT NOT NULL DEFAULT '',
    last_run_ms INTEGER NOT NULL DEFAULT 0,
    created_ms INTEGER NOT NULL
);
)SQL";

// Searchable text of a message body: string content, text parts, and tool call names + arguments.
std::string searchable_text(const json & body) {
    std::string out;
    auto add = [&](const std::string & s) {
        if (s.empty()) return;
        if (!out.empty()) out += '\n';
        out += s;
    };
    if (!body.is_object()) {
        if (body.is_string()) add(body.get<std::string>());
        return out;
    }
    if (auto it = body.find("content"); it != body.end()) {
        if (it->is_string()) {
            add(it->get<std::string>());
        } else if (it->is_array()) {
            for (auto & part : *it) {
                if (part.is_string()) add(part.get<std::string>());
                else if (part.is_object() && part.contains("text") && part["text"].is_string())
                    add(part["text"].get<std::string>());
            }
        }
    }
    if (auto it = body.find("tool_calls"); it != body.end() && it->is_array()) {
        for (auto & tc : *it) {
            if (!tc.is_object() || !tc.contains("function") || !tc["function"].is_object()) continue;
            auto & f = tc["function"];
            if (f.contains("name") && f["name"].is_string()) add(f["name"].get<std::string>());
            if (f.contains("arguments") && f["arguments"].is_string()) add(f["arguments"].get<std::string>());
        }
    }
    return out;
}

// Turn free text into a safe FTS5 query: each whitespace token becomes a quoted phrase (implicit AND).
std::string fts_quote(const std::string & q) {
    std::string out;
    size_t i = 0;
    while (i < q.size()) {
        while (i < q.size() && std::isspace((unsigned char) q[i])) i++;
        size_t j = i;
        while (j < q.size() && !std::isspace((unsigned char) q[j])) j++;
        if (j > i) {
            if (!out.empty()) out += ' ';
            out += '"';
            for (size_t k = i; k < j; k++) {
                if (q[k] == '"') out += '"';
                out += q[k];
            }
            out += '"';
        }
        i = j;
    }
    return out;
}

SessionRow read_session(const Stmt & s) {
    SessionRow r;
    r.id = s.str(0); r.title = s.str(1); r.cwd = s.str(2); r.model = s.str(3);
    r.created_ms = s.num(4); r.updated_ms = s.num(5); r.mode = s.str(6);
    r.auto_resume = (int) s.num(7);
    if (r.mode.empty()) r.mode = "chat";
    return r;
}

const char * kSessionCols = "id, title, cwd, model, created_ms, updated_ms, mode, auto_resume";

AgentRow read_agent(const Stmt & s) {
    AgentRow r;
    r.id = s.str(0); r.session = s.str(1); r.parent = s.str(2); r.role = s.str(3); r.task = s.str(4);
    r.status = s.str(5); r.result = s.str(6); r.created_ms = s.num(7); r.updated_ms = s.num(8);
    r.prompt_tokens = s.num(9); r.completion_tokens = s.num(10);
    return r;
}

const char * kAgentCols =
    "id, session, parent, role, task, status, result, created_ms, updated_ms, prompt_tokens, completion_tokens";

MessageRow read_message(const Stmt & s) {
    MessageRow r;
    r.id = s.num(0); r.session = s.str(1); r.agent = s.str(2); r.role = s.str(3);
    r.body = json::parse(s.str(4), nullptr, false);
    if (r.body.is_discarded()) r.body = json::object();
    r.created_ms = s.num(5);
    return r;
}

}  // namespace

Store::Store(const std::string & db_path) {
    if (db_path != ":memory:" && !db_path.empty()) {
        auto parent = std::filesystem::path(db_path).parent_path();
        std::error_code ec;
        if (!parent.empty()) std::filesystem::create_directories(parent, ec);
    }
    int rc = sqlite3_open_v2(db_path.c_str(), &db_,
                             SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string m = db_ ? sqlite3_errmsg(db_) : "out of memory";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("cannot open " + db_path + ": " + m);
    }
    try {
        sqlite3_busy_timeout(db_, 5000);
        exec(db_, "PRAGMA journal_mode=WAL");
        exec(db_, "PRAGMA synchronous=NORMAL");
        exec(db_, "PRAGMA foreign_keys=ON");
        exec(db_, kSchema);
        migrate();
    } catch (...) {
        sqlite3_close(db_);
        db_ = nullptr;
        throw;
    }
}

// Columns added after the first release: ALTER TABLE for databases created by an older build.
void Store::migrate() {
    bool has_mode = false, has_auto_resume = false;
    {
        Stmt s(db_, "PRAGMA table_info(sessions)");
        while (s.step()) {
            if (s.str(1) == "mode") has_mode = true;
            if (s.str(1) == "auto_resume") has_auto_resume = true;
        }
    }
    if (!has_auto_resume) exec(db_, "ALTER TABLE sessions ADD COLUMN auto_resume INTEGER NOT NULL DEFAULT -1");
    if (!has_mode) exec(db_, "ALTER TABLE sessions ADD COLUMN mode TEXT NOT NULL DEFAULT 'chat'");

    // FTS5 virtual table over the message text. External content means the index
    // is derived from the messages table itself rather than storing a copy, so the
    // two stay consistent as long as the sync triggers fire. content_rowid ties
    // each index row back to messages.id (the rowid alias of the primary key).
    // Check before creating: an existing database already has the table and its
    // triggers, while a fresh or pre-search database needs one backfill below.
    bool has_fts = false;
    {
        Stmt s(db_, "SELECT 1 FROM sqlite_master WHERE type='table' AND name='messages_fts'");
        has_fts = s.step();
    }
    exec(db_, "CREATE VIRTUAL TABLE IF NOT EXISTS messages_fts USING fts5(content, content='messages', content_rowid='id')");
    // Three triggers keep the index in lockstep with writes to messages.
    exec(db_, "CREATE TRIGGER IF NOT EXISTS messages_ai AFTER INSERT ON messages BEGIN "
               "INSERT INTO messages_fts(rowid, content) VALUES (new.id, new.content); END;");
    exec(db_, "CREATE TRIGGER IF NOT EXISTS messages_ad AFTER DELETE ON messages BEGIN "
               "INSERT INTO messages_fts(messages_fts, rowid, content) VALUES ('delete', old.id, old.content); END;");
    exec(db_, "CREATE TRIGGER IF NOT EXISTS messages_au AFTER UPDATE ON messages BEGIN "
               "INSERT INTO messages_fts(messages_fts, rowid, content) VALUES ('delete', old.id, old.content); "
               "INSERT INTO messages_fts(rowid, content) VALUES (new.id, new.content); END;");
    // Index rows written before this table existed. Only when the table is new:
    // an existing index is already in sync and rebuilding it on every open would
    // scan the whole message history for nothing.
    if (!has_fts) exec(db_, "INSERT INTO messages_fts(messages_fts) VALUES('rebuild')");
}

Store::~Store() {
    if (db_) sqlite3_close(db_);
}

SessionRow Store::create_session(const std::string & title, const std::string & cwd, const std::string & model,
                                 const std::string & mode) {
    std::lock_guard<std::mutex> lk(mu_);
    SessionRow r;
    r.id = new_id("s");
    r.title = title; r.cwd = cwd; r.model = model;
    r.mode = mode.empty() ? "chat" : mode;
    r.created_ms = r.updated_ms = now_ms();
    Stmt s(db_, "INSERT INTO sessions(id, title, cwd, model, created_ms, updated_ms, mode) VALUES (?,?,?,?,?,?,?)");
    s.text(1, r.id).text(2, title).text(3, cwd).text(4, model).i64(5, r.created_ms).i64(6, r.updated_ms)
        .text(7, r.mode).run();
    return r;
}

std::optional<SessionRow> Store::get_session(const std::string & id) const {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, (std::string("SELECT ") + kSessionCols + " FROM sessions WHERE id=?").c_str());
    s.text(1, id);
    if (!s.step()) return std::nullopt;
    return read_session(s);
}

std::vector<SessionRow> Store::list_sessions(size_t limit, long long before_ms) const {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, (std::string("SELECT ") + kSessionCols +
                 " FROM sessions WHERE (?1 = 0 OR updated_ms < ?1) ORDER BY updated_ms DESC, rowid DESC LIMIT ?2").c_str());
    s.i64(1, before_ms).i64(2, (long long) limit);
    std::vector<SessionRow> out;
    while (s.step()) out.push_back(read_session(s));
    return out;
}

void Store::rename_session(const std::string & id, const std::string & title) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "UPDATE sessions SET title=?, updated_ms=? WHERE id=?");
    s.text(1, title).i64(2, now_ms()).text(3, id).run();
}

bool Store::set_session_mode(const std::string & id, const std::string & mode) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "UPDATE sessions SET mode=? WHERE id=?");
    s.text(1, mode).text(2, id).run();
    return sqlite3_changes(db_) > 0;
}

bool Store::set_session_auto_resume(const std::string & id, int v) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "UPDATE sessions SET auto_resume=? WHERE id=?");
    s.i64(1, v < 0 ? -1 : v > 0 ? 1 : 0).text(2, id).run();
    return sqlite3_changes(db_) > 0;
}

void Store::delete_session(const std::string & id) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "DELETE FROM sessions WHERE id=?");
    s.text(1, id).run();
}

AgentRow Store::create_agent(const std::string & session, const std::string & parent, const std::string & role,
                             const std::string & task) {
    std::lock_guard<std::mutex> lk(mu_);
    AgentRow a;
    a.id = new_id("a");
    a.session = session; a.parent = parent; a.role = role; a.task = task;
    a.status = "queued";
    a.created_ms = a.updated_ms = now_ms();
    Stmt s(db_, "INSERT INTO agents(id, session, parent, role, task, status, result, created_ms, updated_ms, "
                "prompt_tokens, completion_tokens) VALUES (?,?,?,?,?,?,?,?,?,0,0)");
    s.text(1, a.id).text(2, session).text(3, parent).text(4, role).text(5, task).text(6, a.status).text(7, "")
        .i64(8, a.created_ms).i64(9, a.updated_ms).run();
    return a;
}

int Store::interrupt_orphans() {
    exec(db_, "UPDATE agents SET status='interrupted', result='stopped: Judas restarted mid-turn' WHERE status='running'");
    return sqlite3_changes(db_);
}

void Store::update_agent(const AgentRow & a) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "UPDATE agents SET status=?, result=?, prompt_tokens=?, completion_tokens=?, updated_ms=? WHERE id=?");
    s.text(1, a.status).text(2, a.result).i64(3, a.prompt_tokens).i64(4, a.completion_tokens).i64(5, now_ms())
        .text(6, a.id).run();
}

std::optional<AgentRow> Store::get_agent(const std::string & id) const {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, (std::string("SELECT ") + kAgentCols + " FROM agents WHERE id=?").c_str());
    s.text(1, id);
    if (!s.step()) return std::nullopt;
    return read_agent(s);
}

std::vector<AgentRow> Store::list_agents(const std::string & session) const {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, (std::string("SELECT ") + kAgentCols +
                 " FROM agents WHERE session=? ORDER BY created_ms, rowid").c_str());
    s.text(1, session);
    std::vector<AgentRow> out;
    while (s.step()) out.push_back(read_agent(s));
    return out;
}

long long Store::add_message(const std::string & session, const std::string & agent, const std::string & role,
                             const json & body) {
    std::lock_guard<std::mutex> lk(mu_);
    std::string text = body.dump(-1, ' ', false, json::error_handler_t::replace);
    std::string content = searchable_text(body);
    long long t = now_ms();
    exec(db_, "BEGIN");
    try {
        {
            Stmt s(db_, "INSERT INTO messages(session, agent, role, body, content, created_ms) VALUES (?,?,?,?,?,?)");
            s.text(1, session).text(2, agent).text(3, role).text(4, text).text(5, content).i64(6, t).run();
        }
        long long id = sqlite3_last_insert_rowid(db_);
        {
            Stmt s(db_, "UPDATE sessions SET updated_ms=? WHERE id=?");
            s.i64(1, t).text(2, session).run();
        }
        exec(db_, "COMMIT");
        return id;
    } catch (...) {
        sqlite3_exec(db_, "ROLLBACK", nullptr, nullptr, nullptr);
        throw;
    }
}

std::vector<MessageRow> Store::messages(const std::string & session, const std::string & agent) const {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT id, session, agent, role, body, created_ms FROM messages "
                "WHERE session=? AND agent=? ORDER BY id");
    s.text(1, session).text(2, agent);
    std::vector<MessageRow> out;
    while (s.step()) out.push_back(read_message(s));
    return out;
}

std::vector<MessageRow> Store::messages_page(const std::string & session, long long before_id, size_t limit,
                                             bool * more, long long * total) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<MessageRow> out;
    {
        Stmt s(db_, "SELECT id, session, agent, role, body, created_ms FROM messages "
                    "WHERE session=?1 AND (?2 = 0 OR id < ?2) ORDER BY id DESC LIMIT ?3");
        s.text(1, session).i64(2, before_id).i64(3, limit == 0 ? -1 : (long long) limit + 1);
        while (s.step()) out.push_back(read_message(s));
    }
    bool has_more = limit > 0 && out.size() > limit;
    if (has_more) out.resize(limit);
    std::reverse(out.begin(), out.end());
    if (more) *more = has_more;
    if (total) {
        Stmt c(db_, "SELECT COUNT(*) FROM messages WHERE session=?");
        c.text(1, session);
        *total = c.step() ? c.num(0) : 0;
    }
    return out;
}

long long Store::last_message_id(const std::string & session) const {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT COALESCE(MAX(id), 0) FROM messages WHERE session=?");
    s.text(1, session);
    return s.step() ? s.num(0) : 0;
}

std::vector<MessageRow> Store::search(const std::string & query, size_t limit) const {
    std::string fq = fts_quote(query);
    std::vector<MessageRow> out;
    if (fq.empty()) return out;
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT m.id, m.session, m.agent, m.role, m.body, m.created_ms FROM messages_fts f "
                "JOIN messages m ON m.id = f.rowid WHERE messages_fts MATCH ? "
                "ORDER BY m.created_ms DESC, m.id DESC LIMIT ?");
    s.text(1, fq).i64(2, (long long) limit);
    while (s.step()) out.push_back(read_message(s));
    return out;
}

std::vector<SearchHit> Store::search_messages(const std::string & query, int limit,
                                              const std::string & session_id) const {
    std::vector<SearchHit> out;
    std::string fq = fts_quote(query);
    if (fq.empty() || limit <= 0) return out;
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT m.session, s.title, m.id, m.role, m.created_ms, "
                "snippet(messages_fts, 0, '[', ']', '...', 32) "
                "FROM messages_fts f JOIN messages m ON m.id = f.rowid "
                "JOIN sessions s ON s.id = m.session "
                "WHERE messages_fts MATCH ?1 AND (?2 = '' OR m.session = ?2) "
                "ORDER BY m.created_ms DESC, m.id DESC LIMIT ?3");
    s.text(1, fq).text(2, session_id).i64(3, limit);
    while (s.step()) {
        SearchHit h;
        h.session = s.str(0);
        h.title = s.str(1);
        h.message_id = s.num(2);
        h.role = s.str(3);
        h.created_ms = s.num(4);
        h.snippet = s.str(5);
        out.push_back(std::move(h));
    }
    return out;
}

static Store::TaskRow read_task(Stmt & s) {
    Store::TaskRow t;
    t.id = s.str(0);
    t.title = s.str(1);
    t.prompt = s.str(2);
    t.cwd = s.str(3);
    t.kind = s.str(4);
    t.run_at_ms = s.num(5);
    t.interval_s = s.num(6);
    t.next_ms = s.num(7);
    t.enabled = s.num(8) != 0;
    t.last_session = s.str(9);
    t.last_run_ms = s.num(10);
    t.created_ms = s.num(11);
    return t;
}
static const char * kTaskCols =
    "id, title, prompt, cwd, kind, run_at_ms, interval_s, next_ms, enabled, last_session, last_run_ms, created_ms";

Store::TaskRow Store::create_task(const TaskRow & in) {
    TaskRow t = in;
    if (t.id.empty()) t.id = new_id("t");
    t.created_ms = now_ms();
    if (t.next_ms == 0) {
        if (t.kind == "once") t.next_ms = t.run_at_ms;
        else if (t.kind == "interval") t.next_ms = now_ms() + t.interval_s * 1000;
    }
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "INSERT INTO tasks (id,title,prompt,cwd,kind,run_at_ms,interval_s,next_ms,enabled,last_session,"
                "last_run_ms,created_ms) VALUES (?,?,?,?,?,?,?,?,?,?,?,?)");
    s.text(1, t.id).text(2, t.title).text(3, t.prompt).text(4, t.cwd).text(5, t.kind).i64(6, t.run_at_ms)
        .i64(7, t.interval_s).i64(8, t.next_ms).i64(9, t.enabled ? 1 : 0).text(10, t.last_session)
        .i64(11, t.last_run_ms).i64(12, t.created_ms);
    s.run();
    return t;
}

std::vector<Store::TaskRow> Store::list_tasks() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TaskRow> out;
    Stmt s(db_, (std::string("SELECT ") + kTaskCols + " FROM tasks ORDER BY created_ms DESC").c_str());
    while (s.step()) out.push_back(read_task(s));
    return out;
}

std::optional<Store::TaskRow> Store::get_task(const std::string & id) const {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, (std::string("SELECT ") + kTaskCols + " FROM tasks WHERE id = ?").c_str());
    s.text(1, id);
    if (s.step()) return read_task(s);
    return std::nullopt;
}

void Store::update_task(const TaskRow & t) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "UPDATE tasks SET title=?,prompt=?,cwd=?,kind=?,run_at_ms=?,interval_s=?,next_ms=?,enabled=?,"
                "last_session=?,last_run_ms=? WHERE id=?");
    s.text(1, t.title).text(2, t.prompt).text(3, t.cwd).text(4, t.kind).i64(5, t.run_at_ms).i64(6, t.interval_s)
        .i64(7, t.next_ms).i64(8, t.enabled ? 1 : 0).text(9, t.last_session).i64(10, t.last_run_ms).text(11, t.id);
    s.run();
}

void Store::delete_task(const std::string & id) {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "DELETE FROM tasks WHERE id = ?");
    s.text(1, id);
    s.run();
}

std::vector<Store::TaskRow> Store::due_tasks(long long now) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<TaskRow> out;
    Stmt s(db_, (std::string("SELECT ") + kTaskCols +
                 " FROM tasks WHERE enabled=1 AND kind!='manual' AND next_ms>0 AND next_ms<=? ORDER BY next_ms")
                    .c_str());
    s.i64(1, now);
    while (s.step()) out.push_back(read_task(s));
    return out;
}

std::optional<std::string> Store::tool_output(const std::string & session, const std::string & tcid) const {
    std::lock_guard<std::mutex> lk(mu_);
    Stmt s(db_, "SELECT body FROM messages WHERE session=? AND role='tool' AND "
                "json_extract(body,'$.tool_call_id')=? ORDER BY id DESC LIMIT 1");
    s.text(1, session).text(2, tcid);
    if (s.step()) {
        try {
            return json::parse(s.str(0)).value("content", "");
        } catch (...) {
        }
    }
    return std::nullopt;
}

json Store::usage_stats(int days) const {
    std::lock_guard<std::mutex> lk(mu_);
    json out;
    {
        Stmt s(db_, "SELECT (SELECT COUNT(*) FROM messages WHERE role='user'), (SELECT COUNT(*) FROM sessions), "
                    "(SELECT COALESCE(SUM(prompt_tokens),0) FROM agents), "
                    "(SELECT COALESCE(SUM(completion_tokens),0) FROM agents)");
        s.step();
        out["turns"] = s.num(0);
        out["sessions"] = s.num(1);
        out["prompt_tokens"] = s.num(2);
        out["completion_tokens"] = s.num(3);
    }
    // One "turn" per user message, grouped by local calendar date.
    json daily = json::array();
    {
        Stmt s(db_, "SELECT date(created_ms/1000,'unixepoch','localtime') d, COUNT(*) FROM messages "
                    "WHERE role='user' AND created_ms/1000 >= strftime('%s','now',? ) GROUP BY d ORDER BY d");
        s.text(1, "-" + std::to_string(days) + " days");
        while (s.step()) daily.push_back({{"date", s.str(0)}, {"count", s.num(1)}});
    }
    out["daily"] = daily;
    json models = json::array();
    {
        Stmt s(db_, "SELECT model, COUNT(*) c FROM sessions GROUP BY model ORDER BY c DESC");
        while (s.step()) models.push_back({{"model", s.str(0)}, {"sessions", s.num(1)}});
    }
    out["models"] = models;
    return out;
}

}  // namespace judas
