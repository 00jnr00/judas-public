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
    TEST_MAIN_END();
}
