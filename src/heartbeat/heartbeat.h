// Heartbeat: read-only health check of the stack. Collectors gather facts; a model summarizes them and flags
// problems; the result is a report with a severity. Nothing here writes or runs non-read-only commands.
#pragma once
#include <functional>
#include <string>
#include <vector>
#include "core/config.h"
#include "core/json.h"
namespace judas {

struct HeartbeatReport {
    std::string summary;                 // the model's plain-language summary
    std::string severity = "ok";         // ok | notice | warn | critical (parsed from the model, else inferred)
    json facts;                          // raw collector output {name: {text, ms, error}}
    long long ran_ms = 0;
    bool pushed = false;                 // whether it was sent to a channel this run
};

// Runs collectors read-only and returns their facts as JSON (no model call). Safe to call anytime.
json collect_facts(const HeartbeatConfig & cfg);

// A model summarizer: given the facts text, return {summary, severity}. The caller supplies this so heartbeat
// does not depend on the engine directly (the daemon passes a local-engine or remote-HTTP summarizer).
using Summarizer = std::function<std::pair<std::string, std::string>(const std::string & facts_text)>;

// Full run: collect, summarize, decide severity. Does not push; the caller pushes via Channels when warranted.
HeartbeatReport run_heartbeat(const HeartbeatConfig & cfg, const Summarizer & summarize);

// Render facts JSON as the compact text block sent to the model.
std::string facts_to_text(const json & facts);

}
