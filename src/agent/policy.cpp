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

// A finite verb anywhere in a short line means it is a statement, not a bare noun phrase naming the next target.
bool has_finite_verb(const std::string & text) {
    static const char * finite[] = {
        "is", "are", "was", "were", "shows", "show", "showed", "confirms", "confirmed", "gives", "gave", "works",
        "worked", "passes", "passed", "pass", "looks", "means", "makes", "made", "returns", "returned", "fails",
        "failed", "fail", "takes", "took", "found", "finds", "has", "have", "had", "matches", "matched", "match",
        "proves", "proved", "says", "said", "done", "complete", "work", "run", "runs", "ran", "fit", "fits",
        "remain", "remains", "seem", "seems", "look", "appear", "appears", "belong", "belongs", "matter", "matters",
        "exist", "exists", "depend", "depends", "include", "includes", "contain", "contains", "cover", "covers",
        "span", "spans", "weigh", "weighs", "measure", "measures", "cost", "costs", "total", "totals", "add", "adds",
        "remove", "removes", "help", "helps", "need", "needs", "want", "wants", "allow", "allows", "require",
        "requires", "use", "uses", "used", "become", "becomes", "came", "come", "comes", "went", "go", "goes",
        "compiles", "builds", "renders", "opens", "loads", "starts", "finishes", "ends", "begins", "continues",
        "succeeds", "breaks", "changes", "updates", "improves", "reduces", "increases", "solves", "meets", "keeps",
        "leaves", "stays", "gets", "puts", "sets", "tells", "asks", "writes", "reads", "checks", "pulls", "fetches"};
    std::string cur, prev;
    auto is_finite = [&](const std::string & w) {
        for (const char * f : finite)
            if (w == f) return true;
        return false;
    };
    for (char ch : text) {
        if (std::isalpha((unsigned char) ch) || ch == '\'') {
            cur += (char) std::tolower((unsigned char) ch);
        } else if (!cur.empty()) {
            if (prev != "to" && is_finite(cur)) return true;  // "how to remove X" is an infinitive, not finite
            prev = cur;
            cur.clear();
        }
    }
    if (!cur.empty() && prev != "to" && is_finite(cur)) return true;
    return false;
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
    // Conversational openers the model puts in front of the real sentence. Stripping them lets "Sure, let me
    // look up X." read as the announcement it is (seen live: the turn paused for 9 minutes after this line).
    static const char * preambles[] = {"sure, ",   "right, ",       "got it, ",     "great, ",     "good, ",
                                       "cool, ",   "perfect, ",     "absolutely, ", "of course, ", "certainly, ",
                                       "well, ",   "yes, ",         "yep, ",        "yeah, ",      "hmm, ",
                                       "alright then, "};
    bool sequenced = false;
    for (bool cut = true; cut;) {
        cut = false;
        for (const char * m : markers) {
            if (!starts_with(l, m)) continue;
            l = ltrim(l.substr(std::string(m).size()));
            sequenced = true;
            cut = true;
            break;
        }
        if (cut) continue;
        for (const char * m : preambles) {
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
    // "Now the main \"how to remove Activation Lock\" page and the proof-of-ownership route." names the next
    // thing to open without a verb. A sequencing marker followed by a determiner noun phrase and no finite verb
    // is a plan fragment, not a finished answer (seen live: the turn paused waiting for a nudge).
    if (sequenced && !has_finite_verb(l)) {
        static const char * det[] = {"the ",  "a ",   "an ",   "this ",  "that ",  "these ", "those ",
                                     "its ",  "my ",  "our ",  "your ",  "their ", "his ",   "her "};
        for (const char * d : det)
            if (starts_with(l, d)) return true;
    }
    // "Showing you a real example now." / "Searching for photos now..." announce work in the progressive form.
    // Only with an explicit "now" or a trailing ellipsis, so a description like "Running the tests shows ..." is
    // not mistaken for a plan.
    static const char * gerunds[] = {"showing ", "searching ", "looking ", "pulling ",  "fetching ", "grabbing ",
                                     "finding ", "getting ",   "checking ", "opening ", "loading ",  "starting ",
                                     "reading ", "running ",   "writing ",  "building ", "creating ", "generating ",
                                     "researching ", "browsing ", "reviewing ", "verifying ", "confirming "};
    std::string t = l;
    while (!t.empty() && (t.back() == '.' || t.back() == '!' || std::isspace((unsigned char) t.back()))) t.pop_back();
    bool soon = t.size() >= 4 && (t.compare(t.size() - 4, 4, " now") == 0 || l.find("...") != std::string::npos ||
                                  l.find("\u2026") != std::string::npos);
    if (soon)
        for (const char * g : gerunds)
            if (starts_with(l, g)) return true;
    // "I need lines 118-132 for module 3." / "I still need the helpers." name the next read without a verb; a
    // request to the user ("I need you to ...", "I need your OK") is an answer, not work.
    for (const char * a : {"i need ", "i still need ", "i still have to ", "i must ", "continuing with ", "moving on to "}) {
        if (!starts_with(l, a)) continue;
        std::string rest = l.substr(std::string(a).size());
        for (const char * u : {"you", "your", "the user", "permission", "approval", "confirmation", "more information"})
            if (starts_with(rest, u)) return false;
        return true;
    }
    return false;
}

// "I'll summarize: ...", "Let me tell you what changed." introduce the answer that follows; they are not work.
bool speech_act(const std::string & sentence) {
    std::string l = lower_apostrophes(ltrim(sentence));
    for (const char * a : {"let me ", "let's ", "let us ", "i'll ", "i will "}) {
        if (!starts_with(l, a)) continue;
        std::string rest = l.substr(std::string(a).size());
        for (const char * v : {"summarize", "summarise", "tell", "note", "explain", "say", "mention", "clarify",
                               "recap", "point out", "describe", "list what", "walk you", "be brief", "add that"})
            if (starts_with(rest, v)) return true;
    }
    return false;
}

// Any sentence of the last line counts: "Now let me spawn 6 workers, one per module. Each gets the rules." still
// announces work it has not started, even though its last sentence only describes the plan.
// "I'll hold here and wait for your next instruction." hands the turn back to the user; it is not a plan of work.
bool waits_on_user(const std::string & text) {
    std::string t;
    for (char ch : text) t += (char) std::tolower((unsigned char) ch);
    for (const char * k : {"wait for you", "wait for your", "waiting for you", "waiting for your", "hold here", "hold off",
                           "until you", "when you're ready", "when you are ready", "your go-ahead", "your go ahead",
                           "your next instruction", "your next message", "your call", "let me know when", "let me know if"})
        if (t.find(k) != std::string::npos) return true;
    return false;
}

bool announces_next_action(const std::string & text) {
    if (waits_on_user(last_sentence(last_line(text)))) return false;
    std::string line = last_line(text);
    while (!line.empty()) {
        std::string s = last_sentence(line);

        if (is_announcement(s) && (s == last_sentence(last_line(text)) || !speech_act(s))) return true;

        if (s.size() >= line.size()) break;
        line = line.substr(0, line.size() - s.size());
        while (!line.empty() && std::isspace((unsigned char) line.back())) line.pop_back();
        if (!line.empty()) line.pop_back();   // the sentence's own . ! or ?
        while (!line.empty() && std::isspace((unsigned char) line.back())) line.pop_back();
        if (!line.empty()) line += ".";      // keep a terminator so last_sentence splits the rest
    }
    return false;
}

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
    // "...Two holes remove 663 mm3. Now record net volume, compute mass, export, and render." ends on an order to
    // itself: the last sentence alone decides. (Seen live in Engineering.)
    if (finish_reason == "stop") {
        std::string last = c;
        while (!last.empty() && (last.back() == '.' || last.back() == '!' || std::isspace((unsigned char) last.back()))) last.pop_back();
        size_t cut = last.find_last_of(".!\n");
        if (cut != std::string::npos) {
            std::string tail = ltrim(last.substr(cut + 1));
            if (tail.size() <= 160 && tail.find('?') == std::string::npos && classify_reply(tail + ".", false, "stop") == ReplyKind::Announce &&
                !waits_on_user(tail))
                return ReplyKind::Announce;
        }
    }
    // A whole reply that is one short "-ing" sentence ("Extruding to the full arm length.") narrates the next step;
    // a real answer is not a lone progress line. Seen live in Engineering mode, where the turn ended there.
    if (finish_reason == "stop" && c.size() <= 160 && c.find('\n') == std::string::npos && c.find('?') == std::string::npos) {
        std::string w;
        for (char ch : c) { if (std::isalpha((unsigned char) ch)) w += (char) std::tolower((unsigned char) ch); else break; }
        size_t ends = 0;
        for (char ch : c) if (ch == '.' || ch == '!') ends++;
        bool pronounish = w == "nothing" || w == "everything" || w == "something" || w == "anything" || w == "thing" ||
                          w == "king" || w == "ring" || w == "string" || w == "morning" || w == "evening" || w == "building";
        // "Extruding to ...", "Adding the holes": the -ing word takes an object or a direction. "Running the tests shows
        // all 12 pass." has a finite verb, so it reports a result; "Looking good." describes, it does not act.
        std::vector<std::string> words;
        {
            std::string cur;
            for (char ch : c) {
                if (std::isalpha((unsigned char) ch) || ch == '\'') cur += (char) std::tolower((unsigned char) ch);
                else { if (!cur.empty()) words.push_back(cur); cur.clear(); }
            }
            if (!cur.empty()) words.push_back(cur);
        }
        static const char * objs[] = {"to", "the", "a", "an", "it", "them", "this", "that", "these", "those", "in", "on",
                                      "into", "onto", "with", "each", "both", "two", "three", "all", "up", "out", "its",
                                      "my", "your", "our", "now", "back", "over", "through", "for", "from"};
        static const char * finite[] = {"is", "are", "was", "were", "shows", "show", "showed", "confirms", "confirmed", "gives",
                                        "gave", "works", "worked", "passes", "passed", "looks", "means", "makes", "made",
                                        "returns", "returned", "fails", "failed", "takes", "took", "found", "finds", "has",
                                        "had", "matches", "matched", "proves", "proved", "says", "said", "done", "complete"};
        bool takes_object = false, has_finite = false;
        if (words.size() >= 2) for (const char * o : objs) if (words[1] == o) takes_object = true;
        for (size_t k = 1; k < words.size(); k++) for (const char * f : finite) if (words[k] == f) has_finite = true;
        if (w.size() > 4 && w.compare(w.size() - 3, 3, "ing") == 0 && !pronounish && ends <= 1 && takes_object &&
            !has_finite && !waits_on_user(c))
            return ReplyKind::Announce;
        // "Now create the artifact and render it." gives itself the next order and stops. Seen live in Engineering.
        if ((w == "now" || w == "next" || w == "then") && words.size() >= 3 && ends <= 1 && !has_finite && !waits_on_user(c)) {
            static const char * not_verb[] = {"that", "the", "it's", "its", "everything", "all", "you", "we're", "i'm", "there",
                                              "this", "both", "each", "your", "my"};
            bool verb = true;
            for (const char * nv : not_verb) if (words[1] == nv) verb = false;
            if (verb) return ReplyKind::Announce;
        }
    }
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
    st.empty_streak = kind == ReplyKind::Empty ? st.empty_streak + 1 : 0;
    if (kind == ReplyKind::ToolCalls) {
        d.action = Action::Execute;
        return d;
    }
    if (kind == ReplyKind::Final && (plan.open == 0 || st.wrapping)) {   // after a wrap-up, the summary is the end
        d.action = Action::Finish;
        return d;
    }
    if (kind == ReplyKind::Empty && st.wrapping) {
        d.action = Action::Pause;
        d.reason = "empty replies even after a wrap-up instruction";
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
    else if (kind == ReplyKind::TextToolCall)
        d.nudge = text_call_retry_nudge();
    else if (kind == ReplyKind::Empty && st.empty_streak >= kEmptyWrapUpAfter) {
        st.wrapping = true;
        d.nudge = wrap_up_message("Your last " + std::to_string(st.empty_streak) + " replies were empty.");
    } else
        d.nudge = "[harness] You did not call a tool. Either call the next tool or call done with a summary.";
    return d;
}

std::string text_call_retry_nudge() {
    return "[harness] You wrote a tool call as text, so nothing ran. Make a real tool call through the tool interface "
           "(do not type <tool_call> or JSON into the reply), or call done with a summary.";
}

namespace {

size_t skip_ws(const std::string & s, size_t i) {
    while (i < s.size() && std::isspace((unsigned char) s[i])) i++;
    return i;
}

// One value: a single leading and trailing newline are layout, not content.
std::string param_value(std::string v) {
    if (!v.empty() && v.front() == '\n') v.erase(0, 1);
    if (!v.empty() && v.back() == '\n') v.pop_back();
    if (!v.empty() && v.back() == '\r') v.pop_back();
    return v;
}

bool known_name(const std::vector<std::string> & known, const std::string & n) {
    return std::find(known.begin(), known.end(), n) != known.end();
}

// <function=NAME> blocks. Returns Malformed when any block is cut off, names an unknown tool or has a bad parameter.
TextCallParse parse_xml_calls(const std::string & c, const std::vector<std::string> & known, std::vector<TextCall> & out) {
    size_t pos = 0;
    int found = 0;
    while ((pos = c.find("<function=", pos)) != std::string::npos) {
        found++;
        size_t ns = pos + 10, ne = c.find('>', ns);
        if (ne == std::string::npos) return TextCallParse::Malformed;
        TextCall call;
        call.name = c.substr(ns, ne - ns);
        while (!call.name.empty() && std::isspace((unsigned char) call.name.back())) call.name.pop_back();
        size_t end = c.find("</function>", ne);
        if (end == std::string::npos || !known_name(known, call.name)) return TextCallParse::Malformed;
        std::string body = c.substr(ne + 1, end - ne - 1);
        size_t p = 0;
        while ((p = body.find("<parameter=", p)) != std::string::npos) {
            size_t ks = p + 11, ke = body.find('>', ks);
            if (ke == std::string::npos) return TextCallParse::Malformed;
            size_t ve = body.find("</parameter>", ke);
            if (ve == std::string::npos) return TextCallParse::Malformed;
            std::string key = body.substr(ks, ke - ks);
            if (key.empty()) return TextCallParse::Malformed;
            call.args[key] = param_value(body.substr(ke + 1, ve - ke - 1));
            p = ve + 12;
        }
        out.push_back(std::move(call));
        pos = end + 11;
    }
    return found ? TextCallParse::Ok : TextCallParse::None;
}

bool json_call(const json & j, const std::vector<std::string> & known, TextCall & call) {
    if (!j.is_object() || !j.contains("name") || !j["name"].is_string()) return false;
    call.name = j["name"].get<std::string>();
    if (!known_name(known, call.name)) return false;
    const char * key = j.contains("arguments") ? "arguments" : j.contains("parameters") ? "parameters" : nullptr;
    if (!key) {
        call.args = json::object();
        return true;
    }
    json a = j[key];
    if (a.is_string()) {   // arguments given as JSON text
        a = json::parse(a.get<std::string>(), nullptr, false);
    }
    if (!a.is_object()) return false;
    call.args = a;
    return true;
}

// The whole reply is a JSON call (or a list of them), optionally in a code fence.
TextCallParse parse_json_calls(const std::string & content, const std::vector<std::string> & known,
                               std::vector<TextCall> & out) {
    std::string c = rtrim(ltrim(content));
    if (c.rfind("```", 0) == 0 && c.size() > 6 && c.compare(c.size() - 3, 3, "```") == 0) {
        size_t nl = c.find('\n');
        if (nl == std::string::npos) return TextCallParse::None;
        c = rtrim(ltrim(c.substr(nl + 1, c.size() - 3 - nl - 1)));
    }
    if (c.empty() || (c[0] != '{' && c[0] != '[') || c.find("\"name\"") == std::string::npos) return TextCallParse::None;
    json j = json::parse(c, nullptr, false);
    if (j.is_discarded()) {
        // Invalid JSON that is shaped like a call (a name and arguments, nothing else in the reply): a broken call,
        // not an answer, so it gets the retry request instead of ending the turn.
        bool shaped = c.find("\"arguments\"") != std::string::npos || c.find("\"parameters\"") != std::string::npos;
        return shaped ? TextCallParse::Malformed : TextCallParse::None;
    }
    std::vector<json> items = j.is_array() ? j.get<std::vector<json>>() : std::vector<json>{j};
    if (items.empty()) return TextCallParse::None;
    std::vector<TextCall> calls;
    for (auto & it : items) {
        if (!it.is_object() || !it.contains("name")) return TextCallParse::None;   // some other JSON
        TextCall tc;
        if (!json_call(it, known, tc)) return TextCallParse::Malformed;
        calls.push_back(std::move(tc));
    }
    for (auto & tc : calls) out.push_back(std::move(tc));
    return TextCallParse::Ok;
}

}  // namespace

TextCallParse parse_text_tool_calls(const std::string & content, const std::vector<std::string> & known,
                                    const std::map<std::string, std::vector<std::string>> & params,
                                    std::vector<TextCall> & out) {
    out.clear();
    // A nameless call: <tool_call> opens straight into <parameter=KEY> tags whose keys are not tool names, and no
    // <function=...> follows. Pick the one tool whose parameters include every key used and insert its name.
    std::string named;
    for (size_t pos = content.find("<tool_call>"); pos != std::string::npos; pos = content.find("<tool_call>", pos + 1)) {
        size_t end = content.find("</tool_call>", pos);
        std::string block = content.substr(pos, end == std::string::npos ? std::string::npos : end - pos);
        if (block.find("<function=") != std::string::npos) continue;
        size_t t = pos + 11;
        while (t < content.size() && std::isspace((unsigned char) content[t])) t++;
        if (content.compare(t, 11, "<parameter=") != 0) continue;
        std::vector<std::string> keys;
        for (size_t k = block.find("<parameter="); k != std::string::npos; k = block.find("<parameter=", k + 1)) {
            size_t e = block.find('>', k);
            if (e == std::string::npos) break;
            keys.push_back(rtrim(ltrim(block.substr(k + 11, e - k - 11))));
        }
        if (keys.empty() || std::find(known.begin(), known.end(), keys[0]) != known.end()) continue;
        std::string pick;
        int fits = 0;
        for (auto & [tool, ps] : params) {
            if (std::find(known.begin(), known.end(), tool) == known.end()) continue;
            bool all = std::all_of(keys.begin(), keys.end(), [&](const std::string & k) {
                return std::find(ps.begin(), ps.end(), k) != ps.end();
            });
            if (all) { pick = tool; fits++; }
        }
        if (fits != 1) continue;
        if (named.empty()) named = content;
        size_t off = named.size() - content.size();
        named.insert(t + off, "<function=" + pick + ">\n");
    }
    if (!named.empty()) return parse_text_tool_calls(named, known, out);
    return parse_text_tool_calls(content, known, out);
}

TextCallParse parse_text_tool_calls(const std::string & content, const std::vector<std::string> & known,
                                    std::vector<TextCall> & out) {
    out.clear();
    // A common slip: "<tool_call> <parameter=memory_search> <parameter=query> ..." names the tool with
    // <parameter=> instead of <function=>. When that first tag after <tool_call> is a known tool, read it as one.
    std::string fixed;
    for (size_t pos = content.find("<tool_call>"); pos != std::string::npos; pos = content.find("<tool_call>", pos + 1)) {
        size_t t = pos + 11;
        while (t < content.size() && std::isspace((unsigned char) content[t])) t++;
        if (content.compare(t, 11, "<parameter=") != 0) {
            // Another slip, seen with no-argument tools: "<tool_call> <cad_state> </parameter> </function>" names the
            // tool as a bare tag. When that tag is a known tool, read it as <function=NAME> and close it if needed.
            if (t >= content.size() || content[t] != '<') continue;
            size_t e = content.find('>', t);
            if (e == std::string::npos) continue;
            std::string name = rtrim(ltrim(content.substr(t + 1, e - t - 1)));
            if (name.empty() || name.find_first_of(" =/") != std::string::npos) continue;
            if (std::find(known.begin(), known.end(), name) == known.end()) continue;
            if (fixed.empty()) fixed = content;
            size_t off = fixed.size() - content.size();
            fixed.replace(t + off, e - t + 1, "<function=" + name + ">");
            size_t close = fixed.find("</tool_call>", t + off), fend = fixed.find("</function>", t + off);
            if (fend == std::string::npos || (close != std::string::npos && fend > close))
                fixed.insert(close == std::string::npos ? fixed.size() : close, "</function>");
            continue;
        }
        size_t e = content.find('>', t);
        if (e == std::string::npos) continue;
        std::string name = rtrim(ltrim(content.substr(t + 11, e - t - 11)));
        if (std::find(known.begin(), known.end(), name) == known.end()) continue;
        if (fixed.empty()) fixed = content;
        size_t off = fixed.size() - content.size();
        fixed.replace(t + off, 11, "<function=");
        // the bogus tag has no </parameter> of its own; the call's </function> closes it
    }
    if (!fixed.empty() && fixed != content) return parse_text_tool_calls(fixed, known, out);
    if (content.find("<function=") != std::string::npos) {
        auto r = parse_xml_calls(content, known, out);
        if (r != TextCallParse::Ok) out.clear();
        return r;
    }
    if (content.find("<tool_call>") != std::string::npos) {
        // The wrapper without a usable <function=...> inside: JSON in the tags is the other common shape.
        size_t a = content.find("<tool_call>") + 11, b = content.find("</tool_call>", a);
        if (b == std::string::npos) return TextCallParse::Malformed;
        auto r = parse_json_calls(content.substr(a, b - a), known, out);
        if (r != TextCallParse::Ok) out.clear();
        return r == TextCallParse::Ok ? r : TextCallParse::Malformed;
    }
    auto r = parse_json_calls(content, known, out);
    if (r != TextCallParse::Ok) out.clear();
    return r;
}

void coerce_text_args(json & args, const json & schema) {
    if (!args.is_object() || !schema.is_object() || !schema.contains("properties")) return;
    for (auto & [k, v] : args.items()) {
        if (!v.is_string() || !schema["properties"].contains(k)) continue;
        const json & prop = schema["properties"][k];
        if (!prop.is_object() || !prop.contains("type")) continue;
        // "type" is a name or a list of names ({"type": ["string", "number"]}); a string is already fine when allowed
        std::string type;
        if (prop["type"].is_string()) type = prop["type"].get<std::string>();
        else if (prop["type"].is_array()) {
            bool str_ok = false;
            for (auto & t : prop["type"]) if (t.is_string()) { if (t == "string") str_ok = true; else if (type.empty()) type = t.get<std::string>(); }
            if (str_ok) continue;
        }
        std::string s = v.get<std::string>();
        if (type == "integer" || type == "number") {
            json n = json::parse(s, nullptr, false);
            if (n.is_number() && (type == "number" || n.is_number_integer())) v = n;
        } else if (type == "boolean") {
            if (s == "true" || s == "True") v = true;
            else if (s == "false" || s == "False") v = false;
        } else if (type == "array" || type == "object") {
            json n = json::parse(s, nullptr, false);
            if (!n.is_discarded() && (type == "array" ? n.is_array() : n.is_object())) v = n;
        }
    }
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
    // Two calls taking turns (A B A B A): the third A fires first, and clearing only A would let B's third turn
    // count as a second warning and pause the turn before the model could react to the first one.
    uint64_t partner = 0;
    {
        size_t n = hist_.size();
        if (e.call && n >= 5 && hist_[n - 3].call == e.call && hist_[n - 5].call == e.call &&
            hist_[n - 2].call && hist_[n - 2].call != e.call && hist_[n - 4].call == hist_[n - 2].call)
            partner = hist_[n - 2].call;
    }
    if (e.call)
        for (auto & h : hist_) calls += h.call == e.call;
    if (e.err)
        for (auto & h : hist_) errs += h.err == e.err;
    if (calls >= repeats_ || errs >= repeats_) {
        bool by_call = calls >= repeats_;
        r.kind = by_call ? (partner ? "cycle" : "call") : "error";
        r.tool = tool;
        r.count = by_call ? calls : errs;
        if (by_call && partner)
            for (auto & h : hist_)
                if (h.call == partner) r.other = h.tool;
        for (auto & h : hist_) {  // clear the evidence so the next warning needs fresh repeats
            if (by_call && (h.call == e.call || (partner && h.call == partner))) h.call = 0;
            if (!by_call && h.err == e.err) h.err = 0;
        }
        warnings_++;
        since_warn_ = 0;
        r.verdict = warnings_ >= pause_after_ ? Verdict::Pause : Verdict::Warn;
    }
    return r;
}

std::string loop_nudge(const LoopGuard::Result & r, bool can_spawn) {
    std::string what = r.kind == "cycle" ? "calls to " + r.tool + " and " + r.other + " taking turns"
                       : r.kind == "call" ? "the same " + r.tool + " call"
                                          : "the same " + r.tool + " error";
    std::string fix = r.kind == "error" ? "Re-read the error, change your approach, or "
                                        : "You already have that result: act on it, or ";
    return "[harness] You are looping: " + what + " " + std::to_string(r.count) + " times. Stop repeating it. " + fix +
           (can_spawn ? "spawn a worker with a fresh view of the problem, or " : "") +
           "stop and say what is blocking you (call done with that summary).";
}

std::string loop_pause_reason(const LoopGuard::Result & r) {
    if (r.kind == "cycle")
        return "looping: " + r.tool + " and " + r.other + " calls kept alternating after a warning";
    return "looping: the same " + r.tool + " " + (r.kind == "call" ? "call" : "error") +
           " kept repeating after a warning";
}

std::string wrap_up_message(const std::string & why) {
    return "[harness] " + why + " Stop working now. Reply in plain text, without a tool call: summarize what is done "
           "and what is left, then stop.";
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

bool deliver_blocks_call(const std::string & tool, const std::string & command) {
    if (tool == "web_fetch" || tool == "web_search") return true;
    if (tool != "shell") return false;
    std::string c;
    for (char ch : command) c += (char) std::tolower((unsigned char) ch);
    for (const char * w : {"curl", "wget", "sleep"}) {
        size_t at = 0;
        while ((at = c.find(w, at)) != std::string::npos) {
            bool l = at == 0 || !(std::isalnum((unsigned char) c[at - 1]) || c[at - 1] == '_' || c[at - 1] == '-');
            size_t e = at + std::string(w).size();
            bool r = e >= c.size() || !(std::isalnum((unsigned char) c[e]) || c[e] == '_');
            if (l && r) return true;
            at = e;
        }
    }
    return false;
}

bool deliver_satisfied(const std::string & tool, size_t content_chars) {
    return tool == "write_file" && content_chars >= 400;   // a real document, not a scratch note or a small edit
}

std::string deliver_blocked_message() {
    return "Refused: your workers are done, so the deliverable comes first. Write the report now from the worker "
           "results (write_file to the requested path, or give it as your answer). Fetching and verifying "
           "come after the report exists. Nothing ran.";
}

std::string workers_done_message(const std::string & reports) {
    return "[harness] All your workers have finished; their results follow. Your next step is to WRITE THE "
           "DELIVERABLE from these results: write_file to the requested path, or give it as your final answer if no "
           "path was requested. Do this before any verification, fetching or scraping. Mark what is unverified in "
           "the report instead of chasing it. You may refine the report afterwards.\n" + reports;
}

std::string report_now_message() {
    return "[harness] Your last turns produced nothing. Write the report now from what you have: write_file to the "
           "requested path, or give it as your final answer. Use the worker results already in this conversation, "
           "mark gaps as unverified, and do not start new research.";
}

bool empty_streak_observe(EmptyStreak & st, bool empty, int limit) {
    if (!empty) {
        st.count = 0;
        return false;
    }
    st.count++;
    if (limit > 0 && st.count >= limit) {
        st.count = 0;
        return true;
    }
    return false;
}

static std::string lower(const std::string & s) {
    std::string o = s;
    for (auto & c : o) c = (char) std::tolower((unsigned char) c);
    return o;
}

bool fetch_stub_text(const std::string & output) {
    std::string t = lower(ltrim(output));
    if (t.size() < 2000) {   // stubs are tiny; a long page that mentions a rate limit is real content
        for (const char * k : {"local_rate_limited", "rate limited", "rate limit exceeded", "too many requests",
                               "http 429", "error 429", "status 429", "status\":429"})
            if (t.find(k) != std::string::npos) return true;
        for (const char * k : {"enable javascript", "requires javascript", "javascript is required",
                               "javascript is disabled", "please turn javascript on"})
            if (t.find(k) != std::string::npos) return true;
    }
    return false;
}

bool fetch_result_failed(bool tool_ok, const std::string & output) {
    if (!tool_ok) return true;
    std::string t = rtrim(ltrim(output));
    if (t.size() < 80) return true;   // empty or an empty shell: a title and nothing else
    return fetch_stub_text(output);
}

std::string normalize_url(const std::string & url) {
    std::string u = url;
    if (size_t h = u.find('#'); h != std::string::npos) u.erase(h);
    size_t s = u.find("://");
    if (s != std::string::npos) {
        size_t e = u.find_first_of("/?", s + 3);
        std::string head = lower(u.substr(0, e == std::string::npos ? u.size() : e));
        u = head + (e == std::string::npos ? "" : u.substr(e));
    }
    while (!u.empty() && (u.back() == '/' || u.back() == '\'' || u.back() == '"')) u.pop_back();
    return u;
}

std::vector<std::string> fetch_urls_in_command(const std::string & command) {
    std::vector<std::string> out;
    std::string c = lower(command);
    auto has_word = [&](const char * w) {
        size_t at = 0, n = std::string(w).size();
        while ((at = c.find(w, at)) != std::string::npos) {
            bool l = at == 0 || !(std::isalnum((unsigned char) c[at - 1]) || c[at - 1] == '_' || c[at - 1] == '-');
            bool r = at + n >= c.size() || !(std::isalnum((unsigned char) c[at + n]) || c[at + n] == '_');
            if (l && r) return true;
            at += n;
        }
        return false;
    };
    if (!has_word("curl") && !has_word("wget")) return out;
    size_t at = 0;
    while (at < command.size()) {
        size_t h = c.find("http", at);   // c is the lowercased command (same length): schemes are case-insensitive
        if (h == std::string::npos) break;
        std::string rest = command.substr(h);
        if (c.compare(h, 7, "http://") != 0 && c.compare(h, 8, "https://") != 0) { at = h + 4; continue; }
        size_t e = rest.find_first_of(" \t\r\n'\"`;|&<>()\\");
        std::string u = rest.substr(0, e == std::string::npos ? rest.size() : e);
        if (!u.empty()) out.push_back(u);
        at = h + std::max<size_t>(u.size(), 4);
    }
    return out;
}

std::string UrlGuard::blocked(const std::string & url) const {
    if (cap_ <= 0) return "";
    auto it = fails_.find(normalize_url(url));
    if (it == fails_.end() || it->second < cap_) return "";
    return "Give up on this URL: " + url + " failed " + std::to_string(it->second) +
           " times (rate limit, error or a page that needs JavaScript). Nothing ran. Do not fetch it again, by "
           "web_fetch or by curl/wget in the shell. Use what the workers found, or another source, and "
           "mark this one as unverified in the report.";
}

void UrlGuard::record(const std::string & url, bool failed) {
    std::string k = normalize_url(url);
    if (failed) fails_[k]++;
    else fails_.erase(k);
}

int UrlGuard::failures(const std::string & url) const {
    auto it = fails_.find(normalize_url(url));
    return it == fails_.end() ? 0 : it->second;
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
