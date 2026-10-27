// Skills loader: Markdown files with `---` frontmatter, matched on trigger word boundaries.
#include "skills/skills.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace judas {
namespace fs = std::filesystem;

namespace {

// Debounce window: reload_if_changed refreshes at most once per this many seconds.
constexpr int k_min_refresh_s = 2;

std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string lower(std::string s) {
    for (auto & c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool parse_bool(const std::string & v) {
    std::string s = lower(trim(v));
    return s == "true" || s == "1" || s == "yes" || s == "on";
}

int parse_version(const std::string & v) {
    try {
        size_t used = 0;
        int n = std::stoi(trim(v), &used);
        return used > 0 && n > 0 ? n : 1;
    } catch (...) {
        return 1;
    }
}

// Archived and quarantined trees are parked directly under the skills dir but must never load.
bool parked_dir(const fs::path & p) {
    const std::string name = p.filename().string();
    return name == ".archive" || name == ".quarantine";
}

// "[a, b]" or "a, b": split on commas, drop brackets and quotes, lowercase, skip empties.
std::vector<std::string> split_triggers(std::string v) {
    v = trim(v);
    if (!v.empty() && v.front() == '[') v.erase(0, 1);
    if (!v.empty() && v.back() == ']') v.pop_back();
    std::vector<std::string> out;
    std::stringstream ss(v);
    for (std::string item; std::getline(ss, item, ',');) {
        item = trim(item);
        if (item.size() >= 2 && (item.front() == '"' || item.front() == '\'') && item.back() == item.front())
            item = trim(item.substr(1, item.size() - 2));
        item = lower(item);
        if (!item.empty()) out.push_back(item);
    }
    return out;
}

std::string first_line(const std::string & text) {
    std::stringstream ss(text);
    for (std::string line; std::getline(ss, line);)
        if (auto t = trim(line); !t.empty()) return t;
    return "";
}

bool parse_skill(const fs::path & p, Skill & s) {
    std::ifstream f(p);
    if (!f) return false;
    std::string text((std::istreambuf_iterator<char>(f)), {});
    s.name = p.stem().string();
    s.path = p.string();
    bool has_desc = false;
    if (text.rfind("---", 0) == 0) {
        size_t end = text.find("\n---", 3);
        if (end != std::string::npos) {
            std::stringstream fm(text.substr(3, end - 3));
            for (std::string line; std::getline(fm, line);) {
                size_t c = line.find(':');
                if (c == std::string::npos) continue;
                std::string k = trim(line.substr(0, c)), v = trim(line.substr(c + 1));
                if (k == "name" && !v.empty()) s.name = v;
                else if (k == "description") { s.description = v; has_desc = true; }
                else if (k == "triggers") s.triggers = split_triggers(v);
                else if (k == "pinned") s.pinned = parse_bool(v);
                else if (k == "source" && !v.empty()) s.source = v;
                else if (k == "version") s.version = parse_version(v);
            }
            size_t nl = text.find('\n', end + 1);
            text = nl == std::string::npos ? "" : text.substr(nl + 1);
        }
    }
    s.body = trim(text);
    if (!has_desc) s.description = first_line(s.body);
    return true;
}

// A trigger matches only when both ends sit on a non-alphanumeric boundary, so "log" does not hit "blog".
bool trigger_matches(const std::string & text, const std::string & trigger) {
    if (trigger.size() < 3) return false;
    for (size_t pos = text.find(trigger); pos != std::string::npos; pos = text.find(trigger, pos + 1)) {
        bool left = pos == 0 || !std::isalnum((unsigned char)text[pos - 1]);
        size_t end = pos + trigger.size();
        bool right = end == text.size() || !std::isalnum((unsigned char)text[end]);
        if (left && right) return true;
    }
    return false;
}

}  // namespace

Skills::Skills(const SkillsConfig & cfg) : cfg_(cfg) { reload(); cache_key_ = dir_key(); }

void Skills::reload() {
    std::vector<Skill> skills_;   // built unlocked, swapped in at the end
    if (!cfg_.enabled) { std::unique_lock lk(mu_); this->skills_.clear(); return; }
    for (const auto & d : cfg_.paths) {
        try {
            std::error_code ec;
            fs::path dir = expand_home(d);
            if (!fs::is_directory(dir, ec)) continue;
            std::vector<fs::path> files;
            for (const auto & e : fs::directory_iterator(dir, ec)) {
                std::error_code de;
                if (e.is_directory(de) && parked_dir(e.path())) continue;
                if (e.path().extension() != ".md") continue;
                files.push_back(e.path());
            }
            std::sort(files.begin(), files.end());   // deterministic order
            for (const auto & p : files) {
                Skill s;
                try {
                    if (!parse_skill(p, s)) continue;
                } catch (...) { continue; }
                auto it = std::find_if(skills_.begin(), skills_.end(),
                                       [&](const Skill & o) { return o.name == s.name; });
                if (it != skills_.end()) *it = std::move(s);   // later file wins
                else skills_.push_back(std::move(s));
            }
        } catch (...) {}
    }
    std::unique_lock lk(mu_);
    this->skills_.swap(skills_);
}

// Fingerprint of the skill dirs: a hash of the newest .md mtime and the file count across every
// configured path. Cheap to compute, enough to catch an added/edited/removed file without a full load.
uint64_t Skills::dir_key() const {
    uint64_t h = 1469598103967729411ULL;  // FNV-1a offset basis
    for (const auto & d : cfg_.paths) {
        std::error_code ec;
        fs::path dir = expand_home(d);
        if (!fs::is_directory(dir, ec)) continue;
        size_t count = 0;
        std::chrono::file_clock::time_point newest = std::chrono::file_clock::time_point::min();
        for (const auto & e : fs::directory_iterator(dir, ec)) {
            std::error_code de;
            if (e.is_directory(de) && parked_dir(e.path())) continue;
            if (e.path().extension() != ".md") continue;
            count++;
            auto mt = e.last_write_time(ec);
            if (mt > newest) newest = mt;
        }
        h ^= count;
        h *= 1099511628211ULL;
        h ^= uint64_t(newest.time_since_epoch().count());
        h *= 1099511628211ULL;
    }
    return h;
}
// Cheap refresh after a lead turn. Debounces: a burst of calls within ~2 s does not force a reload.
// Returns true when it actually reloaded, false when it short-circuited.
bool Skills::reload_if_changed() {
    auto now = std::chrono::steady_clock::now();
    {
        std::lock_guard lk(check_mu_);   // guards only the debounce state; readers of skills_ are not blocked
        // Debounce: if we refreshed the key within the window, assume no change.
        if (checked_ && now - last_check_ < std::chrono::seconds(k_min_refresh_s)) return false;
        checked_ = true;
        last_check_ = now;
        uint64_t key = dir_key();
        if (cache_key_ == key) return false;  // dirs unchanged since last applied reload
        cache_key_ = key;
    }
    reload();  // reload() swaps skills_ under its own unique lock
    return true;
}

std::vector<Skill> Skills::all() const {
    std::shared_lock lk(mu_);
    return skills_;
}

std::optional<Skill> Skills::by_name(const std::string & name) const {
    std::shared_lock lk(mu_);
    for (const auto & s : skills_)
        if (s.name == name) return s;
    return std::nullopt;
}

std::vector<Skill> Skills::matching(const std::string & text) const {
    std::string t = lower(text);
    std::vector<Skill> out;
    std::shared_lock lk(mu_);
    for (const auto & s : skills_)
        for (const auto & trig : s.triggers)
            if (trigger_matches(t, trig)) {
                out.push_back(s);
                break;
            }
    return out;
}

std::string Skills::index_block() const {
    std::shared_lock lk(mu_);
    if (skills_.empty()) return "";
    std::string out = "Available skills (call load_skill with the name to read one):\n";
    for (const auto & s : skills_) out += "- " + s.name + ": " + s.description + "\n";
    return out;
}

std::string Skills::write_dir() const {
    if (cfg_.paths.empty()) return "";
    std::string dir = expand_home(cfg_.paths.front());
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir;
}

}  // namespace judas

