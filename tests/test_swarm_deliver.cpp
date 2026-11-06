// Swarm lead delivery rules with fake tool results: deliver first, per-URL retry cap, empty-turn streak.
#include "agent/policy.h"
#include "test.h"

using namespace judas;

static void test_deliver_blocks() {
    CHECK(deliver_blocks_call("web_fetch", ""));
    CHECK(deliver_blocks_call("web_search", ""));
    CHECK(deliver_blocks_call("shell", "curl -s https://shop.example/p/1 | head"));
    CHECK(deliver_blocks_call("shell", "for u in a b; do wget $u; sleep 5; done"));
    CHECK(deliver_blocks_call("shell", "sleep 30"));
    CHECK(!deliver_blocks_call("shell", "mkdir -p out && ls out"));
    CHECK(!deliver_blocks_call("shell", "echo recurl"));
    CHECK(!deliver_blocks_call("write_file", ""));
    CHECK(!deliver_blocks_call("read_file", ""));
    CHECK(!deliver_blocks_call("done", ""));
    CHECK(deliver_satisfied("write_file", 2000));
    CHECK(!deliver_satisfied("write_file", 50));     // a scratch note
    CHECK(!deliver_satisfied("edit_file", 5000));    // an edit of some other file
    CHECK(!deliver_satisfied("shell", 5000));
    std::string m = workers_done_message("[{\"agent\":\"w1\"}]");
    CHECK(m.find("WRITE THE DELIVERABLE") != std::string::npos);
    CHECK(m.find("before any verification") != std::string::npos);
    CHECK(m.find("w1") != std::string::npos);
}

static void test_fetch_failure() {
    // fake results
    CHECK(fetch_result_failed(false, "web_fetch: timed out"));
    CHECK(fetch_result_failed(true, ""));
    CHECK(fetch_result_failed(true, "{\"error\":\"local_rate_limited\"}"));
    CHECK(fetch_result_failed(true, "Too Many Requests"));
    CHECK(fetch_result_failed(true, "<html><title>Shop</title></html>"));   // empty JS shell
    CHECK(fetch_result_failed(true, std::string(300, ' ') + "Please enable JavaScript to view this page. " + std::string(100, 'x')));
    CHECK(!fetch_result_failed(true, std::string(3000, 'a') + " rate limited " + std::string(500, 'b')));   // real long page
    CHECK(!fetch_result_failed(true, "Product: Widget 3000. Price 19.99 EUR. In stock at 12 stores. Ships in two days from the central warehouse."));
    CHECK(fetch_stub_text("local_rate_limited"));
    CHECK(!fetch_stub_text("saved 1 file"));
}

static void test_command_urls() {
    auto u = fetch_urls_in_command("curl -sL 'https://shop.example/p/1?x=2' -o a.html && sleep 3 && wget http://b.example/q");
    CHECK(u.size() == 2);
    if (u.size() == 2) { CHECK(u[0] == "https://shop.example/p/1?x=2"); CHECK(u[1] == "http://b.example/q"); }
    CHECK(fetch_urls_in_command("echo https://a.example").empty());     // no curl or wget
    CHECK(fetch_urls_in_command("curl --version").empty());
    auto up = fetch_urls_in_command("curl HTTP://Shop.example/p");
    CHECK(up.size() == 1 && up[0] == "HTTP://Shop.example/p");
    CHECK(normalize_url("HTTPS://Shop.Example/p/1/#top") == normalize_url("https://shop.example/p/1"));
}

static void test_url_guard() {
    UrlGuard g(2);
    const std::string url = "https://shop.example/p/1";
    CHECK(g.blocked(url).empty());
    g.record(url, true);
    CHECK(g.blocked(url).empty());
    g.record(url + "#reviews", true);   // the same page, tried again by another route
    std::string msg = g.blocked(url);
    CHECK(!msg.empty());
    CHECK(msg.find("Give up on this URL") == 0);
    CHECK(msg.find("curl") != std::string::npos);
    CHECK(g.blocked("https://shop.example/p/2").empty());   // other URLs are unaffected
    g.record(url, false);                                   // a success clears the count
    CHECK(g.blocked(url).empty());

    UrlGuard off(0);
    for (int i = 0; i < 5; i++) off.record(url, true);
    CHECK(off.blocked(url).empty());

    // web_fetch failures and shell curl failures of the same URL share the count
    UrlGuard s(2);
    s.record(url, fetch_result_failed(true, "{\"error\":\"local_rate_limited\"}"));
    for (auto & x : fetch_urls_in_command("curl -s " + url)) s.record(x, fetch_stub_text("local_rate_limited"));
    CHECK(!s.blocked(url).empty());
}

static void test_empty_streak() {
    EmptyStreak st;
    CHECK(!empty_streak_observe(st, true, 2));
    CHECK(empty_streak_observe(st, true, 2));      // second in a row
    CHECK(st.count == 0);
    CHECK(!empty_streak_observe(st, true, 2));
    CHECK(!empty_streak_observe(st, false, 2));    // a real reply resets
    CHECK(!empty_streak_observe(st, true, 2));
    CHECK(empty_streak_observe(st, true, 2));
    EmptyStreak off;
    for (int i = 0; i < 6; i++) CHECK(!empty_streak_observe(off, true, 0));
    CHECK(report_now_message().find("Write the report now") != std::string::npos);
}

int main() {
    test_deliver_blocks();
    test_fetch_failure();
    test_command_urls();
    test_url_guard();
    test_empty_streak();
    TEST_MAIN_END();
}
