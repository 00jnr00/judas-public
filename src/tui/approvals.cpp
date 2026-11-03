#include "tui/approvals.h"

#include <algorithm>

namespace judas::tui {

namespace {
std::string jstr(const judas::json & j, const char * k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}
}  // namespace

ApprovalReq approval_from_json(const judas::json & j) {
    ApprovalReq r;
    if (!j.is_object()) return r;
    const judas::json & d = j.contains("data") && j["data"].is_object() ? j["data"] : j;
    r.id = jstr(d, "id");
    r.tool = jstr(d, "tool");
    r.what = jstr(d, "what");
    r.agent = jstr(d, "agent").empty() ? jstr(j, "agent") : jstr(d, "agent");
    r.role = jstr(d, "role").empty() ? jstr(j, "role") : jstr(d, "role");
    r.strict = d.contains("strict") && d["strict"].is_boolean() && d["strict"].get<bool>();
    r.created_ms = d.contains("created_ms") && d["created_ms"].is_number_integer() ? d["created_ms"].get<long long>() : 0;
    return r;
}

bool ApprovalQueue::add(ApprovalReq r) {
    if (r.id.empty()) return false;
    for (auto & x : items_) if (x.id == r.id) return false;
    items_.push_back(std::move(r));
    return true;
}

bool ApprovalQueue::remove(const std::string & id) {
    for (auto it = items_.begin(); it != items_.end(); ++it)
        if (it->id == id) { items_.erase(it); return true; }
    return false;
}

void ApprovalQueue::sync(const std::vector<ApprovalReq> & pending) {
    std::vector<ApprovalReq> next;
    for (auto & p : pending) {
        if (p.id.empty()) continue;
        auto it = std::find_if(items_.begin(), items_.end(), [&](const ApprovalReq & x) { return x.id == p.id; });
        next.push_back(it != items_.end() ? *it : p);   // keep the local copy so the order is stable
    }
    items_ = std::move(next);
}

static std::string first_line(const std::string & s) {
    size_t nl = s.find('\n');
    return nl == std::string::npos ? s : s.substr(0, nl);
}

std::vector<std::string> approval_body_rows(const ApprovalReq & r, const Palette & pal, int width) {
    std::vector<std::string> rows;
    size_t nl = r.what.find('\n');
    const int w = std::max(10, width - 4);
    // a one-line request that does not fit the header (a long shell command) has its full text as the body
    if (nl == std::string::npos && display_width(r.what) <= w) return rows;
    const std::string body = nl == std::string::npos ? r.what : r.what.substr(nl + 1);
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t e = body.find('\n', pos);
        std::string line = body.substr(pos, e == std::string::npos ? std::string::npos : e - pos);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string & color = !line.empty() && line[0] == '+' ? pal.ok : !line.empty() && line[0] == '-' ? pal.err : pal.dim;
        // a wrapped diff line keeps its color on every row
        for (auto & wr : wrap(line, w)) rows.push_back("    " + color + wr.text + pal.reset);
        if (e == std::string::npos) break;
        pos = e + 1;
    }
    while (!rows.empty() && strip_ansi(rows.back()).find_first_not_of(' ') == std::string::npos) rows.pop_back();
    return rows;
}

std::vector<std::string> approval_rows(const ApprovalReq & r, const Palette & pal, int width, int max_body) {
    std::vector<std::string> out;
    std::string who = !r.role.empty() && r.role != "lead" ? r.role + " " + r.agent : r.agent.empty() ? "lead" : "lead " + r.agent;
    std::string head = pal.warn + std::string("  approval needed") + pal.reset + "  " + pal.bold + (r.tool.empty() ? "tool" : r.tool) +
                       pal.reset + pal.dim + "  (" + who + (r.strict ? "; always asked" : "") + ")" + pal.reset;
    out.push_back(head);
    out.push_back("  " + truncate_width(one_line(first_line(r.what)), std::max(10, width - 4)));
    auto body = approval_body_rows(r, pal, width);
    int n = (int) std::min<size_t>(body.size(), (size_t) std::max(0, max_body));
    for (int i = 0; i < n; i++) out.push_back(body[(size_t) i]);
    if ((size_t) n < body.size())
        out.push_back(pal.dim + std::string("    ... ") + std::to_string(body.size() - (size_t) n) + " more lines ([v] to view all)" + pal.reset);
    return out;
}

std::vector<std::string> approval_page(const std::vector<std::string> & body, size_t from, int n, size_t * next) {
    std::vector<std::string> out;
    size_t end = std::min(body.size(), from + (size_t) std::max(1, n));
    for (size_t i = from; i < end; i++) out.push_back(body[i]);
    if (next) *next = end;
    return out;
}

std::string approval_prompt(const Palette & pal, size_t waiting_more, bool viewing, size_t shown, size_t total) {
    std::string s = pal.warn + std::string("  Approve? [y]es / [n]o / [v]iew full") + pal.reset;
    if (viewing)
        s = pal.warn + std::string("  Approve? [y]es / [n]o / [space] more / [q] stop viewing") + pal.reset + pal.dim + "  (" +
            std::to_string(shown) + "/" + std::to_string(total) + " lines)" + pal.reset;
    if (waiting_more) s += pal.dim + std::string("  +") + std::to_string(waiting_more) + " waiting" + pal.reset;
    return s;
}

std::vector<std::string> approval_list_rows(const ApprovalQueue & q, const Palette & pal, int width) {
    std::vector<std::string> out;
    if (q.empty()) { out.push_back(pal.dim + std::string("  no approvals pending") + pal.reset); return out; }
    int i = 0;
    for (auto & r : q.items()) {
        std::string who = r.role.empty() ? r.agent : r.role + " " + r.agent;
        out.push_back("  " + std::string(i == 0 ? "> " : "  ") + r.id + "  " + (r.tool.empty() ? "tool" : r.tool) + "  " + pal.dim + who + pal.reset +
                      "  " + truncate_width(one_line(first_line(r.what)), std::max(10, width - 30)));
        i++;
    }
    return out;
}

ApprovalAct approval_key(bool viewing, const Key & k) {
    if (k.type == Key::Esc) return ApprovalAct::Deny;
    if (k.type == Key::PgDn) return viewing ? ApprovalAct::ViewMore : ApprovalAct::None;
    if (k.type != Key::Char || k.ctrl || k.alt) return ApprovalAct::None;
    if (k.text == "y" || k.text == "Y") return ApprovalAct::Approve;
    if (k.text == "n" || k.text == "N") return ApprovalAct::Deny;
    if (k.text == "v" || k.text == "V") return ApprovalAct::ViewMore;
    if (k.text == " ") return viewing ? ApprovalAct::ViewMore : ApprovalAct::None;
    if (k.text == "q" || k.text == "Q") return viewing ? ApprovalAct::StopView : ApprovalAct::None;
    return ApprovalAct::None;
}

}  // namespace judas::tui
