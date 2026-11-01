// Compactor tests: fake counter (len/4), fake summarizer, fake recall refs.
#include "context/compactor.h"
#include "test.h"

using namespace judas;

static int count(const std::string & s) { return static_cast<int>(s.size() / 4); }
static std::string summ(const std::vector<ChatMessage> & v) { return "SUMMARY(" + std::to_string(v.size()) + " msgs)"; }

static ChatMessage msg(const std::string & role, const std::string & c) {
    ChatMessage m;
    m.role = role;
    m.content = c;
    return m;
}

// system, user1, then n x (assistant, tool(big), user)
static std::vector<ChatMessage> convo(int n) {
    std::vector<ChatMessage> v;
    v.push_back(msg("system", "you are judas"));
    v.push_back(msg("user", "first task"));
    for (int i = 0; i < n; i++) {
        v.push_back(msg("assistant", "calling tool " + std::to_string(i)));
        v.push_back(msg("tool", "line one of output " + std::to_string(i) + "\n" + std::string(2000, 'x')));
        v.push_back(msg("user", "next question " + std::to_string(i)));
    }
    return v;
}

static bool same(const ChatMessage & a, const ChatMessage & b) {
    return a.role == b.role && a.content == b.content;
}

static std::string ref_all(const ChatMessage &) { return "m1"; }
static std::string ref_none(const ChatMessage &) { return ""; }

int main() {
    ContextConfig cfg;  // trigger 0.75, target 0.45, keep 3
    auto h = convo(12);  // ~ 12 * 505 tokens
    int total = 0;
    for (auto & m : h) total += count(m.content);

    // Under trigger: unchanged.
    {
        auto r = compact(h, cfg, total * 2, count, summ, ref_all);
        CHECK(!r.changed);
        CHECK(r.messages.size() == h.size());
        for (size_t i = 0; i < h.size(); i++) CHECK(same(r.messages[i], h[i]));
    }
    // Over trigger: pointer-drop.
    {
        int cap = total * 10 / 9;  // total is ~90% of cap
        auto r = compact(h, cfg, cap, count, summ, ref_all);
        CHECK(r.changed);
        CHECK(r.after_tokens < r.before_tokens);
        CHECK(r.after_tokens <= cap * cfg.target_pct);
        CHECK(r.dropped_tool_results > 0 && r.recalled > 0);
        CHECK(r.messages.size() == h.size());  // no summary needed
        CHECK(same(r.messages[0], h[0]));
        CHECK(same(r.messages[1], h[1]));
        bool found = false;
        for (auto & m : r.messages)
            if (m.role == "tool" && m.content.find("recall id=m1") != std::string::npos) found = true;
        CHECK(found);
        // Last 3 turns verbatim.
        size_t n = h.size();
        for (size_t i = n - 9; i < n; i++) CHECK(same(r.messages[i], h[i]));
    }
    // No recall: summarization.
    {
        int cap = total * 10 / 9;
        auto r = compact(h, cfg, cap, count, summ, ref_none);
        CHECK(r.changed);
        CHECK(r.dropped_tool_results == 0);
        CHECK(r.summarized_turns > 0);
        CHECK(r.after_tokens < r.before_tokens);
        bool found = false;
        for (auto & m : r.messages)
            if (m.role == "system" && m.content.find("[earlier conversation summarized]") == 0) found = true;
        CHECK(found);
        CHECK(same(r.messages[0], h[0]));
        CHECK(same(r.messages[1], h[1]));
        for (int k = 1; k <= 9; k++)
            CHECK(same(r.messages[r.messages.size() - k], h[h.size() - k]));
    }
    // keep_turns = 1 and 5 respected.
    for (int keep : {1, 5}) {
        ContextConfig c = cfg;
        c.keep_turns = keep;
        auto r = compact(h, c, total * 10 / 9, count, summ, ref_none);
        CHECK(r.changed);
        for (int k = 1; k <= keep * 3; k++)
            CHECK(same(r.messages[r.messages.size() - k], h[h.size() - k]));
    }
    // Empty, tiny, all-protected.
    {
        auto r = compact({}, cfg, 100, count, summ, ref_all);
        CHECK(!r.changed && r.messages.empty());
        std::vector<ChatMessage> tiny = {msg("user", std::string(4000, 'y'))};
        auto r2 = compact(tiny, cfg, 100, count, summ, ref_all);
        CHECK(!r2.changed && r2.messages.size() == 1);
        // Under trigger stays unchanged regardless of turn count (the tail is token-budgeted,
        // not turn-counted, so a small history is left alone only because it is below trigger).
        auto small = convo(2);
        int st = 0;
        for (auto & m : small) st += count(m.content);
        auto r3 = compact(small, cfg, st * 2, count, summ, ref_all);  // total ~50% of cap, under 0.75 trigger
        CHECK(!r3.changed && r3.messages.size() == small.size());
    }
    // Digests name the call that produced them; a summary keeps a recall index of the folded tool results.
    {
        std::vector<ChatMessage> v;
        v.push_back(msg("system", "you are judas"));
        v.push_back(msg("user", "first task"));
        for (int i = 0; i < 12; i++) {
            ChatMessage a = msg("assistant", "reading part " + std::to_string(i));
            a.tool_calls.push_back({"c" + std::to_string(i), "read_file",
                                    "{\"path\":\"SPEC.md\",\n\"offset\":" + std::to_string(i * 80) + "}"});
            v.push_back(a);
            ChatMessage t = msg("tool", "    1\tline\n" + std::string(2000, 'x'));
            t.tool_call_id = "c" + std::to_string(i);
            t.tool_name = "read_file";
            v.push_back(t);
            v.push_back(msg("user", "next " + std::to_string(i)));
        }
        int vt = 0;
        for (auto & m : v) vt += count(m.content);
        auto by_id = [](const ChatMessage & m) { return m.tool_call_id; };
        auto r = compact(v, cfg, vt * 10 / 9, count, summ, by_id);
        CHECK(r.changed);
        bool labeled = false;
        for (auto & m : r.messages)
            if (m.role == "tool" && m.content.find("[read_file {\"path\":\"SPEC.md\", \"offset\":0}]") == 0 &&
                m.content.find("recall id=c0") != std::string::npos)
                labeled = true;
        CHECK(labeled);
        // Small tool outputs (under the pointer-drop size) with big assistant turns force summarization; the
        // folded tool results stay fetchable, so the summary lists them in a recall index.
        std::vector<ChatMessage> w;
        w.push_back(msg("system", "you are judas"));
        w.push_back(msg("user", "first task"));
        for (int i = 0; i < 12; i++) {
            ChatMessage a = msg("assistant", "thinking aloud " + std::string(2000, 'z'));
            a.tool_calls.push_back({"c" + std::to_string(i), "read_file", "{\"path\":\"SPEC.md\"}"});
            w.push_back(a);
            ChatMessage t = msg("tool", "short output " + std::to_string(i));
            t.tool_call_id = "c" + std::to_string(i);
            w.push_back(t);
            w.push_back(msg("user", "next " + std::to_string(i)));
        }
        int wt = 0;
        for (auto & m : w) wt += count(m.content);
        auto r2 = compact(w, cfg, wt * 10 / 9, count, summ, by_id);
        bool indexed = false;
        for (auto & m : r2.messages)
            if (m.role == "system" && m.content.find("Recall index") != std::string::npos &&
                m.content.find("- c0: read_file {\"path\":\"SPEC.md\"}") != std::string::npos)
                indexed = true;
        CHECK(r2.summarized_turns > 0);
        CHECK(indexed);
    }
    // Rule pinning: the first message names SPEC.md; its read (rule on line 1, big filler) is dropped, and the
    // rule survives verbatim, once, in one pinned message that later passes keep.
    {
        const std::string rule = "     1\tRULE: every function returns early on bad input, no exceptions";
        std::vector<ChatMessage> w;
        w.push_back(msg("system", "you are judas"));
        w.push_back(msg("user", "Build the thing and follow SPEC.md exactly."));
        ChatMessage a0 = msg("assistant", "reading the spec");
        a0.tool_calls.push_back({"s0", "read_file", "{\"path\":\"/work/proj/SPEC.md\"}"});
        w.push_back(a0);
        ChatMessage spec = msg("tool", rule + "\n" + std::string(6000, 'f'));
        spec.tool_call_id = "s0";
        spec.tool_name = "read_file";
        w.push_back(spec);
        auto more = [&](int from, int to) {
            for (int i = from; i < to; i++) {
                ChatMessage a = msg("assistant", "step " + std::to_string(i));
                a.tool_calls.push_back({"c" + std::to_string(i), "read_file", "{\"path\":\"src/f" + std::to_string(i) + ".cpp\"}"});
                w.push_back(a);
                ChatMessage t = msg("tool", "line one " + std::to_string(i) + "\n" + std::string(2000, 'x'));
                t.tool_call_id = "c" + std::to_string(i);
                t.tool_name = "read_file";
                w.push_back(t);
            }
        };
        more(0, 14);
        const std::vector<ChatMessage> w0 = w;
        auto by_id = [](const ChatMessage & m) { return m.tool_call_id; };
        auto occurrences = [&](const std::vector<ChatMessage> & v) {
            int n = 0;
            for (auto & m : v)
                for (size_t p = m.content.find("RULE: every function"); p != std::string::npos;
                     p = m.content.find("RULE: every function", p + 1))
                    n++;
            return n;
        };
        int wt = 0;
        for (auto & m : w) wt += count(m.content);
        auto r = compact(w, cfg, wt * 10 / 9, count, summ, by_id);
        CHECK(r.changed);
        CHECK(occurrences(r.messages) == 1);
        CHECK(r.messages.size() > 3 && is_pinned_message(r.messages[2]));  // system, user, pinned
        CHECK(r.messages[2].content.find(rule) != std::string::npos);
        // Pinned text is capped: well under the 6000-char file, never over pin_tokens.
        CHECK(count(r.messages[2].content) <= cfg.pin_tokens + 40);
        // Grow the conversation and compact again, forcing summaries: still exactly one copy, still second.
        w = r.messages;
        more(14, 40);
        wt = 0;
        for (auto & m : w) wt += count(m.content);
        auto r2 = compact(w, cfg, wt * 10 / 9, count, summ, ref_none);  // no recall ids: summaries
        CHECK(r2.changed && r2.summarized_turns > 0);
        CHECK(occurrences(r2.messages) == 1);
        CHECK(is_pinned_message(r2.messages[2]));
        int pins = 0;
        for (auto & m : r2.messages) pins += is_pinned_message(m);
        CHECK(pins == 1);
        // Re-reading the file replaces the entry (latest read wins) instead of adding a second one.
        ChatMessage a1 = msg("assistant", "reread");
        a1.tool_calls.push_back({"s1", "read_file", "{\"path\":\"SPEC.md\"}"});
        w = r2.messages;
        w.push_back(a1);
        ChatMessage spec2 = msg("tool", "     1\tRULE: every function version two\n" + std::string(6000, 'g'));
        spec2.tool_call_id = "s1";
        spec2.tool_name = "read_file";
        w.push_back(spec2);
        more(40, 60);
        wt = 0;
        for (auto & m : w) wt += count(m.content);
        auto r3 = compact(w, cfg, wt * 10 / 9, count, summ, by_id);
        CHECK(r3.changed);
        CHECK(r3.messages[2].content.find("RULE: every function version two") != std::string::npos);
        CHECK(r3.messages[2].content.find("bad input, no exceptions") != std::string::npos);  // /work/proj/SPEC.md stays: different path string
        // pin_total = 0 switches pinning off.
        ContextConfig off = cfg;
        off.pin_total = 0;
        int w0t = 0;
        for (auto & m : w0) w0t += count(m.content);
        auto r4 = compact(w0, off, w0t * 10 / 9, count, summ, by_id);
        CHECK(r4.changed);
        for (auto & m : r4.messages) CHECK(!is_pinned_message(m));
        // Files nobody named are not pinned.
        std::vector<ChatMessage> u = w0;
        u[1].content = "Build the thing.";
        auto r5 = compact(u, cfg, w0t * 10 / 9, count, summ, by_id);
        CHECK(r5.changed);
        for (auto & m : r5.messages) CHECK(!is_pinned_message(m));
    }
    // AGENTS.md is pinned without being named.
    {
        std::vector<ChatMessage> w;
        w.push_back(msg("user", "do the task"));
        ChatMessage a0 = msg("assistant", "rules");
        a0.tool_calls.push_back({"g0", "read_file", "{\"path\":\"AGENTS.md\"}"});
        w.push_back(a0);
        ChatMessage t0 = msg("tool", "     1\tALWAYS WRAP IN TRY\n" + std::string(4000, 'q'));
        t0.tool_call_id = "g0";
        t0.tool_name = "read_file";
        w.push_back(t0);
        for (int i = 0; i < 12; i++) {
            w.push_back(msg("assistant", std::string(800, 'a')));
            w.push_back(msg("user", std::string(800, 'u')));
        }
        int wt = 0;
        for (auto & m : w) wt += count(m.content);
        auto r = compact(w, cfg, wt * 10 / 9, count, summ, [](const ChatMessage & m) { return m.tool_call_id; });
        CHECK(r.changed && is_pinned_message(r.messages[1]));
        CHECK(r.messages[1].content.find("ALWAYS WRAP IN TRY") != std::string::npos);
    }
    TEST_MAIN_END();
}
