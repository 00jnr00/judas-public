// HTTP server (cpp-httplib). One port (server.port, default 8079) serves:
//   OpenAI-compatible:  GET /v1/models, POST /v1/chat/completions (stream and non-stream, tools)
//   Health/state:       GET /health, GET /state (EngineState + sessions summary as JSON)
//   Judas API:          GET  /api/sessions, POST /api/sessions {title,cwd,mode}, PATCH /api/sessions/<id> {mode,title}
//                       GET  /api/sessions/<id>?limit&before (session + agents + a page of messages + turn marker)
//                       POST /api/sessions/<id>/messages {content, cwd}  -> starts a turn, returns {session}
//                       GET  /api/sessions/<id>/events  (SSE with ids, replay via ?since / Last-Event-ID; ?follow=1 stays open)
//                       GET  /api/events  (global SSE feed: session and turn changes)
//                       POST /api/sessions/<id>/cancel
//                       GET  /api/snapshots?session=, GET /api/snapshots/<id>/diff, POST /api/snapshots/<id>/revert
#pragma once

#include <memory>
#include <string>

#include "agent/agent.h"
#include "core/config.h"
#include "engine/engine.h"
#include "snap/journal.h"
#include "store/store.h"

namespace judas {

class Server {
public:
    Server(const Config & cfg, Engine & engine, AgentManager & agents, Store & store, Journal & journal);
    ~Server();
    // Blocks until stop(). Returns false if the primary bind cannot be bound; extra binds are retried in the background.
    bool run();
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace judas
