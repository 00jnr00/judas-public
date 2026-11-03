// RemoteEngine: the Engine interface backed by another Judas instance's OpenAI-compatible endpoint.
// Lets team members run their agent loop on the CPU swarm instance instead of the in-process GPU engine.
#pragma once

#include <memory>
#include <string>

#include "engine/engine.h"

namespace judas {

class RemoteEngine : public Engine {
public:
    // base_url like "http://127.0.0.1:8083" (trailing slashes ok). model is sent as "model" in the request body.
    explicit RemoteEngine(std::string base_url, std::string model = "swarm", int read_timeout_s = 900);
    ~RemoteEngine() override;

    // POST {base}/v1/chat/completions with stream=true and stream_options.include_usage=true. Streams
    // delta.content -> ChatDelta.content and delta.reasoning_content -> ChatDelta.reasoning via on_delta; if on_delta
    // returns false or *cancel becomes true, stop reading and return finish_reason "cancelled" with what arrived.
    // Cancellation is checked when a chunk arrives, so a request stuck in a long prefill is only dropped once the
    // remote sends its next byte (or the read timeout hits).
    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> & on_delta = {},
                    const std::atomic<bool> * cancel = nullptr) override;
    // GET {base}/api/state with a 2 s timeout. Unreachable -> loaded=false, model="".
    EngineState state() const override;
    std::string model_name() const override;   // the remote's reported model once known, else the name passed in
    int count_tokens(const std::string & text) const override;   // estimate: (bytes + 3) / 4
    // true if GET {base}/health answers 200 within 2 s
    bool reachable() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace judas
