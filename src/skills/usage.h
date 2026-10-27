// Skill usage telemetry: per-skill load, autoload, patch and create counters in a small SQLite file.
// The database is separate from sessions.db so the curator and skills tools can read it without the store.
#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct sqlite3;

namespace judas {

struct SkillUsageRow {
    std::string name;
    int loads = 0;            // load_skill calls
    int autoloads = 0;        // trigger matches that injected the body
    int patches = 0;          // successful patch/rewrite writes
    long long created_ms = 0;
    long long last_used_ms = 0;  // 0 until a load or patch happens
    std::string state = "active";  // active | stale, curator bookkeeping
};

class SkillUsage {
public:
    // path is the database file, usually <data_dir>/skills.db. Parent directories are created.
    explicit SkillUsage(std::string path);
    ~SkillUsage();
    SkillUsage(const SkillUsage &) = delete;
    SkillUsage & operator=(const SkillUsage &) = delete;

    void record_load(const std::string & name, bool autoload);
    void record_patch(const std::string & name);
    void record_create(const std::string & name);
    // Curator lifecycle: active | stale. Anything else is refused so a typo cannot corrupt the column.
    void set_state(const std::string & name, const std::string & state);
    std::vector<SkillUsageRow> all() const;
    std::optional<SkillUsageRow> get(const std::string & name) const;

private:
    sqlite3 * db_ = nullptr;
    mutable std::mutex mu_;
};

}  // namespace judas
