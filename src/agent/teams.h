// Swarm teams (docs/SWARM-TEAMS.md): the pure parts. A team spec (members, edges, a shared brief and house rules),
// its parsing and validation, edge checks between member copies, the pinned brief block every member carries, and
// the status merge used by the Swarm screen. No threads, no engine: AgentManager owns the runtime.
#pragma once

#include <string>
#include <vector>

#include "core/json.h"

namespace judas {

struct TeamMemberSpec {
    std::string id;                    // [a-z0-9_-], unique in the team; copies are id.1 .. id.N when count > 1
    std::string role = "worker";       // lead (the team lead, at most one, count 1) | worker | reviewer | custom role
    int count = 1;                     // 1..8
    std::vector<std::string> skills;   // skill names; their bodies are given to the member at start
    std::string model = "gpu";         // gpu (main model) | cpu (the swarm instance)
};

struct TeamEdge {
    std::string from, to;              // member ids (not copies); an edge from x to x links copies of x
    std::string kind = "message";      // message | observe
};

struct TeamSpec {
    std::string name;                  // [a-z0-9_-], 1..40
    std::string brief;                 // the shared culture: goal, constraints, output format
    std::vector<std::string> rules;    // house rules, pinned verbatim
    std::vector<std::string> rule_files;  // "path" or "path#Heading" (one Markdown section), pinned verbatim
    std::vector<TeamMemberSpec> members;
    std::vector<TeamEdge> edges;       // extra edges; the defaults always exist (see edge_allows)
    std::vector<std::vector<std::string>> place;   // pinned map rows of copies (empty = automatic layout)
};

// Parse from the tool's JSON arguments or a TOML file. "" on success, else the reason. Unknown keys are an error
// (so a spec can never sneak in permissions, sandbox or tool settings).
std::string team_spec_from_json(const json & j, TeamSpec & out);
std::string team_spec_from_toml(const std::string & text, TeamSpec & out);
json team_spec_to_json(const TeamSpec & s);

// Checks names, counts, roles (lead at most once, count 1), models, edges naming real members, and that the team
// fits: total copies <= max_members, depth of the deepest member <= max_depth given the creator's depth.
std::string validate_team_spec(const TeamSpec & s, int max_members, int creator_depth, int max_depth);

// Team map files (docs/SWARM-TEAMS.md "Map files"): one fact per line, keyword first, lenient. A bad line becomes
// a warning and is skipped; nothing in a map can touch permissions. Lines before the first `team` line belong to
// a team named `default_name`. Each team is NOT validated here (call validate_team_spec).
struct TeamMap {
    std::vector<TeamSpec> teams;
    std::vector<std::string> warnings;   // "line 7: ..." for every skipped or corrected line
};
TeamMap parse_team_map(const std::string & text, const std::string & default_name = "team");
// The canonical text of one team (what the harness writes back and the UI shows).
std::string team_map_text(const TeamSpec & s);

// The team lead's member id, or "".
std::string team_lead_of(const TeamSpec & s);
// Copies of one member: {"id"} when count == 1, else {"id.1", ..., "id.N"}.
std::vector<std::string> team_copies(const TeamMemberSpec & m);
// All copies of all members, in spec order.
std::vector<std::string> team_all_copies(const TeamSpec & s);
// Member id of a copy ("reader.2" -> "reader", "editor" -> "editor"); "" if no member matches.
std::string team_member_of(const TeamSpec & s, const std::string & copy);
const TeamMemberSpec * team_member(const TeamSpec & s, const std::string & member_id);

// May `from` reach `to` (both copies, or "lead" for the session lead) with an edge of this kind?
// Defaults: lead -> team lead (or -> every member when there is no team lead); team lead -> its members;
// every member -> its parent (message only); lead and team lead may observe anyone below them. Extra spec edges
// add links between members; an edge x -> x links distinct copies of x. Nobody messages itself.
bool edge_allows(const TeamSpec & s, const std::string & from, const std::string & to, const std::string & kind);
// Who a copy reports to: the team lead's copy, or "lead".
std::string team_parent_of(const TeamSpec & s, const std::string & copy);

// The text of one rule_files entry: the whole file, or the "#Heading" section (the heading line through the line
// before the next heading of the same or a higher level). Capped at max_chars (a note says it was cut).
// The caller reads the file (with the normal read permission check) and passes its text. "" if the heading is
// not found.
std::string team_rule_text(const std::string & ref, const std::string & file_text, size_t max_chars = 6000);
// "SPEC.md#House rules" -> "SPEC.md".
std::string team_rule_path(const std::string & ref);

// The block pinned into every member's prompt (docs/SWARM-TEAMS.md "Pinning"). `rules_text` is the resolved
// house rules (rules, rule_files, config house_rules) already joined; `request` is the user's request verbatim.
struct TeamBlockInput {
    const TeamSpec * spec = nullptr;
    std::string copy;                  // this member's copy id
    std::string rules_text;
    std::string request;
    size_t request_max = 2000;
    bool cpu_fallback = false;         // a cpu member running on the main model because the swarm is off
};
std::string team_block(const TeamBlockInput & in);
// Marker that starts the pinned block, so it can be found and refreshed in the message list.
inline const char * TEAM_BLOCK_PREFIX = "[team ";

// Live status for the UI: activity (what the agent loop says) refined by the engine's view of the agent's tag.
// activity: queued | generating | tool | waiting | blocked | done | failed | idle (agent side)
// engine_phase: "" (not seen) | "queue" | "prefill" | "generate" | "idle"
// Result is one of queued | prefilling | generating | tool | waiting | blocked | done | failed | idle.
std::string team_status_merge(const std::string & activity, const std::string & engine_phase);

}  // namespace judas
