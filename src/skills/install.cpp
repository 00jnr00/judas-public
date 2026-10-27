// Skills install: copy an external skill into the skills dir after a guard scan.
#include "skills/install.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <vector>

#include "skills/guard.h"

namespace judas {
namespace fs = std::filesystem;

namespace {

std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// [a-z0-9][a-z0-9-]{0,63}
bool valid_name(const std::string & n) {
    if (n.empty() || n.size() > 64) return false;
    auto alnum = [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'); };
    if (!alnum(n[0])) return false;
    for (char c : n)
        if (!alnum(c) && c != '-') return false;
    return true;
}

std::string unquote(std::string v) {
    v = trim(v);
    if (v.size() >= 2 && (v.front() == '"' || v.front() == '\'') && v.back() == v.front())
        v = v.substr(1, v.size() - 2);
    return v;
}

}  // namespace

InstallResult install_skill(const std::string & src_path, const std::string & skills_dir, bool force) {
    InstallResult r{false, false, "", ""};
    std::ifstream f(src_path);
    if (!f) {
        r.report = "cannot read " + src_path;
        return r;
    }
    std::string text((std::istreambuf_iterator<char>(f)), {});

    GuardReport rep = scan_skill_text(text);
    for (const auto & g : rep.findings)
        r.report += g.severity + " " + g.rule + " line " + std::to_string(g.line) + ": " + g.excerpt + "\n";
    const bool blocked = rep.blocked();

    // Split frontmatter into lines so unknown keys survive untouched; only `source:` is rewritten.
    const std::string abs_src = fs::absolute(src_path).string();
    const std::string source_line = "source: external:" + abs_src;
    std::vector<std::string> fm_lines;
    std::string body = text, name;
    if (text.rfind("---", 0) == 0) {
        size_t end = text.find("\n---", 3);
        if (end != std::string::npos) {
            std::stringstream fm(text.substr(3, end - 3));
            for (std::string line; std::getline(fm, line);) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (trim(line).empty()) continue;
                size_t c = line.find(':');
                std::string key = c == std::string::npos ? "" : trim(line.substr(0, c));
                if (key == "source") continue;   // replaced below
                if (key == "name") name = unquote(line.substr(c + 1));
                fm_lines.push_back(line);
            }
            size_t nl = text.find('\n', end + 1);
            body = nl == std::string::npos ? "" : text.substr(nl + 1);
        }
    }
    fm_lines.push_back(source_line);
    if (name.empty()) name = fs::path(src_path).stem().string();

    if (!valid_name(name)) {
        r.report += "refused: invalid skill name '" + name + "' (need [a-z0-9][a-z0-9-]{0,63})\n";
        return r;
    }
    // A skill with this name is already installed (or archived-in-place): never overwrite, even with --force.
    const fs::path live = fs::path(skills_dir) / (name + ".md");
    std::error_code ec;
    if (fs::exists(live, ec)) {
        r.report += "refused: a skill named '" + name + "' is already installed at " + live.string() + "\n";
        return r;
    }

    const bool quarantine = blocked && !force;
    fs::path dest = quarantine ? fs::path(skills_dir) / ".quarantine" / (name + ".md") : live;
    fs::create_directories(dest.parent_path(), ec);
    std::ofstream out(dest, std::ios::trunc);
    if (!out) {
        r.report += "refused: cannot write " + dest.string() + "\n";
        return r;
    }
    out << "---\n";
    for (const auto & l : fm_lines) out << l << "\n";
    out << "---\n" << body;
    if (!body.empty() && body.back() != '\n') out << "\n";
    out.close();

    r.installed = !quarantine;
    r.quarantined = quarantine;
    r.dest = dest.string();
    return r;
}

}  // namespace judas
