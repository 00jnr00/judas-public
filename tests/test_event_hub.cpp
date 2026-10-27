// EventHub: ids, replay, turn tracking, bounds, wake-ups.
#include <chrono>
#include <thread>

#include "server/event_hub.h"
#include "test.h"

using namespace judas;

static void test_ids_and_since() {
    EventHub h;
    CHECK(h.head("s") == 0);
    CHECK(h.since("s", 0).entries.empty());
    for (int i = 1; i <= 5; i++) CHECK(h.push("s", "delta", "{\"n\":" + std::to_string(i) + "}") == (uint64_t) i);
    auto all = h.since("s", 0);
    REQUIRE(all.entries.size() == 5);
    CHECK(!all.gap && all.head == 5);
    CHECK(all.entries[0].id == 1 && all.entries[4].id == 5);
    auto tail = h.since("s", 3);
    REQUIRE(tail.entries.size() == 2);
    CHECK(tail.entries[0].id == 4 && tail.entries[0].json == "{\"n\":4}");
    CHECK(h.since("s", 5).entries.empty());
    CHECK(h.since("s", 2, 1).entries.size() == 1);   // max
    // rings are independent
    CHECK(h.push("t", "x", "{}") == 1);
}

static void test_turn_replay() {
    EventHub h;
    bool in = true;
    CHECK(h.replay_floor("s", &in) == 0 && !in);
    h.push("s", "spawned", "{\"a\":1}", "turn_start");       // id 1: an old turn
    h.push("s", "delta", "{}", "turn");                       // 2
    h.push("s", "done", "{}", "turn_end");                    // 3
    CHECK(h.replay_floor("s", &in) == 3 && !in);              // nothing in progress: live from the head
    h.push("s", "spawned", "{\"a\":2}", "turn_start");        // 4
    h.push("s", "reasoning", "{\"text\":\"hm\"}", "turn");    // 5
    h.push("s", "delta", "{\"text\":\"Hel\"}", "turn");       // 6
    h.push("s", "spawned", "{\"a\":3}", "turn_start");        // 7: a queued follow-up must not move the start
    h.push("s", "delta", "{\"text\":\"lo\"}", "turn");        // 8
    uint64_t fl = h.replay_floor("s", &in);
    CHECK(in && fl == 3);
    auto b = h.since("s", fl);
    REQUIRE(b.entries.size() == 5);
    CHECK(b.entries[0].id == 4 && b.entries[0].type == "spawned");
    std::string text;
    for (auto & e : b.entries)
        if (e.type == "delta") text += e.json;
    CHECK(text == "{\"text\":\"Hel\"}{\"text\":\"lo\"}");   // the partial message, in order
    h.push("s", "done", "{}", "turn_end");
    CHECK(!(h.replay_floor("s", &in), in));
    // a lead event with no spawned (queued input restarted the loop) opens a turn
    h.push("s", "delta", "{}", "turn");
    fl = h.replay_floor("s", &in);
    CHECK(in && fl == h.head("s") - 1);
}

static void test_bounds() {
    EventHub h(10, 1 << 20, 4);   // 10 events per ring
    for (int i = 0; i < 25; i++) h.push("s", "delta", "{}");
    CHECK(h.ring_events("s") == 10);
    auto b = h.since("s", 0);
    CHECK(b.gap);
    REQUIRE(b.entries.size() == 10);
    CHECK(b.entries.front().id == 16 && b.entries.back().id == 25);
    CHECK(!h.since("s", 20).gap);   // contiguous with what is kept
    CHECK(h.since("s", 14).gap);

    EventHub bytes(1000, 400, 4);   // byte cap
    std::string big(100, 'x');
    for (int i = 0; i < 20; i++) bytes.push("s", "delta", big);
    CHECK(bytes.ring_events("s") <= 3);
    CHECK(bytes.ring_events("s") >= 1);

    // ring count: idle rings are evicted oldest first, a turn in progress and "*" are kept
    EventHub r(100, 1 << 20, 3);
    r.push("*", "x", "{}");
    r.push("live", "spawned", "{}", "turn_start");
    r.push("a", "x", "{}");
    r.push("b", "x", "{}");
    r.push("c", "x", "{}");
    CHECK(r.ring_count() <= 3);
    CHECK(r.head("*") == 1);
    CHECK(r.head("live") == 1);
    CHECK(r.head("a") == 0);   // evicted
    CHECK(r.head("c") == 1);

    // client remembers ids from a ring that was dropped and restarted: whole ring, flagged
    EventHub d;
    d.push("s", "x", "{}");
    d.drop("s");
    d.push("s", "y", "{}");
    auto reset = d.since("s", 40);
    CHECK(reset.gap && reset.entries.size() == 1);
}

static void test_wait() {
    EventHub h;
    uint64_t g = h.generation();
    auto t0 = std::chrono::steady_clock::now();
    h.wait(g, 50);   // nothing pushed: times out
    CHECK(std::chrono::steady_clock::now() - t0 >= std::chrono::milliseconds(40));
    std::thread th([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        h.push("s", "x", "{}");
    });
    t0 = std::chrono::steady_clock::now();
    uint64_t g2 = h.wait(g, 5000);
    CHECK(g2 != g);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(2000));
    th.join();
    // a push between generation() and wait() is not lost
    uint64_t g3 = h.generation();
    h.push("s", "x", "{}");
    t0 = std::chrono::steady_clock::now();
    h.wait(g3, 5000);
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(500));
}

int main() {
    test_ids_and_since();
    test_turn_replay();
    test_bounds();
    test_wait();
    std::printf("test_event_hub: done\n");
    TEST_MAIN_END();
}
