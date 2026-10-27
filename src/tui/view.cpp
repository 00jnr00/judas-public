#include "tui/view.h"

#include <algorithm>

namespace judas::tui {

using json = judas::json;

namespace {

const char * DOT = "\xE2\x97\x8F";     // black circle
const char * ELBOW = "\xE2\x8E\xBF";   // result connector
const char * THINK = "\xE2\x9C\xBB";
const char * DIAMOND = "\xE2\x97\x87";
const char * CHECK = "\xE2\x9C\x93";
const char * CROSS = "\xE2\x9C\x97";
const char * SPIN[] = {"\xC2\xB7", "\xE2\x9C\xA2", "\xE2\x9C\xB3", "\xE2\x9C\xB6", "\xE2\x9C\xBB", "\xE2\x9C\xBD"};

std::string jstr(const json & j, const char * k) {
    return j.is_object() && j.contains(k) && j[k].is_string() ? j[k].get<std::string>() : std::string();
}

json parse_args(const json & a) {
    if (a.is_string()) {
        auto j = json::parse(a.get<std::string>(), nullptr, false);
        return j.is_discarded() ? json(a) : j;
    }
    return a;
}

int count_lines(const std::string & s) {
    if (s.empty()) return 0;
    int n = (int) std::count(s.begin(), s.end(), '\n');
    return s.back() == '\n' ? n : n + 1;
}

}  // namespace

std::string tool_label(const std::string & name, const json & args0, int max_cols) {
    json args = parse_args(args0);
    std::string s;
    if (args.is_object()) {
        for (const char * k : {"path", "file_path", "file", "command", "cmd", "pattern", "query", "url", "task", "name", "id"}) {
            std::string v = jstr(args, k);
            if (!v.empty()) { s = v; break; }
        }
        if (s.empty() && !args.empty()) s = args.dump();
    } else if (args.is_string()) {
        s = args.get<std::string>();
    } else if (!args.is_null()) {
        s = args.dump();
    }
    if (name == "update_plan") s.clear();
    return truncate_width(one_line(s), max_cols);
}

std::string output_summary(const std::string & out, int max_cols, int * total) {
    int n = count_lines(out);
    if (total) *total = n;
    if (n == 0) return "(no output)";
    size_t i = 0;
    std::string first;
    while (i < out.size()) {
        size_t e = out.find('\n', i);
        std::string ln = out.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? out.size() : e + 1;
        if (ln.find_first_not_of(" \t\r") != std::string::npos) { first = ln; break; }
    }
    std::string s = truncate_width(one_line(first), max_cols);
    if (n > 1) s += " (+" + std::to_string(n - 1) + " lines)";
    return s;
}

Stream::Stream(Palette pal, int width) : pal_(std::move(pal)), width_(width < 20 ? 20 : width) {}

void Stream::reset() {
    running_ = text_started_ = false;
    turn_status_ = "idle";
    reason_.clear(); plan_.clear(); tbuf_.clear(); rbuf_.clear(); cur_reply_.clear();
    turn_tokens_ = 0;
    md_ = MdState();
    pending_.clear();
    agents_.clear();
    replies_.clear();
    approval.reset();
}

void Stream::set_running(bool r) {
    running_ = r;
    if (r) { turn_status_ = "running"; turn_tokens_ = 0; reason_.clear(); }
    else if (turn_status_ == "running") turn_status_ = "done";
}

void Stream::commit(const std::vector<std::string> & rows) {
    for (auto & r : rows) out_.push_back(r);
}

void Stream::note(const std::string & text, char level) {
    const std::string & c = level == 'w' ? pal_.warn : level == 'e' ? pal_.err : level == 'n' ? pal_.text : pal_.dim;
    for (auto & r : wrap(text, width_ - 2))
        out_.push_back(c + r.text + pal_.reset);
}

std::vector<std::string> Stream::take_committed() {
    std::vector<std::string> o;
    o.swap(out_);
    return o;
}

void Stream::end_reply() {
    if (!cur_reply_.empty()) {
        while (!cur_reply_.empty() && (cur_reply_.back() == '\n' || cur_reply_.back() == ' ')) cur_reply_.pop_back();
        replies_.push_back(cur_reply_);
    }
    cur_reply_.clear();
}

std::vector<std::string> Stream::replies() const {
    auto r = replies_;
    if (!cur_reply_.empty()) r.push_back(cur_reply_);
    return r;
}

void Stream::user_message(const std::string & text) {
    flush_text(true);
    flush_reasoning();
    end_reply();
    out_.push_back("");
    bool first = true;
    size_t pos = 0;
    while (true) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        for (auto & r : wrap(line, width_ - 2)) {
            out_.push_back(pal_.user + (first ? "> " : "  ") + r.text + pal_.reset);
            first = false;
        }
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    out_.push_back("");
}

void Stream::add_text(const std::string & t) {
    flush_reasoning();
    tbuf_ += t;
    cur_reply_ += t;
    flush_text(false);
}

void Stream::flush_text(bool all) {
    auto emit_line = [&](const std::string & line) {
        std::string first = text_started_ ? "  " : pal_.text + DOT + pal_.reset + " ";
        bool blank = line.find_first_not_of(" \t\r") == std::string::npos;
        for (auto & r : render_md_line(line, width_, md_, pal_, first, "  ")) out_.push_back(r);
        if (!blank) text_started_ = true;
    };
    size_t nl;
    while ((nl = tbuf_.find('\n')) != std::string::npos) {
        std::string line = tbuf_.substr(0, nl);
        tbuf_.erase(0, nl + 1);
        emit_line(line);
    }
    if (!all) {
        // A very long paragraph: commit the rows that are certain, keep the last three in the live area.
        auto rows = wrap(tbuf_, width_ - 2);
        if (rows.size() > 6 && !md_.in_fence) {
            size_t k = rows.size() - 3;
            emit_line(tbuf_.substr(0, rows[k - 1].consumed));
            tbuf_.erase(0, rows[k - 1].consumed);
        }
        return;
    }
    if (tbuf_.find_first_not_of(" \t\r") != std::string::npos) emit_line(tbuf_);
    tbuf_.clear();
    text_started_ = false;
    md_ = MdState();
}

void Stream::flush_reasoning() {
    if (rbuf_.empty()) return;
    int n = count_lines(rbuf_);
    if (verbose_) {
        out_.push_back(pal_.think + THINK + " Thinking" + pal_.reset);
        size_t pos = 0;
        while (pos < rbuf_.size()) {
            size_t nl = rbuf_.find('\n', pos);
            std::string line = rbuf_.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
            pos = nl == std::string::npos ? rbuf_.size() : nl + 1;
            for (auto & r : wrap(line, width_ - 4)) out_.push_back(pal_.think + "  " + r.text + pal_.reset);
        }
    } else {
        out_.push_back(pal_.think + THINK + " Thinking (" + std::to_string(n) + (n == 1 ? " line" : " lines") +
                       (hinted_ ? ")" : ", ctrl+o to show)") + pal_.reset);
        hinted_ = true;
    }
    rbuf_.clear();
}

AgentInfo & Stream::agent_of(const json & ev) {
    std::string id = jstr(ev, "agent");
    for (auto & a : agents_) if (a.id == id) return a;
    AgentInfo a;
    a.id = id;
    a.parent = jstr(ev, "parent");
    a.role = jstr(ev, "role");
    a.status = "running";
    agents_.push_back(a);
    return agents_.back();
}

std::vector<std::string> Stream::tool_rows(const Pending & p) const {
    std::vector<std::string> o;
    std::string ind(p.indent, ' ');
    const std::string & c = p.log.ok ? pal_.ok : pal_.err;
    o.push_back(ind + c + DOT + pal_.reset + " " + pal_.bold + p.log.name + pal_.reset + " " + pal_.dim +
                p.log.label + pal_.reset);
    int total = 0;
    std::string sum = output_summary(p.log.output, width_ - p.indent - 8, &total);
    std::string hint = total > 1 && !verbose_ ? " /expand " + std::to_string(p.log.n) : "";
    if (!p.log.ok) {
        o.push_back(ind + "  " + pal_.err + ELBOW + " " + sum + pal_.reset + pal_.dim + hint + pal_.reset);
    } else if (p.log.name != "update_plan") {
        o.push_back(ind + "  " + pal_.dim + ELBOW + " " + sum + hint + pal_.reset);
    }
    if (verbose_) {
        for (auto & r : expand_rows(p.log.n)) o.push_back(ind + r);
    }
    return o;
}

std::vector<std::string> Stream::expand_rows(int n) const {
    std::vector<std::string> o;
    const ToolLog * t = tool(n);
    if (!t) return o;
    size_t pos = 0;
    int shown = 0;
    const std::string & s = t->output;
    while (pos < s.size() && shown < 60) {
        size_t nl = s.find('\n', pos);
        std::string line = s.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? s.size() : nl + 1;
        for (auto & r : wrap(line, width_ - 6)) { o.push_back(pal_.dim + "    " + r.text + pal_.reset); shown++; }
    }
    if (pos < s.size()) o.push_back(pal_.dim + "    ... (" + std::to_string(count_lines(s.substr(pos))) + " more lines)" + pal_.reset);
    return o;
}

const ToolLog * Stream::tool(int n) const {
    if (log_.empty()) return nullptr;
    if (n <= 0) return &log_.back();
    for (auto & t : log_) if (t.n == n) return &t;
    return nullptr;
}

void Stream::commit_done_tools() {
    while (!pending_.empty() && pending_.front().done) {
        for (auto & r : tool_rows(pending_.front())) out_.push_back(r);
        log_.push_back(pending_.front().log);
        if (log_.size() > 200) log_.pop_front();
        pending_.pop_front();
    }
}

void Stream::tool_call(const std::string & agent, bool lead, const std::string & id, const std::string & name,
                       const json & args) {
    if (lead) { flush_text(true); flush_reasoning(); }
    Pending p;
    p.id = id;
    p.agent = agent;
    p.indent = lead ? 0 : 2;
    p.log.n = ++tool_counter_;
    p.log.name = name;
    p.log.label = tool_label(name, args, width_ - p.indent - name.size() - 4);
    if (lead && name == "update_plan") {
        json a = parse_args(args);
        if (a.is_object()) plan_ = jstr(a, "plan");
    }
    pending_.push_back(std::move(p));
}

void Stream::tool_result(const std::string & agent, const std::string & id, const std::string & name, bool ok,
                         const std::string & output) {
    Pending * hit = nullptr;
    for (auto & p : pending_)
        if (!p.done && ((!id.empty() && p.id == id) || (id.empty() && p.agent == agent && p.log.name == name))) { hit = &p; break; }
    if (!hit) {  // result without a call (history gap): show it on its own
        Pending p;
        p.id = id;
        p.agent = agent;
        p.log.n = ++tool_counter_;
        p.log.name = name.empty() ? "tool" : name;
        p.done = true;
        pending_.push_back(p);
        hit = &pending_.back();
    }
    hit->log.ok = ok;
    hit->log.output = output;
    hit->done = true;
    commit_done_tools();
    if (!ok && output.rfind("not approved:", 0) == 0)
        note("  needs approval; the daemon's permission mode blocks this (see docs/TUI.md, Permissions)", 'w');
}

void Stream::end_turn(const std::string & status, const std::string & reason) {
    flush_text(true);
    flush_reasoning();
    for (auto & p : pending_) p.done = true;  // an interrupted turn leaves no spinner behind
    commit_done_tools();
    end_reply();
    running_ = false;
    approval.reset();
    turn_status_ = status.empty() ? "done" : status;
    reason_ = reason;
    if (turn_status_ == "paused")
        note("paused: " + (reason.empty() ? std::string("a limit was reached") : reason) + "  (/resume to continue)", 'w');
    else if (turn_status_ == "failed")
        note("turn failed" + (reason.empty() ? std::string() : ": " + reason), 'e');
    else if (turn_status_ == "killed")
        note("cancelled", 'd');
}

void Stream::on_event(const json & ev) {
    std::string type = jstr(ev, "type");
    if (type == "closed") { if (running_) end_turn("done", ""); return; }
    if (type == "gap") { note("(some earlier events were not kept; the transcript may have a hole)", 'd'); return; }
    if (type == "session_deleted") { note("this session was deleted", 'w'); end_turn("killed", ""); return; }
    if (type == "approval_request") {
        const json d = ev.contains("data") && ev["data"].is_object() ? ev["data"] : ev;
        approval = Approval{jstr(d, "id"), jstr(d, "what")};
        return;
    }
    json d = ev.contains("data") && ev["data"].is_object() ? ev["data"] : json::object();
    AgentInfo & a = agent_of(ev);
    bool lead = a.parent.empty();
    std::string tag = lead ? "" : a.role.empty() ? "worker" : a.role;
    if (type == "spawned") {
        if (a.role.empty()) a.role = jstr(d, "role");
        a.task = jstr(d, "task");
        a.status = "running";
        if (lead) {
            running_ = true;
            turn_status_ = "running";
            reason_.clear();
            turn_tokens_ = 0;
        } else {
            out_.push_back("  " + pal_.dim + DIAMOND + " " + a.role + "  " +
                           truncate_width(one_line(a.task), width_ - 8 - (int) a.role.size()) + pal_.reset);
        }
    } else if (type == "delta") {
        a.status = "running";
        std::string t = jstr(d, "text");
        if (lead) add_text(t);
        else a.activity = truncate_width(one_line(t.size() > 60 ? t.substr(t.size() - 60) : t), 40);
    } else if (type == "reasoning") {
        a.status = "running";
        if (lead) { flush_text(true); rbuf_ += jstr(d, "text"); }
    } else if (type == "tool_call") {
        a.status = "running";
        a.activity = jstr(d, "name");
        tool_call(a.id, lead, jstr(d, "id"), jstr(d, "name"), d.contains("args") ? d["args"] : json::object());
    } else if (type == "tool_result") {
        bool ok = !d.contains("ok") || !d["ok"].is_boolean() || d["ok"].get<bool>();
        tool_result(a.id, jstr(d, "id"), jstr(d, "name"), ok, jstr(d, "output"));
    } else if (type == "message") {
        if (d.contains("usage") && d["usage"].is_object() && d["usage"].contains("completion") &&
            d["usage"]["completion"].is_number())
            turn_tokens_ += d["usage"]["completion"].get<long long>();
    } else if (type == "status") {
        if (d.contains("error")) {
            a.status = "failed";
            std::string e = d["error"].is_string() ? d["error"].get<std::string>() : d["error"].dump();
            note((lead ? "" : "[" + tag + "] ") + e, 'e');
        } else if (jstr(d, "turn_status") == "paused") {
            reason_ = jstr(d, "reason");
        } else if (d.contains("nudge")) {
            note("  nudge: " + jstr(d, "nudge"), 'd');
        }
    } else if (type == "done") {
        std::string st = jstr(d, "status");
        a.status = st.empty() ? "done" : st;
        if (lead) {
            end_turn(a.status, jstr(d, "reason"));
        } else {
            const std::string & c = a.status == "done" ? pal_.ok : pal_.err;
            std::string res = output_summary(jstr(d, "result"), width_ - 10 - (int) a.role.size(), nullptr);
            out_.push_back("  " + c + (a.status == "done" ? CHECK : CROSS) + pal_.reset + " " + pal_.dim + a.role + ": " + res + pal_.reset);
        }
    }
}

std::vector<std::string> Stream::live(int frame, double elapsed) const {
    std::vector<std::string> o;
    const char * sp = SPIN[((frame % 6) + 6) % 6];
    int shown = 0;
    for (auto & p : pending_) {
        if (shown++ >= 6) break;
        std::string ind(p.indent, ' ');
        o.push_back(ind + pal_.accent + (p.done ? DOT : sp) + pal_.reset + " " + pal_.bold + p.log.name + pal_.reset + " " +
                    pal_.dim + p.log.label + pal_.reset);
    }
    if (!tbuf_.empty()) {
        MdState st = md_;
        std::string first = text_started_ ? "  " : pal_.text + DOT + pal_.reset + " ";
        for (auto & r : render_md_line(tbuf_, width_, st, pal_, first, "  ")) o.push_back(r);
    }
    if (!rbuf_.empty()) {
        std::string last = rbuf_;
        while (!last.empty() && (last.back() == '\n' || last.back() == ' ')) last.pop_back();
        size_t nl = last.rfind('\n');
        if (nl != std::string::npos) last = last.substr(nl + 1);
        int cols = width_ - 16;
        if (display_width(last) > cols) {
            // keep the tail so the newest words show
            size_t i = last.size();
            int w = 0;
            while (i > 0) {
                size_t p = utf8_prev(last, i);
                size_t n;
                int cw = cp_width(utf8_decode(last, p, &n));
                if (w + cw > cols) break;
                w += cw;
                i = p;
            }
            last = last.substr(i);
        }
        o.push_back(pal_.think + THINK + " Thinking... " + last + pal_.reset);
    }
    shown = 0;
    for (auto & a : agents_) {
        if (a.parent.empty() || a.status == "done" || a.status == "failed" || a.status == "killed") continue;
        if (shown++ >= 4) break;
        o.push_back("  " + pal_.dim + sp + " " + a.role + " [" + a.status + "] " +
                    truncate_width(one_line(a.activity.empty() ? a.task : a.activity), width_ - 18 - (int) a.role.size()) +
                    pal_.reset);
    }
    if (running_) {
        char b[96];
        std::snprintf(b, sizeof b, " Working... (%ds%s%s, esc to interrupt)", (int) elapsed,
                      turn_tokens_ ? ", " : "", turn_tokens_ ? (human_tokens(turn_tokens_) + " tok").c_str() : "");
        o.push_back(pal_.accent + sp + pal_.reset + pal_.dim + b + pal_.reset);
    }
    return o;
}

void Stream::load_history(const json & messages, const json & agents, long long max_id) {
    std::string lead;
    if (agents.is_array())
        for (auto & a : agents)
            if (jstr(a, "parent").empty()) { lead = jstr(a, "id"); break; }
    if (!messages.is_array()) return;
    for (auto & m : messages) {
        if (max_id >= 0 && m.contains("id") && m["id"].is_number() && m["id"].get<long long>() > max_id) continue;
        std::string ag = jstr(m, "agent");
        if (!lead.empty() && ag != lead) continue;
        json b = m.contains("body") ? m["body"] : json::object();
        if (b.is_string()) { auto p = json::parse(b.get<std::string>(), nullptr, false); b = p.is_discarded() ? json::object() : p; }
        std::string role = jstr(m, "role");
        if (role.empty()) role = jstr(b, "role");
        std::string content = b.contains("content") && b["content"].is_string() ? b["content"].get<std::string>() : "";
        if (role == "user") {
            if (!content.empty()) user_message(content);
        } else if (role == "assistant") {
            std::string rc = jstr(b, "reasoning_content");
            if (!rc.empty()) { rbuf_ += rc; flush_reasoning(); }
            if (!content.empty()) { add_text(content); flush_text(true); }
            if (b.contains("tool_calls") && b["tool_calls"].is_array())
                for (auto & tc : b["tool_calls"]) {
                    json fn = tc.contains("function") ? tc["function"] : json::object();
                    tool_call(ag, true, jstr(tc, "id"), jstr(fn, "name"), fn.contains("arguments") ? fn["arguments"] : json::object());
                }
        } else if (role == "tool") {
            bool ok = content.rfind("not approved:", 0) != 0 && content.rfind("error", 0) != 0 && content != "cancelled";
            tool_result(ag, jstr(b, "tool_call_id"), "", ok, content);
        }
    }
    flush_text(true);
    for (auto & p : pending_) p.done = true;
    commit_done_tools();
    end_reply();
}

}  // namespace judas::tui
