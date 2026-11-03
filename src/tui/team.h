// Swarm team view for the terminal UI: parsing GET /api/sessions/<id>/team, the compact text map printed by
// /team and redrawn by /team watch, hand-off notes from `team` events, and the row lookup used by /pause,
// /resume, /cancel-agent and /read. Pure (no terminal, no HTTP), unit tested in tests/test_tui_team.cpp.
#pragma once
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"
#include "tui/text.h"

namespace judas::tui {

struct TeamRow {
    int n = 0;                   // row number shown by /team (0 for the lead and for unnumbered rows)
    std::string team, member, copy, agent, role, model, activity, tool, last, result;
    double tok_s = 0;
    int ctx_pct = -1;            // -1 = unknown
    long long tokens = 0;        // prompt + completion
    bool team_lead = false;
    bool spawned = true;         // false: the copy exists in the spec but has no agent yet
};
struct TeamBlock {
    std::string name, brief;     // brief: first line only
    bool disbanded = false;
    std::vector<TeamRow> rows;   // team lead first, then failed and blocked, working, waiting, idle, done
};
struct TeamView {
    std::optional<TeamRow> lead;
    std::vector<TeamBlock> teams;
    int count = 0;               // numbered rows
    bool empty() const { return teams.empty(); }
};

TeamView parse_team(const judas::json & body);

// Glyph and state word, same vocabulary as the web Map tab.
std::string team_glyph(const std::string & activity);
std::string team_word(const std::string & activity);
// Escape color for a state (ok / warn / err / dim) and for a context percentage band (<60 ok, 60 to 80 warn, >80 err).
std::string state_color(const Palette & pal, const std::string & activity);
std::string ctx_color(const Palette & pal, int pct);

// The text map: lead line, then one block per team. Fits `width` columns; extra width shows more of the tail.
std::vector<std::string> render_team(const TeamView & v, const Palette & pal, int width);

// "editor -> reader.2 assigned" for team events (assigned, message, report, created, disbanded); nullopt otherwise.
std::optional<std::string> handoff_note(const judas::json & event);

// Row for "3", "reader.2", an agent id (or prefix), "lead", or a unique member name. nullptr when nothing matches
// or the key is ambiguous.
const TeamRow * find_row(const TeamView & v, const std::string & key);

// The text /read prints: the final result when the agent has finished, else the tail of its latest output.
// Bounded to max_chars (cut on a UTF-8 boundary, with a note when cut).
std::string agent_output_text(const TeamRow & r, size_t max_chars);

// Splits "N|copy [chars]" for /read: key and optional number (0 = default).
void split_key_arg(const std::string & arg, std::string & key, size_t & num);

}  // namespace judas::tui
