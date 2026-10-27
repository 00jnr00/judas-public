// Skill usage telemetry backed by its own SQLite file. All calls are serialized by one mutex.
#include "skills/usage.h"

#include <sqlite3.h>

#include <chrono>
#include <filesystem>
#include <stdexcept>

namespace judas {
namespace fs = std::filesystem;

namespace {

long long usage_now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

[[noreturn]] void db_error(sqlite3 * db, const char * what) {
    throw std::runtime_error(std::string("skills.db: ") + what + ": " + sqlite3_errmsg(db));
}

void exec(sqlite3 * db, const char * sql) {
    char * err = nullptr;
    if (sqlite3_exec(db, sql, nullptr, nullptr, &err) != SQLITE_OK) {
        std::string m = err ? err : "unknown";
        sqlite3_free(err);
        throw std::runtime_error("skills.db: " + m);
    }
}

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

const char * kSchema = R"SQL(
CREATE TABLE IF NOT EXISTS skill_usage (
    name TEXT PRIMARY KEY,
    loads INTEGER NOT NULL DEFAULT 0,
    autoloads INTEGER NOT NULL DEFAULT 0,
    patches INTEGER NOT NULL DEFAULT 0,
    created_ms INTEGER NOT NULL,
    last_used_ms INTEGER NOT NULL DEFAULT 0,
    state TEXT NOT NULL DEFAULT 'active'
);
)SQL";

}  // namespace

SkillUsage::SkillUsage(std::string path) {
    fs::path p(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    int rc = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "cannot open database";
        sqlite3_close(db_);
        db_ = nullptr;
        throw std::runtime_error("skills.db: " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec(db_, kSchema);
    // Migration: databases created before the curator joined lack the state column. Fresh rows default to
    // active, which is what anything recorded pre-curator effectively was.
    {
        Stmt has_state(db_, "SELECT COUNT(*) FROM pragma_table_info('skill_usage') WHERE name = 'state'");
        if (!has_state.step() || has_state.num(0) == 0)
            exec(db_, "ALTER TABLE skill_usage ADD COLUMN state TEXT NOT NULL DEFAULT 'active'");
    }
}

SkillUsage::~SkillUsage() {
    if (db_) sqlite3_close(db_);
}

void SkillUsage::record_load(const std::string & name, bool autoload) {
    std::lock_guard lk(mu_);
    long long now = usage_now_ms();
    Stmt s(db_, "INSERT INTO skill_usage(name, loads, autoloads, patches, created_ms, last_used_ms) "
                "VALUES(?1, 1, ?2, 0, ?3, ?3) "
                "ON CONFLICT(name) DO UPDATE SET loads = loads + 1, "
                "autoloads = autoloads + excluded.autoloads, last_used_ms = excluded.last_used_ms");
    s.text(1, name).i64(2, autoload ? 1 : 0).i64(3, now).run();
}

void SkillUsage::record_patch(const std::string & name) {
    std::lock_guard lk(mu_);
    long long now = usage_now_ms();
    Stmt s(db_, "INSERT INTO skill_usage(name, loads, autoloads, patches, created_ms, last_used_ms) "
                "VALUES(?1, 0, 0, 1, ?2, ?2) "
                "ON CONFLICT(name) DO UPDATE SET patches = patches + 1, last_used_ms = excluded.last_used_ms");
    s.text(1, name).i64(2, now).run();
}

void SkillUsage::record_create(const std::string & name) {
    std::lock_guard lk(mu_);
    Stmt s(db_, "INSERT INTO skill_usage(name, loads, autoloads, patches, created_ms, last_used_ms, state) "
                "VALUES(?1, 0, 0, 0, ?2, 0, 'active') "
                "ON CONFLICT(name) DO UPDATE SET created_ms = excluded.created_ms, last_used_ms = 0, state = 'active'");
    // a re-created skill is a new generation: the curator must not judge it by the archived one's idle time
    s.text(1, name).i64(2, usage_now_ms()).run();
}

void SkillUsage::set_state(const std::string & name, const std::string & state) {
    if (state != "active" && state != "stale")
        throw std::runtime_error("skills.db: unknown state '" + state + "'");
    std::lock_guard lk(mu_);
    long long now = usage_now_ms();
    // A missing record is created as a side effect, so a stale call can never silently vanish.
    Stmt s(db_, "INSERT INTO skill_usage(name, loads, autoloads, patches, created_ms, last_used_ms, state) "
                "VALUES(?1, 0, 0, 0, ?2, 0, ?3) "
                "ON CONFLICT(name) DO UPDATE SET state = excluded.state");
    s.text(1, name).i64(2, now).text(3, state).run();
}

std::vector<SkillUsageRow> SkillUsage::all() const {
    std::lock_guard lk(mu_);
    Stmt s(db_, "SELECT name, loads, autoloads, patches, created_ms, last_used_ms, state "
                "FROM skill_usage ORDER BY name");
    std::vector<SkillUsageRow> out;
    while (s.step()) {
        SkillUsageRow r;
        r.name = s.str(0);
        r.loads = (int) s.num(1);
        r.autoloads = (int) s.num(2);
        r.patches = (int) s.num(3);
        r.created_ms = s.num(4);
        r.last_used_ms = s.num(5);
        r.state = s.str(6);
        out.push_back(std::move(r));
    }
    return out;
}

std::optional<SkillUsageRow> SkillUsage::get(const std::string & name) const {
    std::lock_guard lk(mu_);
    Stmt s(db_, "SELECT name, loads, autoloads, patches, created_ms, last_used_ms, state "
                "FROM skill_usage WHERE name = ?1");
    s.text(1, name);
    if (!s.step()) return std::nullopt;
    SkillUsageRow r;
    r.name = s.str(0);
    r.loads = (int) s.num(1);
    r.autoloads = (int) s.num(2);
    r.patches = (int) s.num(3);
    r.created_ms = s.num(4);
    r.last_used_ms = s.num(5);
    r.state = s.str(6);
    return r;
}

}  // namespace judas
