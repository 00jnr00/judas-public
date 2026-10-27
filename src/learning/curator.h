// Curator: scheduled lifecycle manager for the agent-authored skill library, following the curator design of
// Hermes Agent (Nous Research, MIT; see THIRD_PARTY.md). Agent-created skills that sit unused go stale; long-unused ones move into
// .archive/ (never deleted). Every pass backs the skills dir up first, writes state to
// <data_dir>/curator.json before touching anything, and leaves a Markdown report. Pinned skills and every
// other source are user owned and never touched. This version ships no LLM consolidation pass.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "skills/skills.h"
#include "skills/usage.h"

namespace judas {

// One planned change. action: seed | stale | activate | archive; reason is a short why for the report.
struct CuratorTransition {
    std::string name;
    std::string action;
    std::string reason;
};
struct CuratorPlan {
    std::vector<CuratorTransition> transitions;
};

// Pure: decide what to do with each curated skill (source = agent, unpinned) at now_ms. Inactivity anchor
// is last_used_ms, else created_ms. Archive past archive_after_days, else stale past stale_after_days; a
// stale skill whose anchor is fresh goes back to active; a skill with no usage record yet is seeded
// (created_ms = now) and deferred to the next pass.
CuratorPlan plan_transitions(const std::vector<Skill> & skills, const std::vector<SkillUsageRow> & rows,
                             long long now_ms, const LearningConfig & cfg);

struct CuratorState {
    long long last_run_ms = 0;    // 0 = never ran
    int run_count = 0;
    std::string summary;
};
// Stored as <data_dir>/curator.json {last_run_ms, run_count, last_summary}. A missing or corrupt file reads
// back as nullopt so a bad file costs a fresh seed, not a parse error.
std::optional<CuratorState> read_curator_state(const std::string & data_dir);
void write_curator_state(const std::string & data_dir, const CuratorState & s);

struct CuratorRunResult {
    bool ran = false;          // false: the first run seeded its state, or the backup failed
    bool first_run = false;
    bool dry_run = false;
    std::vector<CuratorTransition> transitions;  // applied in real runs, planned in dry runs
    std::string summary;       // one line, e.g. "archived 1, stale 2, reactivated 0, seeded 3"
    std::string report_path;
    std::string error;
};

// One curator pass: backup the skills dir into <data_dir>/skill-backups/ (keep cfg.curator_backups newest;
// a failed backup aborts the run), plan, write state BEFORE applying, then move the archive set and switch
// usage states. dry_run plans and writes only the report and touches nothing else.
CuratorRunResult run_curator(const LearningConfig & cfg, const std::vector<Skill> & skills,
                             const std::string & data_dir, const std::string & skills_dir, SkillUsage & usage,
                             long long now_ms, bool dry_run);

}  // namespace judas
