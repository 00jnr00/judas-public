// Artifact previews. model3d, svg and image render inside judas (raster.h). html and markdown need a browser
// engine, so judas asks an open Judas web page (desktop or phone) to capture them and waits for the PNG.
#pragma once

#include <condition_variable>
#include <functional>
#include <list>
#include <mutex>
#include <string>
#include <vector>

#include "artifacts/artifacts.h"

namespace judas {

class RenderBroker {
public:
    static RenderBroker & instance();
    // The server sets this to publish a global "artifact.render" event; unset means no page can ever answer.
    void set_notifier(std::function<void(const json &)> fn);
    // Ask a web page to capture version `ver` of `id` at w x h. Blocks up to timeout_ms.
    bool request(const std::string & id, int ver, int w, int h, int timeout_ms, std::string & png, std::string & err);
    // A page delivers a capture. Returns true when a request was waiting for it.
    bool submit(const std::string & id, int ver, const std::string & png);

private:
    struct Pending { std::string id; int ver; bool done = false; std::string png; };
    std::mutex mu_;
    std::condition_variable cv_;
    std::list<Pending> pending_;
    std::function<void(const json &)> notify_;
};

struct RenderResult {
    bool ok = false;
    std::string png, error;
    std::string how;   // "server" (rendered inside judas) or "browser" (captured by an open web page)
    int version = 0;
};

// ver <= 0: latest. views only matters for model3d. client_timeout_ms is how long to wait for a web page.
RenderResult render_artifact(ArtifactStore & store, const std::string & id, int ver, int w, int h,
                             const std::vector<std::string> & views, int client_timeout_ms = 25000);

}  // namespace judas
