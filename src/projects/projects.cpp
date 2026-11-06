#include "projects/projects.h"

#include <cctype>
#include <filesystem>
#include <system_error>

namespace judas::projects {

namespace fs = std::filesystem;

static const size_t kMaxInstructions = 8000;

std::string slugify(const std::string & name) {
    std::string out;
    bool dash = false;
    for (unsigned char c : name) {
        if (std::isalnum(c) && c < 128) {
            if (dash && !out.empty()) out += '-';
            dash = false;
            out += (char) std::tolower(c);
        } else {
            dash = true;
        }
    }
    if (out.size() > 48) out.resize(48);
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "project" : out;
}

std::string unique_folder(const std::string & root, const std::string & name) {
    fs::path base = fs::path(expand_home(root)) / slugify(name);
    fs::path p = base;
    std::error_code ec;
    for (int i = 2; fs::exists(p, ec) && i < 10000; i++) p = base.string() + "-" + std::to_string(i);
    return p.string();
}

static std::string trim(std::string s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

Result create(Store & store, const Config & cfg, const std::string & name_in, const std::string & mode,
              const std::string & folder_in, const std::string & instructions) {
    Result r;
    std::string name = trim(name_in);
    if (name.empty()) { r.error = "a project needs a name"; return r; }
    if (name.size() > 120) name.resize(120);
    std::string ins = instructions.size() > kMaxInstructions ? instructions.substr(0, kMaxInstructions) : instructions;
    std::error_code ec;
    if (mode == "make") {
        std::string dir = unique_folder(cfg.projects.root, name);
        fs::create_directories(dir, ec);
        if (ec) { r.error = "cannot create " + dir + ": " + ec.message(); return r; }
        r.project = store.create_project(name, dir, ins, "made");
    } else if (mode == "existing") {
        std::string f = expand_home(trim(folder_in));
        if (f.empty() || !fs::path(f).is_absolute()) { r.error = "the folder must be an absolute path"; return r; }
        fs::path c = fs::weakly_canonical(f, ec);
        if (ec || !fs::is_directory(c, ec)) { r.error = "no such folder: " + f; return r; }
        r.project = store.create_project(name, c.string(), ins, "existing");
    } else {
        r.error = "mode must be \"make\" (new folder) or \"existing\"";
    }
    return r;
}

std::string ensure_folder(const ProjectRow & p) {
    std::error_code ec;
    if (fs::is_directory(p.folder, ec)) return "";
    if (p.kind != "made") return "the project folder is missing: " + p.folder;
    fs::create_directories(p.folder, ec);
    if (ec) return "cannot create " + p.folder + ": " + ec.message();
    return "";
}

std::string folder_delete_refusal(const std::string & folder) {
    std::error_code ec;
    fs::path p = fs::weakly_canonical(folder, ec);
    if (ec || p.empty() || !p.is_absolute()) return "not an absolute path";
    size_t depth = 0;
    for (auto & part : p) if (part != "/" && !part.empty()) depth++;
    if (depth < 3) return "that folder is too close to the filesystem root to delete from here";
    fs::path home = fs::weakly_canonical(expand_home("~"), ec);
    if (!home.empty() && (p == home || p == home.parent_path())) return "that is the home folder";
    // a folder that contains home (or any ancestor of it) must never go
    std::string hs = home.string() + "/", ps = p.string() + "/";
    if (!home.empty() && hs.rfind(ps, 0) == 0) return "that folder contains the home folder";
    for (const char * sys : {"/usr", "/etc", "/var", "/bin", "/sbin", "/lib", "/boot", "/opt", "/srv", "/mnt", "/home", "/run", "/proc", "/sys", "/dev"})
        if (p == sys) return "that is a system folder";
    return "";
}

DeleteResult remove(Store & store, const std::string & id, bool delete_folder) {
    DeleteResult r;
    auto p = store.get_project(id);
    if (!p) { r.error = "no such project"; return r; }
    if (delete_folder) {
        if (std::string why = folder_delete_refusal(p->folder); !why.empty()) { r.error = why; return r; }
    }
    store.delete_project(id);
    r.ok = true;
    if (delete_folder) {
        std::error_code ec;
        fs::remove_all(p->folder, ec);
        r.folder_removed = !ec && !fs::exists(p->folder, ec);
        if (!r.folder_removed) r.error = "the project was removed but its folder could not be fully deleted";
    }
    return r;
}

std::optional<ProjectRow> of_session(const Store & store, const std::string & session_id) {
    auto s = store.get_session(session_id);
    if (!s || s->project_id.empty()) return std::nullopt;
    return store.get_project(s->project_id);
}

std::string prompt_block(const ProjectRow & p) {
    std::string b = "Project: " + p.name + "\nProject folder: " + p.folder +
                    "\nThis chat belongs to that project; its folder is your working directory.";
    if (!trim(p.instructions).empty()) b += "\n\nProject instructions (from the user, follow them):\n" + p.instructions;
    return b;
}

}  // namespace judas::projects
