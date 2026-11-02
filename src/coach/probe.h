// Coach P1: probes. Run one task twice per round, once under the base harness and once with a proposal's
// overlay, tool notes and skills added for that session only, each in a fresh copy of a fixture directory.
// Compare mechanical signals (an optional check command's exit code, tool calls, writes, read streaks, nudges,
// tokens, wall time). Nothing is adopted and the user's real working directory is never touched.
// Clean-room; see docs/COACH.md.
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "agent/agent.h"
#include "coach/coach.h"

namespace judas {

struct ProbeOptions {
    std::string task;              // the prompt both arms receive
    std::string fixture;           // directory copied fresh for every arm and round
    std::string check;             // optional shell command run in the arm's work dir (never by the model)
    std::string mode = "code";     // session mode
    int k = 1;                     // rounds
    int timeout_s = 1800;          // per run
    int check_timeout_s = 300;
    // Where the scratch trees go. Empty = $XDG_CACHE_HOME/judas/probes (else ~/.cache/judas/probes). Never under
    // data_dir: tools refuse writes to Judas's own data, so an arm working there could not edit anything.
    std::string work_root;
};

// The scratch root for proposal `id` (work_root/<id>), or "" with *err when it would sit inside data_dir.
std::string coach_probe_work_root(const Config & cfg, const ProbeOptions & o, const std::string & id, std::string * err);

struct ProbeRun {
    std::string arm;               // base | variant
    int round = 0;                 // 1-based
    std::string session;
    std::string work;              // work dir
    std::string status;            // done | paused | error | killed | timeout
    int check_exit = -1;           // -1 = no check, 124 = check timed out
    std::string check_tail;        // last 40 lines of the check output
    double wall_s = 0;
    CoachStats stats;
    std::string signals;           // coach_signals text
    bool passed() const;           // check exit 0 when a check ran, else status == done
};

struct ProbeResult {
    bool ok = false;
    std::string error;
    std::string dir;               // <data_dir>/coach/<id>/probes
    std::string verdict;
    std::vector<ProbeRun> runs;
};

// What the variant arm gets: overlay.md and tools.md (tool notes under a heading), joined. Sets *err and
// returns "" when either file touches the safety floor or is over the size caps.
std::string coach_probe_overlay(const Config & cfg, const CoachVariant & v, std::string * err);
// Copy the proposal's skills that pass the safety floor and the skills guard into dst (created). Returns
// the number copied; the rest are named in *skipped.
int coach_probe_stage_skills(const CoachVariant & v, const std::string & dst, std::vector<std::string> * skipped = nullptr);
// Copy a fixture directory into dst with reflinks where the filesystem has them.
bool coach_probe_copy_fixture(const std::string & src, const std::string & dst, std::string * err);
// Run a shell command in dir with a timeout; returns its exit code (124 on timeout), *tail gets the last 40 lines.
int coach_probe_run_check(const std::string & cmd, const std::string & dir, int timeout_s, std::string * tail);

// "variant better", "base better" or "no clear difference", with the numbers; k = 1 is marked low confidence.
std::string coach_probe_verdict(const std::vector<ProbeRun> & runs, int k);
std::string coach_probe_evidence_md(const std::string & id, const ProbeOptions & o, const std::vector<ProbeRun> & runs,
                                    const std::string & verdict);
json coach_probe_evidence_json(const std::string & id, const ProbeOptions & o, const std::vector<ProbeRun> & runs,
                               const std::string & verdict);

// Run the whole probe (blocks; sequential, base first each round). Writes <id>/probes/evidence.{md,json} and
// marks the proposal probed.
ProbeResult coach_probe(const Config & cfg, AgentManager & agents, Store & store, const std::string & id,
                        const ProbeOptions & o, const std::atomic<bool> * cancel = nullptr);

// The evidence table of a proposal ("" when it has not been probed).
std::string coach_probe_evidence_text(const Config & cfg, const std::string & id);

}  // namespace judas
