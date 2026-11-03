// /snapshots and /revert for the terminal UI: parsing GET /api/snapshots, diff statistics, colored diff rows,
// the revert preview and result. Pure (no network, no terminal): unit tested in tests/test_tui_snapshots.cpp.
#pragma once
#include <string>
#include <vector>

#include "core/json.h"
#include "tui/text.h"
#include "tui/theme.h"

namespace judas::tui {

struct SnapInfo {
    std::string id, session, agent, label;
    long long created_ms = 0;
    bool committed = false, reverted = false;
    std::vector<std::string> files, irreversible;
    int added = -1, removed = -1;  // line counts from the diff, -1 until known
};
// The "snapshots" array of GET /api/snapshots. Newest first.
std::vector<SnapInfo> parse_snapshots(const judas::json & snapshots);
// Keep only snapshots that touched a file under `folder` (used when there is no current session).
std::vector<SnapInfo> in_folder(const std::vector<SnapInfo> & all, const std::string & folder);
// Exact id, else a unique prefix, else a unique suffix ("0007"). "" when none or ambiguous.
std::string resolve_snapshot_id(const std::vector<SnapInfo> & all, const std::string & query);

struct DiffStat {
    int added = 0, removed = 0, files = 0;
};
// Counts "+"/"-" body lines of a unified diff (the "+++ "/"--- " headers are not counted).
DiffStat diff_stat(const std::string & diff);

// "14:05" for today, "Nov 02 14:05" otherwise, local time.
std::string format_clock(long long ms, long long now_ms);

// One-screen table: time, agent, files, +/-, label. `width` is the terminal width.
std::vector<std::string> render_snapshot_list(const std::vector<SnapInfo> & snaps, long long now_ms, int width,
                                              const Palette & pal, const std::string & scope);
// The unified diff as colored, width-limited rows (long lines wrap). Added lines use look.diff.add, removed
// look.diff.del; foreground colors only.
std::vector<std::string> render_diff(const std::string & diff, int width, const ThemeLook & look);
// What /revert ID is about to do: the files, the line counts, a warning for irreversible side effects.
std::vector<std::string> render_revert_preview(const SnapInfo & s, const DiffStat & st, bool to, int width,
                                               const Palette & pal);
// The outcome of POST /api/snapshots/<id>/revert (same fields the web shows).
std::vector<std::string> render_revert_result(const judas::json & result, int width, const Palette & pal);

// Paging: rows [pos, pos+page) and the next position.
std::vector<std::string> page_slice(const std::vector<std::string> & rows, size_t pos, size_t page, size_t * next);

}  // namespace judas::tui
