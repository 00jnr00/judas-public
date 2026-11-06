// Artifact storage: immutable numbered versions on disk plus a meta.json index per artifact.
#include "artifacts/artifacts.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <random>
#include <set>
#include <sstream>

namespace judas {
namespace fs = std::filesystem;
namespace {

std::mutex g_mu;   // serializes writers; readers rely on atomic renames

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string lower(std::string s) {
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

std::string ext_of(const std::string & p) {
    size_t d = p.rfind('.');
    size_t s = p.rfind('/');
    if (d == std::string::npos || (s != std::string::npos && d < s)) return "";
    return lower(p.substr(d + 1));
}

bool write_file(const fs::path & p, const std::string & bytes) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream o(p, std::ios::binary | std::ios::trunc);
    if (!o) return false;
    o.write(bytes.data(), (std::streamsize) bytes.size());
    return (bool) o;
}

bool write_atomic(const fs::path & p, const std::string & bytes) {
    fs::path tmp = p;
    tmp += ".tmp";
    if (!write_file(tmp, bytes)) return false;
    std::error_code ec;
    fs::rename(tmp, p, ec);
    return !ec;
}

std::string read_all(const fs::path & p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return "";
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string html_esc(const std::string & s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;"; break;
            case '<': o += "&lt;"; break;
            case '>': o += "&gt;"; break;
            case '"': o += "&quot;"; break;
            default: o += c;
        }
    }
    return o;
}

bool is_generated(const std::string & kind) { return kind == "markdown" || kind == "model3d"; }

bool is_model_ext(const std::string & e) { return e == "stl" || e == "glb"; }
bool is_image_ext(const std::string & e) { return e == "png" || e == "jpg" || e == "jpeg" || e == "gif" || e == "webp"; }

// The entry file of a file set, "" with err when the set does not fit the kind. `doc` is the markdown or model file.
std::string pick_entry(const std::string & kind, const std::set<std::string> & names, std::string & doc, std::string & err) {
    auto first_with = [&](auto pred, const char * prefer) -> std::string {
        if (prefer && names.count(prefer)) return prefer;
        for (auto & n : names) if (pred(ext_of(n))) return n;
        return "";
    };
    doc.clear();
    if (kind == "html") {
        if (names.count("index.html")) return "index.html";
        std::string h = first_with([](const std::string & e) { return e == "html" || e == "htm"; }, nullptr);
        size_t count = 0;
        for (auto & n : names) if (ext_of(n) == "html" || ext_of(n) == "htm") count++;
        if (h.empty() || count != 1) { err = "an html artifact needs index.html (or exactly one .html file)"; return ""; }
        return h;
    }
    if (kind == "svg") {
        std::string s = first_with([](const std::string & e) { return e == "svg"; }, "image.svg");
        if (s.empty()) err = "an svg artifact needs a .svg file";
        return s;
    }
    if (kind == "image") {
        std::string s = first_with(is_image_ext, nullptr);
        if (s.empty()) err = "an image artifact needs a png, jpg, gif or webp file";
        return s;
    }
    if (kind == "markdown") {
        doc = first_with([](const std::string & e) { return e == "md" || e == "markdown"; }, "document.md");
        if (doc.empty()) { err = "a markdown artifact needs a .md file"; return ""; }
        return "index.html";
    }
    if (kind == "model3d") {
        doc = first_with(is_model_ext, "model.stl");
        if (doc.empty()) { err = "a model3d artifact needs a .stl or .glb file"; return ""; }
        return "index.html";
    }
    err = "unknown kind";
    return "";
}

std::string generated_entry(const std::string & kind, const std::string & title, const std::string & doc) {
    std::string h = "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\">"
                    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
                    "<title>" + html_esc(title) + "</title></head>\n";
    if (kind == "markdown")
        h += "<body data-doc=\"" + html_esc(doc) + "\"><script src=\"/vendor/artifacts/mdview.js\"></script></body></html>\n";
    else
        h += "<body data-model=\"" + html_esc(doc) + "\">"
             "<script type=\"importmap\">{\"imports\":{\"three\":\"/vendor/artifacts/three/three.module.min.js\","
             "\"three/addons/\":\"/vendor/artifacts/three/\"}}</script>"
             "<script type=\"module\" src=\"/vendor/artifacts/viewer3d.js\"></script></body></html>\n";
    return h;
}

json version_json(const ArtifactVersion & v) {
    json files = json::array();
    for (auto & f : v.files) files.push_back({{"path", f.path}, {"bytes", f.bytes}});
    return {{"n", v.n}, {"created_ms", v.created_ms}, {"note", v.note}, {"session", v.session}, {"entry", v.entry}, {"files", files}};
}

}  // namespace

const ArtifactVersion * ArtifactMeta::version(int n) const {
    if (versions.empty()) return nullptr;
    if (n <= 0) return &versions.back();
    for (auto & v : versions) if (v.n == n) return &v;
    return nullptr;
}

std::string new_artifact_id() {
    static const char * hex = "0123456789abcdef";
    std::random_device rd;
    std::string id;
    for (int i = 0; i < 10; i++) id += hex[rd() & 15];
    return id;
}

ArtifactStore::ArtifactStore(const Config & cfg) : root_((fs::path(expand_home(cfg.data_dir)) / "artifacts").string()) {}

bool ArtifactStore::valid_id(const std::string & id) {
    if (id.size() < 4 || id.size() > 20) return false;
    for (char c : id) if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
    return true;
}

bool ArtifactStore::valid_kind(const std::string & k) {
    return k == "html" || k == "svg" || k == "markdown" || k == "model3d" || k == "image";
}

std::string ArtifactStore::clean_path(const std::string & p) {
    if (p.empty() || p.size() > 200 || p[0] == '/') return "";
    {   // a first segment like v3 would collide with the /a/<id>/v3/ version URLs
        size_t sl = p.find('/');
        std::string first = p.substr(0, sl);
        if (first.size() > 1 && first[0] == 'v' && first.find_first_not_of("0123456789", 1) == std::string::npos) return "";
    }
    std::string seg;
    auto bad_seg = [](const std::string & s) {
        return s.empty() || s == "." || s == ".." || s[0] == '.' || s.size() > 120;
    };
    for (char c : p) {
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '/';
        if (!ok) return "";
        if (c == '/') {
            if (bad_seg(seg)) return "";
            seg.clear();
        } else {
            seg += c;
        }
    }
    if (bad_seg(seg)) return "";
    return p;
}

std::string ArtifactStore::mime_for(const std::string & path) {
    static const std::map<std::string, std::string> m = {
        {"html", "text/html; charset=utf-8"}, {"htm", "text/html; charset=utf-8"}, {"css", "text/css; charset=utf-8"},
        {"js", "text/javascript; charset=utf-8"}, {"mjs", "text/javascript; charset=utf-8"},
        {"json", "application/json; charset=utf-8"}, {"svg", "image/svg+xml"}, {"png", "image/png"},
        {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"}, {"webp", "image/webp"},
        {"txt", "text/plain; charset=utf-8"}, {"md", "text/markdown; charset=utf-8"},
        {"markdown", "text/markdown; charset=utf-8"}, {"csv", "text/csv; charset=utf-8"},
        {"stl", "model/stl"}, {"glb", "model/gltf-binary"}, {"gltf", "model/gltf+json"}, {"bin", "application/octet-stream"},
        {"woff2", "font/woff2"}, {"woff", "font/woff"}, {"ttf", "font/ttf"}, {"ico", "image/x-icon"},
        {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"ogg", "audio/ogg"}, {"mp4", "video/mp4"}, {"webm", "video/webm"}};
    auto it = m.find(ext_of(path));
    return it == m.end() ? "application/octet-stream" : it->second;
}

json ArtifactStore::meta_json(const ArtifactMeta & m) {
    json vs = json::array();
    for (auto & v : m.versions) vs.push_back(version_json(v));
    int latest = m.versions.empty() ? 0 : m.versions.back().n;
    return {{"id", m.id}, {"title", m.title}, {"kind", m.kind}, {"session", m.session}, {"created_ms", m.created_ms},
            {"updated_ms", m.updated_ms}, {"latest", latest}, {"versions", vs}};
}

std::optional<ArtifactMeta> ArtifactStore::read_meta(const std::string & id) const {
    if (!valid_id(id)) return std::nullopt;
    std::string txt = read_all(fs::path(root_) / id / "meta.json");
    if (txt.empty()) return std::nullopt;
    json j = json::parse(txt, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    ArtifactMeta m;
    m.id = j.value("id", id);
    m.title = j.value("title", "");
    m.kind = j.value("kind", "");
    m.session = j.value("session", "");
    m.created_ms = j.value("created_ms", 0LL);
    m.updated_ms = j.value("updated_ms", 0LL);
    if (j.contains("versions") && j["versions"].is_array())
        for (auto & vj : j["versions"]) {
            ArtifactVersion v;
            v.n = vj.value("n", 0);
            v.created_ms = vj.value("created_ms", 0LL);
            v.note = vj.value("note", "");
            v.session = vj.value("session", "");
            v.entry = vj.value("entry", "");
            if (vj.contains("files") && vj["files"].is_array())
                for (auto & fj : vj["files"]) v.files.push_back({fj.value("path", ""), fj.value("bytes", 0LL)});
            if (v.n > 0) m.versions.push_back(std::move(v));
        }
    if (m.id != id) return std::nullopt;
    return m;
}

bool ArtifactStore::write_meta(const ArtifactMeta & m, std::string & err) const {
    if (!write_atomic(fs::path(root_) / m.id / "meta.json", meta_json(m).dump(1))) {
        err = "cannot write artifact metadata";
        return false;
    }
    return true;
}

std::optional<ArtifactMeta> ArtifactStore::get(const std::string & id) const { return read_meta(id); }

std::vector<ArtifactMeta> ArtifactStore::list(const std::string & query) const {
    std::vector<ArtifactMeta> out;
    std::error_code ec;
    std::string q = lower(query);
    for (auto & e : fs::directory_iterator(root_, ec)) {
        if (!e.is_directory(ec)) continue;
        auto m = read_meta(e.path().filename().string());
        if (!m) continue;
        if (!q.empty() && lower(m->title).find(q) == std::string::npos && lower(m->kind).find(q) == std::string::npos &&
            m->id.find(q) == std::string::npos)
            continue;
        out.push_back(std::move(*m));
    }
    std::sort(out.begin(), out.end(), [](const ArtifactMeta & a, const ArtifactMeta & b) { return a.updated_ms > b.updated_ms; });
    return out;
}

bool ArtifactStore::remove(const std::string & id) {
    if (!valid_id(id)) return false;
    std::lock_guard lk(g_mu);
    std::error_code ec;
    fs::path d = fs::path(root_) / id;
    if (!fs::exists(d / "meta.json", ec)) return false;
    fs::remove_all(d, ec);
    return !ec;
}

std::string ArtifactStore::version_dir(const std::string & id, int n) const {
    return (fs::path(root_) / id / ("v" + std::to_string(n))).string();
}

std::string ArtifactStore::render_file(const std::string & id, int n) const {
    return (fs::path(root_) / id / "renders" / ("v" + std::to_string(n) + ".png")).string();
}

bool ArtifactStore::save_render(const std::string & id, int n, const std::string & png) {
    if (!valid_id(id) || n <= 0 || png.empty()) return false;
    auto m = read_meta(id);
    if (!m || !m->version(n)) return false;
    return write_atomic(render_file(id, n), png);
}

std::string ArtifactStore::load_render(const std::string & id, int n) const {
    if (!valid_id(id) || n <= 0) return "";
    return read_all(render_file(id, n));
}

bool ArtifactStore::write_version(ArtifactMeta & m, const std::string & prev_dir, const std::map<std::string, std::string> & files,
                                  const std::vector<std::string> & remove, const std::string & note, const std::string & session,
                                  std::string & err) {
    if (m.versions.size() >= ArtifactLimits::MAX_VERSIONS) { err = "this artifact has reached the version limit"; return false; }
    int n = m.versions.empty() ? 1 : m.versions.back().n + 1;
    // 1. the file set: previous files (minus removals and the generated entry), then the new ones
    std::map<std::string, long long> set;   // path -> bytes
    std::map<std::string, std::string> carried;   // path -> source path on disk
    if (!m.versions.empty()) {
        std::set<std::string> rm(remove.begin(), remove.end());
        for (auto & f : m.versions.back().files) {
            if (rm.count(f.path)) continue;
            if (is_generated(m.kind) && f.path == "index.html") continue;
            if (files.count(f.path)) continue;
            set[f.path] = f.bytes;
            carried[f.path] = (fs::path(prev_dir) / f.path).string();
        }
    }
    size_t total = 0;
    for (auto & [p, b] : files) {
        if (is_generated(m.kind) && p == "index.html") { err = "index.html is generated for kind " + m.kind + "; do not send it"; return false; }
        if (b.size() > ArtifactLimits::MAX_BINARY_FILE) { err = "file too large: " + p; return false; }
        set[p] = (long long) b.size();
    }
    for (auto & [p, b] : set) total += (size_t) b;
    if (set.size() > ArtifactLimits::MAX_FILES) { err = "too many files (limit " + std::to_string(ArtifactLimits::MAX_FILES) + ")"; return false; }
    if (total > ArtifactLimits::MAX_VERSION_BYTES) { err = "version is larger than the size limit"; return false; }
    std::set<std::string> names;
    for (auto & [p, b] : set) names.insert(p);
    std::string doc;
    std::string entry = pick_entry(m.kind, names, doc, err);
    if (entry.empty()) return false;
    std::string gen;
    if (is_generated(m.kind)) {
        gen = generated_entry(m.kind, m.title, doc);
        set["index.html"] = (long long) gen.size();
    }
    // 2. write into a temp dir, then rename it into place
    fs::path root = fs::path(root_) / m.id;
    fs::path tmp = root / (".tmp-v" + std::to_string(n));
    fs::path fin = root / ("v" + std::to_string(n));
    std::error_code ec;
    fs::remove_all(tmp, ec);
    if (fs::exists(fin, ec)) { err = "version already exists"; return false; }
    fs::create_directories(tmp, ec);
    if (ec) { err = "cannot create the version directory"; return false; }
    auto fail = [&](const std::string & why) { fs::remove_all(tmp, ec); err = why; return false; };
    for (auto & [p, src] : carried) {
        fs::path dst = tmp / p;
        fs::create_directories(dst.parent_path(), ec);
        fs::create_hard_link(src, dst, ec);   // files are never modified in place, so sharing the inode is safe
        if (ec) {
            ec.clear();
            fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
            if (ec) return fail("cannot carry forward " + p);
        }
    }
    for (auto & [p, b] : files)
        if (!write_file(tmp / p, b)) return fail("cannot write " + p);
    if (!gen.empty() && !write_file(tmp / "index.html", gen)) return fail("cannot write the viewer page");
    fs::rename(tmp, fin, ec);
    if (ec) return fail("cannot finish the version");
    ArtifactVersion v;
    v.n = n;
    v.created_ms = now_ms();
    v.note = note.substr(0, 300);
    v.session = session;
    v.entry = entry;
    for (auto & [p, b] : set) v.files.push_back({p, b});
    m.versions.push_back(std::move(v));
    m.updated_ms = m.versions.back().created_ms;
    return true;
}

std::optional<ArtifactMeta> ArtifactStore::create(const std::string & title, const std::string & kind,
                                                  const std::map<std::string, std::string> & files, const std::string & session,
                                                  const std::string & note, std::string & err) {
    if (!valid_kind(kind)) { err = "kind must be html, svg, markdown, model3d or image"; return std::nullopt; }
    std::string t = title;
    while (!t.empty() && (t.back() == ' ' || t.back() == '\n')) t.pop_back();
    if (t.empty()) { err = "title is empty"; return std::nullopt; }
    if (t.size() > ArtifactLimits::MAX_TITLE) t.resize(ArtifactLimits::MAX_TITLE);
    for (auto & [p, b] : files)
        if (clean_path(p).empty()) { err = "bad file path: '" + p + "' (relative, letters digits . _ - /, no .. and no hidden names)"; return std::nullopt; }
    std::lock_guard lk(g_mu);
    std::error_code ec;
    size_t count = 0;
    for (auto & e : fs::directory_iterator(root_, ec)) { (void) e; count++; }
    if (count >= ArtifactLimits::MAX_ARTIFACTS) { err = "too many artifacts; delete some first"; return std::nullopt; }
    ArtifactMeta m;
    do { m.id = new_artifact_id(); } while (fs::exists(fs::path(root_) / m.id, ec));
    m.title = t;
    m.kind = kind;
    m.session = session;
    m.created_ms = now_ms();
    fs::create_directories(fs::path(root_) / m.id, ec);
    if (ec) { err = "cannot create the artifacts directory"; return std::nullopt; }
    if (!write_version(m, "", files, {}, note.empty() ? "first version" : note, session, err) || !write_meta(m, err)) {
        fs::remove_all(fs::path(root_) / m.id, ec);
        return std::nullopt;
    }
    return m;
}

std::optional<ArtifactMeta> ArtifactStore::update(const std::string & id, const std::map<std::string, std::string> & files,
                                                  const std::vector<std::string> & remove, const std::string & note,
                                                  const std::string & session, std::string & err, const std::string & new_title) {
    for (auto & [p, b] : files)
        if (clean_path(p).empty()) { err = "bad file path: '" + p + "'"; return std::nullopt; }
    std::lock_guard lk(g_mu);
    auto m = read_meta(id);
    if (!m || m->versions.empty()) { err = "no artifact with that id"; return std::nullopt; }
    if (!new_title.empty()) m->title = new_title.substr(0, ArtifactLimits::MAX_TITLE);
    if (files.empty() && remove.empty() && new_title.empty()) { err = "nothing to change: send files, remove or title"; return std::nullopt; }
    if (!write_version(*m, version_dir(id, m->versions.back().n), files, remove, note.empty() ? "update" : note, session, err) ||
        !write_meta(*m, err))
        return std::nullopt;
    return m;
}

bool ArtifactStore::read_file(const std::string & id, int n, const std::string & path, std::string & bytes, std::string & used_path,
                              std::string & err) const {
    auto m = read_meta(id);
    if (!m) { err = "no artifact with that id"; return false; }
    const ArtifactVersion * v = m->version(n);
    if (!v) { err = "no such version"; return false; }
    used_path = path.empty() ? v->entry : clean_path(path);
    if (used_path.empty()) { err = "bad path"; return false; }
    bool listed = false;
    for (auto & f : v->files) if (f.path == used_path) listed = true;   // only files the meta knows about
    if (!listed) { err = "no such file"; return false; }
    bytes = read_all(fs::path(version_dir(id, v->n)) / used_path);
    return true;
}

}  // namespace judas
