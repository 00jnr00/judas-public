// Skills: on-demand Markdown instructions. A skill file has YAML-ish frontmatter (name, description, triggers)
// and a Markdown body. Only the one-line descriptions sit in the agent prompt; a body loads when a trigger
// word appears in the user's message or the model calls the load_skill tool. This extends Judas without code.
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <vector>

#include "core/config.h"

namespace judas {

struct Skill {
    std::string name;                     // unique; from frontmatter or filename stem
    std::string description;              // one line, shown in the prompt index
    std::vector<std::string> triggers;    // lowercase words/phrases that auto-load the body
    std::string body;                     // Markdown instructions (loaded lazily by the agent)
    std::string path;
    bool pinned = false;                  // protected from patch/rewrite/archive by skill_manage
    std::string source = "local";         // local | agent | external:<origin>
    int version = 1;                      // frontmatter version, bumped by every skill_manage write
};

class Skills {
public:
    explicit Skills(const SkillsConfig & cfg);
    // Re-read every path. Archived and quarantined files are never loaded.
    void reload();
    // All skills found under cfg.paths (never throws; a bad file is skipped). A copy: skill_manage reloads
    // from one agent's thread while other agents read, so nothing may hold a reference into skills_.
    std::vector<Skill> all() const;
    std::optional<Skill> by_name(const std::string & name) const;
    // Skills whose triggers match the text on word boundaries, for autoload = "trigger".
    std::vector<Skill> matching(const std::string & text) const;
    // The prompt block listing each skill's name + description (empty if none). Kept small.
    std::string index_block() const;
    // Where new skills are written: first cfg.paths entry, ~ expanded, created when missing.
    std::string write_dir() const;
    // Cheap refresh after a lead turn: reload() only when the skill dirs actually changed
    // (newest mtime or file count of the .md files), and at most once per ~2 s. Returns true
    // when it actually reloaded, false when it short-circuited (no work to do).
    bool reload_if_changed();

private:
    // Combined fingerprint of the skill dirs, so a reload can compare before recomputing.
    uint64_t dir_key() const;

    SkillsConfig cfg_;
    std::mutex check_mu_;                          // guards cache_key_ and last_check_
    mutable std::shared_mutex mu_;   // reload() swaps under unique; readers share
    std::vector<Skill> skills_;
    uint64_t cache_key_ = 0;                       // fingerprint from the last applied reload
    std::chrono::steady_clock::time_point last_check_{};
    bool checked_ = false;                         // false until the first reload_if_changed call
};

}  // namespace judas
