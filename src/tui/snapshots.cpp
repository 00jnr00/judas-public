#include "tui/snapshots.h"

#include <algorithm>
#include <ctime>

namespace judas::tui {

using judas::json;

static std::string jstr(const json & j, const char * k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}

std::vector<SnapInfo> parse_snapshots(const json & arr) {
    std::vector<SnapInfo> out;
    if (!arr.is_array()) return out;
    for (auto & j : arr) {
        SnapInfo s;
        s.id = jstr(j, "id");
        if (s.id.empty()) continue;
        s.session = jstr(j, "session");
        s.agent = jstr(j, "agent");
        s.label = jstr(j, "label");
        s.created_ms = j.value("created_ms", 0LL);
        s.committed = j.value("committed", false);
        s.reverted = j.value("reverted", false);
        if (j.contains("files") && j["files"].is_array())
            for (auto & f : j["files"]) s.files.push_back(jstr(f, "path"));
        if (j.contains("irreversible") && j["irreversible"].is_array())
            for (auto & x : j["irreversible"]) if (x.is_string()) s.irreversible.push_back(x.get<std::string>());
        out.push_back(std::move(s));
    }
    std::stable_sort(out.begin(), out.end(), [](const SnapInfo & a, const SnapInfo & b) { return a.created_ms > b.created_ms; });
    return out;
}

std::vector<SnapInfo> in_folder(const std::vector<SnapInfo> & all, const std::string & folder) {
    std::string f = folder;
    while (f.size() > 1 && f.back() == '/') f.pop_back();
    std::vector<SnapInfo> out;
    for (auto & s : all) {
        bool hit = false;
        for (auto & p : s.files)
            if (p == f || p.rfind(f == "/" ? "/" : f + "/", 0) == 0) { hit = true; break; }
        if (hit) out.push_back(s);
    }
    return out;
}

std::string resolve_snapshot_id(const std::vector<SnapInfo> & all, const std::string & q) {
    if (q.empty()) return "";
    for (auto & s : all) if (s.id == q) return s.id;
    std::string hit;
    int n = 0;
    for (auto & s : all) if (s.id.rfind(q, 0) == 0) { hit = s.id; n++; }
    if (n == 1) return hit;
    if (n > 1) return "";
    n = 0;
    for (auto & s : all)
        if (s.id.size() >= q.size() && s.id.compare(s.id.size() - q.size(), q.size(), q) == 0) { hit = s.id; n++; }
    return n == 1 ? hit : "";
}

DiffStat diff_stat(const std::string & diff) {
    DiffStat st;
    size_t pos = 0;
    while (pos < diff.size()) {
        size_t nl = diff.find('\n', pos);
        std::string ln = diff.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? diff.size() : nl + 1;
        if (ln.rfind("+++ ", 0) == 0) st.files++;
        else if (ln.rfind("--- ", 0) == 0) continue;
        else if (!ln.empty() && ln[0] == '+') st.added++;
        else if (!ln.empty() && ln[0] == '-') st.removed++;
    }
    return st;
}

std::string format_clock(long long ms, long long now_ms) {
    std::time_t t = (std::time_t)(ms / 1000), n = (std::time_t)(now_ms / 1000);
    std::tm a{}, b{};
    localtime_r(&t, &a);
    localtime_r(&n, &b);
    char buf[32];
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday) std::strftime(buf, sizeof buf, "%H:%M", &a);
    else std::strftime(buf, sizeof buf, "%b %d %H:%M", &a);
    return buf;
}

static std::string pad(const std::string & s, int w) {
    int d = display_width(s);
    return d >= w ? s : s + std::string(w - d, ' ');
}

std::vector<std::string> render_snapshot_list(const std::vector<SnapInfo> & snaps, long long now_ms, int width,
                                              const Palette & pal, const std::string & scope) {
    std::vector<std::string> rows;
    rows.push_back(pal.accent + pal.bold + "Snapshots" + pal.reset + pal.dim + "  " + scope + "  (" +
                   std::to_string(snaps.size()) + ")" + pal.reset);
    if (snaps.empty()) {
        rows.push_back(pal.dim + std::string("  none yet; every file edit the agent makes becomes one") + pal.reset);
        return rows;
    }
    // narrow terminals show only the trailing counter of an id ("0007"); /snapshots show and /revert accept it
    auto shown_id = [&](const SnapInfo & s) {
        if (width >= 100) return s.id;
        size_t d = s.id.rfind('-');
        return d == std::string::npos ? s.id : s.id.substr(d + 1);
    };
    size_t idw = 0;
    for (auto & s : snaps) idw = std::max(idw, shown_id(s).size());
    for (auto & s : snaps) {
        std::string when = pad(format_clock(s.created_ms, now_ms), 13);
        std::string agent = pad(truncate_width(one_line(s.agent.empty() ? "-" : s.agent), 8), 8);
        std::string nf = std::to_string(s.files.size()) + (s.files.size() == 1 ? " file " : " files");
        nf = pad(nf, 8);
        std::string counts;
        if (s.added >= 0) counts = pal.ok + pad("+" + std::to_string(s.added), 5) + pal.err + pad("-" + std::to_string(s.removed), 5) + pal.reset;
        else counts = pal.dim + pad("", 10) + pal.reset;
        std::string head = "  " + pal.dim + pad(shown_id(s), (int) idw) + "  " + when + agent + " " + pal.reset + pal.text + nf + " " + pal.reset + counts + " ";
        int used = 2 + (int) idw + 2 + 13 + 8 + 1 + 8 + 1 + 10 + 1;
        std::string tail;
        if (s.reverted) tail += " (reverted)";
        if (!s.irreversible.empty()) tail += " !";
        int room = std::max(10, width - used - (int) tail.size());
        std::string label = truncate_width(one_line(s.label), room);
        rows.push_back(head + pal.text + label + pal.reset + (s.reverted ? pal.dim : pal.warn) + tail + pal.reset);
    }
    rows.push_back(pal.dim + std::string("  /snapshots show ID   /revert ID   (an unambiguous start or end of an id works)") + pal.reset);
    return rows;
}

std::vector<std::string> render_diff(const std::string & diff, int width, const ThemeLook & look) {
    std::vector<std::string> rows;
    if (width < 20) width = 20;
    size_t pos = 0;
    while (pos < diff.size()) {
        size_t nl = diff.find('\n', pos);
        std::string ln = diff.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? diff.size() : nl + 1;
        std::string clean;
        for (char c : ln) {
            if (c == '\t') clean += "    ";
            else if (c == '\r') continue;
            else if ((unsigned char) c < 0x20 || c == 0x7f) clean += '?';
            else clean += c;
        }
        const std::string * color = &look.diff.ctx;
        if (clean.rfind("+++ ", 0) == 0 || clean.rfind("--- ", 0) == 0) color = &look.diff.head;
        else if (clean.rfind("@@", 0) == 0) color = &look.diff.hunk;
        else if (!clean.empty() && clean[0] == '+') color = &look.diff.add;
        else if (!clean.empty() && clean[0] == '-') color = &look.diff.del;
        else if (!clean.empty() && clean[0] == ' ') color = &look.pal.text;
        // hard split by columns so indentation and spacing survive
        std::string cur;
        int w = 0;
        std::vector<std::string> parts;
        for (size_t i = 0; i < clean.size();) {
            size_t n = utf8_next(clean, i) - i;
            int cw = cp_width(utf8_decode(clean, i, &n));
            if (w + cw > width - 3 && !cur.empty()) { parts.push_back(cur); cur.clear(); w = 0; }
            cur += clean.substr(i, n);
            w += cw;
            i += n;
        }
        parts.push_back(cur);
        for (size_t k = 0; k < parts.size(); k++)
            rows.push_back(*color + (k ? "  " : "") + parts[k] + look.pal.reset);
    }
    return rows;
}

std::vector<std::string> render_revert_preview(const SnapInfo & s, const DiffStat & st, bool to, int width, const Palette & pal) {
    std::vector<std::string> rows;
    rows.push_back(pal.warn + pal.bold + (to ? "Revert everything back to " : "Revert ") + pal.reset + pal.text +
                   truncate_width(one_line(s.label.empty() ? s.id : s.label), std::max(20, width - 24)) + pal.reset + pal.dim + "  " + s.id + pal.reset);
    if (to) rows.push_back(pal.warn + std::string("  this undoes this snapshot and every later one") + pal.reset);
    size_t shown = 0;
    for (auto & f : s.files) {
        if (shown++ >= 12) { rows.push_back(pal.dim + std::string("  ... and ") + std::to_string(s.files.size() - 12) + " more" + pal.reset); break; }
        rows.push_back("  " + pal.text + truncate_width(f, std::max(20, width - 4)) + pal.reset);
    }
    rows.push_back(pal.dim + std::string("  ") + std::to_string(s.files.size()) + (s.files.size() == 1 ? " file" : " files") +
                   ", currently differing by " + pal.reset + pal.ok + "+" + std::to_string(st.added) + pal.reset + " " + pal.err +
                   "-" + std::to_string(st.removed) + pal.reset + pal.dim + " lines (/snapshots show " + s.id + ")" + pal.reset);
    if (s.reverted) rows.push_back(pal.warn + std::string("  already reverted once") + pal.reset);
    for (auto & x : s.irreversible)
        rows.push_back(pal.warn + std::string("  cannot be undone: ") + truncate_width(one_line(x), std::max(20, width - 24)) + pal.reset);
    return rows;
}

std::vector<std::string> render_revert_result(const json & r, int width, const Palette & pal) {
    std::vector<std::string> rows;
    bool bad = r.value("ok", true) == false || !jstr(r, "error").empty();
    bool conflicts = r.contains("conflicts") && r["conflicts"].is_array() && !r["conflicts"].empty();
    rows.push_back((bad ? pal.err : conflicts ? pal.warn : pal.ok) + std::string(bad ? "Revert failed" : "Revert finished") + pal.reset);
    if (!jstr(r, "error").empty()) rows.push_back("  " + pal.err + jstr(r, "error") + pal.reset);
    const std::pair<const char *, const char *> groups[] = {
        {"restored", "restored"}, {"removed", "removed"}, {"conflicts", "changed since the snapshot (restored anyway)"},
        {"irreversible", "not undone"}};
    for (auto & g : groups) {
        if (!r.contains(g.first) || !r[g.first].is_array() || r[g.first].empty()) continue;
        for (auto & x : r[g.first]) {
            std::string v = x.is_string() ? x.get<std::string>() : x.dump();
            bool warn = std::string(g.first) == "conflicts" || std::string(g.first) == "irreversible";
            rows.push_back("  " + (warn ? pal.warn : pal.text) + std::string(g.second) + ": " +
                           truncate_width(one_line(v), std::max(20, width - 8 - (int) std::string(g.second).size())) + pal.reset);
        }
    }
    return rows;
}

std::vector<std::string> page_slice(const std::vector<std::string> & rows, size_t pos, size_t page, size_t * next) {
    if (pos > rows.size()) pos = rows.size();
    size_t end = std::min(rows.size(), pos + std::max<size_t>(1, page));
    if (next) *next = end;
    return std::vector<std::string>(rows.begin() + pos, rows.begin() + end);
}

}  // namespace judas::tui
