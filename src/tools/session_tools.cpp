// The session_search tool: one hit per line, newest first, with the session title and an FTS snippet.
#include "tools/session_tools.h"

#include <algorithm>
#include <ctime>

namespace judas {
namespace {

// One hit must stay on one line: FTS snippets can carry the message's newlines and tabs.
std::string one_line(const std::string & s) {
    std::string out = s;
    for (char & c : out)
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
    return out;
}

std::string stamp(long long ms) {
    std::time_t t = (std::time_t) (ms / 1000);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &tm);
    return buf;
}

ToolResult t_session_search(const json & a, Store & store) {
    std::string query = a.value("query", "");
    if (query.find_first_not_of(" \t\r\n") == std::string::npos)
        return {false, "session_search: query is empty"};
    int limit = 10;
    if (a.contains("limit") && a["limit"].is_number())
        limit = (int) std::clamp(a["limit"].get<double>(), 1.0, 50.0);
    std::string session = a.contains("session") && a["session"].is_string() ? a["session"].get<std::string>() : "";

    auto hits = store.search_messages(query, limit, session);
    if (hits.empty()) return {true, "no matching messages"};
    std::string out;
    for (const auto & h : hits) {
        if (!out.empty()) out += '\n';
        out += "[" + h.session + "] " + one_line(h.title) + " | " + h.role + " | " + stamp(h.created_ms) +
               " | #" + std::to_string(h.message_id) + " | " + one_line(h.snippet);
    }
    return {true, out};
}

}  // namespace

void register_session_tools(ToolRegistry & reg, Store & store) {
    Tool t;
    t.name = "session_search";
    t.description = "Search past conversations for a word or phrase. Returns matching messages newest first, "
                    "each with its session id and title, role, timestamp, message id and a short excerpt. "
                    "Optional session limits the search to one conversation.";
    t.parameters = {{"type", "object"},
                    {"properties",
                     {{"query", {{"type", "string"}, {"description", "Words or phrase to find in past messages"}}},
                      {"limit",
                       {{"type", "integer"},
                        {"description", "Maximum hits, default 10"},
                        {"minimum", 1},
                        {"maximum", 50}}},
                      {"session",
                       {{"type", "string"},
                        {"description", "Session id to search one conversation; default is all sessions"}}}}},
                    {"required", {"query"}}};
    t.run = [&store](const json & a, ToolContext &) { return t_session_search(a, store); };
    reg.add(std::move(t));
}

}  // namespace judas
