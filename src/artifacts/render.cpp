#include "artifacts/render.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "artifacts/raster.h"

namespace judas {

RenderBroker & RenderBroker::instance() {
    static RenderBroker b;
    return b;
}

void RenderBroker::set_notifier(std::function<void(const json &)> fn) {
    std::lock_guard lk(mu_);
    notify_ = std::move(fn);
}

bool RenderBroker::request(const std::string & id, int ver, int w, int h, int timeout_ms, std::string & png, std::string & err) {
    std::function<void(const json &)> fn;
    {
        std::lock_guard lk(mu_);
        fn = notify_;
        if (!fn) { err = "no web page is connected to capture this artifact"; return false; }
        pending_.push_back({id, ver, false, ""});
    }
    long long deadline = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count() + timeout_ms;
    fn({{"id", id}, {"version", ver}, {"width", w}, {"height", h}, {"deadline_ms", deadline}});
    std::unique_lock lk(mu_);
    auto me = std::prev(pending_.end());
    bool got = cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return me->done; });
    if (got) png = std::move(me->png);
    pending_.erase(me);
    if (!got) { err = "no open Judas web page captured it in time"; return false; }
    return true;
}

bool RenderBroker::submit(const std::string & id, int ver, const std::string & png) {
    std::lock_guard lk(mu_);
    bool any = false;
    for (auto & p : pending_)
        if (!p.done && p.id == id && p.ver == ver) { p.done = true; p.png = png; any = true; break; }
    if (any) cv_.notify_all();
    return any;
}

namespace {

std::string ext_of(const std::string & p) {
    size_t d = p.rfind('.');
    if (d == std::string::npos) return "";
    std::string e = p.substr(d + 1);
    for (auto & c : e) c = (char) std::tolower((unsigned char) c);
    return e;
}

}  // namespace

RenderResult render_artifact(ArtifactStore & store, const std::string & id, int ver, int w, int h,
                             const std::vector<std::string> & views, int client_timeout_ms) {
    RenderResult r;
    auto m = store.get(id);
    if (!m) { r.error = "no artifact with that id"; return r; }
    const ArtifactVersion * v = m->version(ver);
    if (!v) { r.error = "no such version"; return r; }
    r.version = v->n;
    w = std::clamp(w, 64, 2048);
    h = std::clamp(h, 64, 2048);
    std::string bytes, used, err;
    if (m->kind == "model3d") {
        std::string model;
        for (auto & f : v->files) {
            std::string e = ext_of(f.path);
            if ((e == "stl" || e == "glb") && (model.empty() || f.path.rfind("model.", 0) == 0)) model = f.path;
        }
        if (model.empty() || !store.read_file(id, v->n, model, bytes, used, err)) { r.error = "model file missing"; return r; }
        Mesh mesh;
        bool ok = ext_of(model) == "glb" ? load_glb(bytes, mesh, err) : load_stl(bytes, mesh, err);
        if (!ok) { r.error = "cannot read the model: " + err; return r; }
        Image img = render_mesh(mesh, w, h, views.empty() ? default_views() : views);
        r.png = encode_png(img);
        r.how = "server";
    } else if (m->kind == "svg") {
        if (!store.read_file(id, v->n, "", bytes, used, err)) { r.error = err; return r; }
        Image img;
        if (!render_svg(bytes, 0, 0, img, err)) { r.error = err; return r; }
        r.png = encode_png(img);
        r.how = "server";
    } else if (m->kind == "image") {
        if (!store.read_file(id, v->n, "", bytes, used, err)) { r.error = err; return r; }
        if (bytes.size() >= 8 && std::memcmp(bytes.data(), "\x89PNG", 4) == 0) { r.png = bytes; r.how = "server"; }
        else { r.error = "only PNG images can be returned as a preview; open the artifact to view it"; return r; }
    } else {
        if (!RenderBroker::instance().request(id, v->n, w, h, client_timeout_ms, r.png, err)) { r.error = err; return r; }
        r.how = "browser";
    }
    if (r.png.empty()) { r.error = "rendering produced no image"; return r; }
    store.save_render(id, v->n, r.png);
    r.ok = true;
    return r;
}

}  // namespace judas
