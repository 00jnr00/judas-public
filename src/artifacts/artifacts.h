// Artifacts: persistent pages, documents, diagrams and 3D views the model makes for the user. Every change is a
// new immutable version directory; nothing is overwritten and versions are removed only when the whole artifact
// is deleted. Layout and security model: docs/ARTIFACTS.md.
//
//   <data_dir>/artifacts/<id>/meta.json
//   <data_dir>/artifacts/<id>/v<N>/<files...>      immutable once written
//   <data_dir>/artifacts/<id>/renders/v<N>.png     derived preview of version N (replaced when re-rendered)
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"

namespace judas {

struct ArtifactFile {
    std::string path;
    long long bytes = 0;
};

struct ArtifactVersion {
    int n = 0;
    long long created_ms = 0;
    std::string note, session, entry;   // entry: the file served for /a/<id>/v<N>/
    std::vector<ArtifactFile> files;
};

struct ArtifactMeta {
    std::string id, title, kind, session;
    long long created_ms = 0, updated_ms = 0;
    std::vector<ArtifactVersion> versions;   // ascending n
    const ArtifactVersion * version(int n) const;   // n <= 0 = latest
    const ArtifactVersion * latest() const { return versions.empty() ? nullptr : &versions.back(); }
};

struct ArtifactLimits {
    static constexpr size_t MAX_FILES = 64;
    static constexpr size_t MAX_TEXT_FILE = 4u << 20;       // one text file given inline by the model
    static constexpr size_t MAX_BINARY_FILE = 64u << 20;    // a model or image copied from disk
    static constexpr size_t MAX_VERSION_BYTES = 128u << 20;
    static constexpr size_t MAX_VERSIONS = 500;
    static constexpr size_t MAX_ARTIFACTS = 2000;
    static constexpr size_t MAX_TITLE = 120;
};

class ArtifactStore {
public:
    // root = the artifacts directory itself (created on first write)
    explicit ArtifactStore(std::string root) : root_(std::move(root)) {}
    explicit ArtifactStore(const Config & cfg);

    static bool valid_id(const std::string & id);
    static bool valid_kind(const std::string & kind);
    // Safe relative path inside a version ("" when it is absolute, has .., a hidden segment, odd characters, ...).
    static std::string clean_path(const std::string & p);
    static std::string mime_for(const std::string & path);

    // files: path -> bytes. html needs index.html (or exactly one .html file); svg needs one .svg; markdown needs
    // one .md; model3d needs one .stl or .glb; image needs one png/jpg/gif/webp.
    // The viewer entry for markdown and model3d is generated (index.html). Returns nullopt and sets err on failure.
    std::optional<ArtifactMeta> create(const std::string & title, const std::string & kind,
                                       const std::map<std::string, std::string> & files, const std::string & session,
                                       const std::string & note, std::string & err);
    // New version = every file of the previous one, then `files` added or replaced and `remove` dropped.
    std::optional<ArtifactMeta> update(const std::string & id, const std::map<std::string, std::string> & files,
                                       const std::vector<std::string> & remove, const std::string & note,
                                       const std::string & session, std::string & err, const std::string & new_title = "");

    std::optional<ArtifactMeta> get(const std::string & id) const;
    std::vector<ArtifactMeta> list(const std::string & query = "") const;   // newest updated first
    bool remove(const std::string & id);   // the whole artifact, every version

    // Bytes of a file in version n (<= 0 latest); path "" = the entry. Returns false with err on a bad path or miss.
    bool read_file(const std::string & id, int n, const std::string & path, std::string & bytes, std::string & used_path,
                   std::string & err) const;

    std::string version_dir(const std::string & id, int n) const;
    std::string render_file(const std::string & id, int n) const;
    bool save_render(const std::string & id, int n, const std::string & png);
    std::string load_render(const std::string & id, int n) const;   // "" when none
    const std::string & root() const { return root_; }

    static json meta_json(const ArtifactMeta & m);

private:
    std::string root_;
    std::optional<ArtifactMeta> read_meta(const std::string & id) const;
    bool write_meta(const ArtifactMeta & m, std::string & err) const;
    bool write_version(ArtifactMeta & m, const std::string & prev_dir, const std::map<std::string, std::string> & files,
                       const std::vector<std::string> & remove, const std::string & note, const std::string & session,
                       std::string & err);
};

// "<id>" 10 lowercase hex characters
std::string new_artifact_id();

}  // namespace judas
