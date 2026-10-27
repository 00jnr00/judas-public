#include "agent/policy.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace judas {

namespace {

std::string rtrim(std::string s) {
    while (!s.empty() && std::isspace((unsigned char) s.back())) s.pop_back();
    return s;
}

std::string ltrim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a);
}

uint64_t hash64(const std::string & s) {
    return std::hash<std::string>{}(s) | 1;  // never 0, 0 means "none"
}

bool starts_with(const std::string & s, const std::string & prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

// Models write the typographic apostrophe often enough that it must read like a plain one.
std::string lower_apostrophes(std::string s) {
    const std::string curly = "\xE2\x80\x99";
    for (size_t p = s.find(curly); p != std::string::npos; p = s.find(curly, p)) s.replace(p, curly.size(), "'");
    for (char & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

// A dot inside a path ("src/a.cpp") or a decimal has no space after it, so it does not split a sentence.
std::string last_sentence(const std::string & line) {
    size_t start = 0;
    for (size_t i = 0; i + 1 < line.size(); i++) {
        char c = line[i];
        if (c != '.' && c != '!' && c != '?') continue;
        if (!std::isspace((unsigned char) line[i + 1])) continue;
        size_t n = line.find_first_not_of(" \t", i + 1);
        if (n != std::string::npos) start = n;
    }
    return ltrim(line.substr(start));
}

std::string last_line(const std::string & text) {
    size_t end = text.find_last_not_of(" \t\r\n");
    if (end == std::string::npos) return "";
    size_t start = text.rfind('\n', end);
    start = start == std::string::npos ? 0 : start + 1;
    return ltrim(text.substr(start, end - start + 1));
}

// True when the sentence starts with the speaker saying what it is about to do, not what it did or what
// the user could do. Questions and the "Let me know" sign-off are answers, not announcements.
bool is_announcement(const std::string & sentence) {
    static const std::string lead = " \t-*+>#`\"'_";
    std::string s = ltrim(sentence);
    while (!s.empty() && lead.find(s[0]) != std::string::npos) s = ltrim(s.substr(1));
    size_t i = 0;
    while (i < s.size() && std::isdigit((unsigned char) s[i])) i++;
    if (i > 0 && i + 1 < s.size() && (s[i] == '.' || s[i] == ')') && std::isspace((unsigned char) s[i + 1]))
        s = ltrim(s.substr(i + 1));
    std::string l = lower_apostrophes(s);
    static const char * markers[] = {"now, ", "now ", "next, ", "next ", "then, ", "then ", "first, ", "first ",
                                     "finally, ", "finally ", "after that, ", "after that ", "afterwards, ",
                                     "so, ", "ok, ", "okay, ", "alright, "};
    for (bool cut = true; cut;) {
        cut = false;
        for (const char * m : markers) {
            if (!starts_with(l, m)) continue;
            l = ltrim(l.substr(std::string(m).size()));
            cut = true;
            break;
        }
    }
    if (l.find('?') != std::string::npos) return false;
    if (starts_with(l, "let me know")) return false;
    // "I will not ..." declines an action, it does not announce one.
    for (const char * no : {"i will not ", "i'll not ", "i won't ", "i will never ", "i'll never "})
        if (starts_with(l, no)) return false;
    static const char * actions[] = {"let me ",        "let's ",        "let us ",       "i'll ",
                                     "i will ",        "i shall ",      "i'm going to ", "i am going to ",
                                     "i'm about to ",  "i am about to ", "i need to ",   "i have to "};
    for (const char * a : actions)
        if (starts_with(l, a)) return true;
    return false;
}

bool announces_next_action(const std::string & text) { return is_announcement(last_sentence(last_line(text))); }

}  // namespace

const char * reply_kind_name(ReplyKind k) {
    switch (k) {
        case ReplyKind::ToolCalls: return "tool_calls";
        case ReplyKind::Final: return "final";
        case ReplyKind::Empty: return "empty";
        case ReplyKind::Announce: return "announce";
        case ReplyKind::TextToolCall: return "text_tool_call";
        case ReplyKind::Truncated: return "truncated";
    }
    return "?";
}

ReplyKind classify_reply(const std::string & content, bool has_tool_calls, const std::string & finish_reason) {
    if (finish_reason == "length") return ReplyKind::Truncated;
    if (has_tool_calls) return ReplyKind::ToolCalls;
    std::string c = rtrim(content);
    if (c.empty()) return ReplyKind::Empty;
    if (c.find("<tool_call>") != std::string::npos || c.find("<function=") != std::string::npos)
        return ReplyKind::TextToolCall;
    if (c.size() <= 400 && c.back() == ':') return ReplyKind::Announce;
    // "Let me read X." / "I'll fix the regex." / "Next, I need to ..." are plans, not answers: the turn only
    // ends here when the text was cut off or a tool call was parsed, which the checks above already handled.
    if (finish_reason == "stop" && announces_next_action(c)) return ReplyKind::Announce;
    return ReplyKind::Final;
}

PlanState plan_state(const std::string & plan) {
    PlanState st;
    std::stringstream ss(plan);
    for (std::string line; std::getline(ss, line);) {
        std::string l = ltrim(line);
        if (l.empty()) continue;
        if (l[0] == '-' || l[0] == '*' || l[0] == '+') {
            l = ltrim(l.substr(1));
        } else if (std::isdigit((unsigned char) l[0])) {
            size_t i = 0;
            while (i < l.size() && std::isdigit((unsigned char) l[i])) i++;
            if (i < l.size() && (l[i] == '.' || l[i] == ')')) l = ltrim(l.substr(i + 1));
        }
        if (l.rfind("[ ]", 0) == 0) st.open++;
        else if (l.rfind("[x]", 0) == 0 || l.rfind("[X]", 0) == 0) st.done++;
    }
    return st;
}

std::string truncation_nudge(bool was_tool_call, int completion_tokens) {
    std::string n = std::to_string(completion_tokens);
    if (was_tool_call)
        return "[harness] Your last reply was cut off at the output limit (" + n +
               " tokens), so the tool call was NOT run. Split large writes: write_file the first part (a few "
               "thousand characters), then write_file with append=true for each following part, or use edit_file "
               "in chunks. Then continue.";
    return "[harness] Your last reply was cut off at the output limit (" + n +
           " tokens). Think and write less per step: make the next tool call now, in small pieces.";
}

Decision should_continue(ReplyKind kind, const PlanState & plan, TurnState & st, int max_auto_continues,
                         int completion_tokens) {
    Decision d;
    if (kind == ReplyKind::ToolCalls) {
        d.action = Action::Execute;
        return d;
    }
    if (kind == ReplyKind::Final && plan.open == 0) {
        d.action = Action::Finish;
        return d;
    }
    // Not a finish: Empty, Announce, TextToolCall, Truncated, or a Final reply with plan items still open.
    std::string why = kind == ReplyKind::Final ? "plan_open" : reply_kind_name(kind);
    if (kind == ReplyKind::Truncated) st.truncations++;
    if (st.auto_continues >= max_auto_continues) {
        d.action = Action::Pause;
        d.reason = kind == ReplyKind::Final ? "auto-continue cap reached with plan items still open"
                                            : std::string("auto-continue cap reached (") + why + ")";
        return d;
    }
    st.auto_continues++;
    d.action = Action::Continue;
    d.reason = why;
    if (kind == ReplyKind::Truncated) d.nudge = truncation_nudge(false, completion_tokens);
    else if (kind == ReplyKind::Announce)
        d.nudge = "[harness] You said what you would do next but made no tool call. Make that call now.";
    else if (kind == ReplyKind::Final)
        d.nudge = "[harness] You replied without a tool call, but your plan still has " + std::to_string(plan.open) +
                  " open item(s). Call the next tool, tick finished items with update_plan, or call done with a "
                  "summary if the work is complete.";
    else
        d.nudge = "[harness] You did not call a tool. Either call the next tool or call done with a summary.";
    return d;
}

std::string normalize_error(const std::string & output) {
    std::string o;
    bool space = false;
    for (char ch : output) {
        unsigned char c = (unsigned char) ch;
        if (std::isdigit(c)) continue;
        if (std::isspace(c)) {
            if (!space && !o.empty()) o += ' ';
            space = true;
            continue;
        }
        space = false;
        o += (char) c;
        if (o.size() >= 4000) break;
    }
    return o;
}

LoopGuard::Result LoopGuard::observe(const std::string & tool, const std::string & args, bool ok, bool wrote,
                                     const std::string & output, bool track_call) {
    Entry e;
    e.tool = tool;
    if (ok && wrote) {
        for (auto & h : hist_) h.call = 0;  // the files changed: earlier identical reads and builds no longer repeat
    } else if (track_call) {
        e.call = hash64(tool + "\n" + args);
    }
    if (!ok) e.err = hash64(tool + "\n" + normalize_error(output));
    hist_.push_back(e);
    while ((int) hist_.size() > window_) hist_.pop_front();
    if (++since_warn_ >= calm_after_) warnings_ = 0;

    Result r;
    int calls = 0, errs = 0;
    if (e.call)
        for (auto & h : hist_) calls += h.call == e.call;
    if (e.err)
        for (auto & h : hist_) errs += h.err == e.err;
    if (calls >= repeats_ || errs >= repeats_) {
        bool by_call = calls >= repeats_;
        r.kind = by_call ? "call" : "error";
        r.tool = tool;
        r.count = by_call ? calls : errs;
        for (auto & h : hist_) {  // clear the evidence so the next warning needs fresh repeats
            if (by_call && h.call == e.call) h.call = 0;
            if (!by_call && h.err == e.err) h.err = 0;
        }
        warnings_++;
        since_warn_ = 0;
        r.verdict = warnings_ >= pause_after_ ? Verdict::Pause : Verdict::Warn;
    }
    return r;
}

std::string loop_nudge(const LoopGuard::Result & r, bool can_spawn) {
    std::string what = r.kind == "call" ? "the same " + r.tool + " call" : "the same " + r.tool + " error";
    return "[harness] You are looping: " + what + " " + std::to_string(r.count) +
           " times. Stop repeating it. Re-read the error, change your approach, or " +
           (can_spawn ? "spawn a worker with a fresh view of the problem, or " : "") +
           "stop and say what is blocking you (call done with that summary).";
}

bool delegation_nudge(DelegationState & st, bool is_edit, bool is_delegation, int open_items, int threshold,
                      bool can_spawn) {
    if (is_delegation) {
        st.consecutive_edits = 0;
        return false;
    }
    if (!is_edit) return false;
    st.consecutive_edits++;
    if (threshold <= 0 || !can_spawn || open_items < 2 || st.consecutive_edits < threshold) return false;
    st.consecutive_edits = 0;
    return true;
}

namespace {
std::string trim(const std::string & s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
}
std::vector<std::string> words(const std::string & s, size_t max = 3) {
    std::vector<std::string> w;
    size_t i = 0;
    while (i < s.size() && w.size() < max) {
        while (i < s.size() && isspace((unsigned char)s[i])) i++;
        size_t j = i;
        while (j < s.size() && !isspace((unsigned char)s[j])) j++;
        if (j > i) w.push_back(s.substr(i, j - i));
        i = j;
    }
    return w;
}
// Split on && ; || (not on a single |: a pipeline reads as one command, classified by its first word).
std::vector<std::string> split_segments(const std::string & cmd) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i = 0; i < cmd.size(); i++) {
        char c = cmd[i];
        bool two = i + 1 < cmd.size() && ((c == '&' && cmd[i + 1] == '&') || (c == '|' && cmd[i + 1] == '|'));
        if (two) {
            out.push_back(cur);
            cur.clear();
            i++;
        } else if (c == ';' || c == '\n') {
            out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    out.push_back(cur);
    return out;
}
// 0 = read-only, 1 = build/test or anything else (resets), 2 = neutral (cd, empty)
int classify_segment(const std::string & seg_in) {
    std::string seg = trim(seg_in);
    if (seg.empty()) return 2;
    auto w = words(seg);
    const std::string & a = w[0];
    std::string b = w.size() > 1 ? w[1] : "";
    if (a == "cd") return 2;
    // output redirection to a file writes (2>&1 and /dev/null do not count)
    for (size_t i = 0; i < seg.size(); i++) {
        if (seg[i] != '>') continue;
        if (i > 0 && seg[i - 1] == '&') continue;           // >&2
        size_t j = i + 1;
        if (j < seg.size() && seg[j] == '>') j++;
        if (j < seg.size() && seg[j] == '&') continue;      // 2>&1
        while (j < seg.size() && seg[j] == ' ') j++;
        if (seg.compare(j, 9, "/dev/null") == 0) continue;
        return 1;
    }
    if (a == "sed") return seg.find(" -i") != std::string::npos ? 1 : (b == "-n" ? 0 : 1);
    if (a == "cat" || a == "head" || a == "tail" || a == "ls" || a == "grep" || a == "rg" || a == "wc" || a == "tree")
        return 0;
    if (a == "find") return seg.find("-delete") != std::string::npos || seg.find("-exec") != std::string::npos ? 1 : 0;
    if (a == "git") return (b == "log" || b == "show" || b == "diff" || b == "status") ? 0 : 1;
    if (a == "cargo") return (b == "tree" || b == "metadata") ? 0 : 1;
    return 1;
}
}  // namespace

CallKind classify_call(const std::string & tool, const std::string & command, bool writes) {
    if (writes) return CallKind::Reset;
    if (tool == "read_file" || tool == "list_dir" || tool == "glob" || tool == "grep" || tool == "recall" ||
        tool == "snapshot_list" || tool == "snapshot_diff")
        return CallKind::Read;
    if (tool != "shell") return CallKind::Neutral;
    bool any_read = false;
    for (const auto & seg : split_segments(command)) {
        int k = classify_segment(seg);
        if (k == 1) return CallKind::Reset;
        if (k == 0) any_read = true;
    }
    return any_read ? CallKind::Read : CallKind::Neutral;
}

bool read_streak_blocked(const ReadStreak & st, int force) { return force > 0 && st.count >= force; }

StreakEvent read_streak_observe(ReadStreak & st, CallKind kind, int nudge, int force) {
    if (kind == CallKind::Reset) {
        st.count = 0;
        return StreakEvent::None;
    }
    if (kind == CallKind::Neutral) return StreakEvent::None;
    st.count++;
    if (force > 0 && st.count == force) return StreakEvent::Force;
    if (nudge > 0 && st.count == nudge) return StreakEvent::Nudge;
    return StreakEvent::None;
}

std::string read_streak_message(StreakEvent ev, int count) {
    if (ev == StreakEvent::Force)
        return "[harness] Reading is now blocked: " + std::to_string(count) +
               " read-only calls in a row. If the task needs changes, call write_file or edit_file, or run a "
               "build/test command. If it only asks for a review or an answer, stop and give your final answer now.";
    return "[harness] You have made " + std::to_string(count) +
           " read-only calls in a row. You have enough context. If the task needs changes, your next call must be "
           "write_file/edit_file or a build/test command. If it only asks for a review or an answer, give it now.";
}

std::string cap_tool_result(const std::string & out, size_t max_chars, const std::string & recall_id) {
    if (max_chars == 0 || out.size() <= max_chars) return out;
    size_t head = max_chars * 2 / 3, tail = max_chars - head;
    while (head > 0 && ((unsigned char) out[head] & 0xC0) == 0x80) head--;
    size_t from = out.size() - tail;
    while (from < out.size() && ((unsigned char) out[from] & 0xC0) == 0x80) from++;
    return out.substr(0, head) + "\n\n[... " + std::to_string(from - head) +
           " chars cut to fit the context" + (recall_id.empty() ? "" : "; full output: recall id=" + recall_id) +
           "; read files in parts with offset/limit ...]\n\n" + out.substr(from);
}

}  // namespace judas
