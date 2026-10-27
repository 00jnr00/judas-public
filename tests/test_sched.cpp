// KV admission, victim choice and the remote ticket queue: pure bookkeeping, no model.
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

#include "engine/sched.h"
#include "test.h"

using namespace judas;

static KvLive live(int id, int prio, long long adm, int tokens, int pending = 0) {
    KvLive l;
    l.id = id;
    l.priority = prio;
    l.admitted = adm;
    l.tokens = tokens;
    l.pending = pending;
    return l;
}

static void test_admission() {
    KvBudget b(1000, 100);
    CHECK(b.can_ever_fit(999) && !b.can_ever_fit(1000) && !b.can_ever_fit(5000));
    std::vector<KvLive> none;
    CHECK(b.fits(none, 950));           // empty pool: only has to fit at all, reserve is waived
    CHECK(!b.fits(none, 1000));
    std::vector<KvLive> one = {live(0, 1, 1, 400)};
    CHECK(b.committed(one) == 500);
    CHECK(b.free_tokens(one) == 500);
    CHECK(b.fits(one, 400));            // 500 + 400 + 100 == 1000
    CHECK(!b.fits(one, 401));
    // a request that is still prefilling counts for its whole prompt
    std::vector<KvLive> pre = {live(0, 1, 1, 100, 300)};
    CHECK(b.committed(pre) == 500);
    // many live requests can overcommit the free count; it goes negative, never wraps
    std::vector<KvLive> many;
    for (int i = 0; i < 8; i++) many.push_back(live(i, 1, i, 200));
    CHECK(b.free_tokens(many) < 0 && !b.fits(many, 1));
    // reserve 0 behaves as a plain token count
    KvBudget z(100, 0);
    CHECK(z.fits({live(0, 1, 1, 60)}, 40) && !z.fits({live(0, 1, 1, 60)}, 41));
}

static void test_victim() {
    KvBudget b(1000, 0);
    CHECK(b.pick_victim({}) == -1);
    // lowest importance first
    CHECK(b.pick_victim({live(0, 0, 1, 10), live(1, 2, 2, 10), live(2, 1, 3, 10)}) == 1);
    // among equal priorities the most recently admitted one gives way
    CHECK(b.pick_victim({live(0, 1, 5, 10), live(1, 1, 9, 10), live(2, 1, 7, 10)}) == 1);
    // the lead (priority 0) is only chosen when it is the only one
    CHECK(b.pick_victim({live(0, 0, 1, 10)}) == 0);
    CHECK(b.pick_victim({live(0, 0, 9, 10), live(1, 1, 1, 10)}) == 1);
}

static void test_evict() {
    std::vector<KvIdle> idle = {{0, 30, 100}, {1, 10, 50}, {2, 20, 0}, {3, 5, 200}};
    auto v = KvBudget::idle_to_evict(idle, 250);
    CHECK((v == std::vector<int>{3, 1}));
    CHECK((KvBudget::idle_to_evict(idle, 120) == std::vector<int>{3}));       // oldest first, skipping the empty one, stops once 250 freed
    CHECK(KvBudget::idle_to_evict(idle, 0).empty());
    CHECK(KvBudget::idle_to_evict(idle, 10000).size() == 3);
    CHECK(KvBudget::idle_to_evict({}, 10).empty());
}

// Simulate the engine's loop: admit in priority order, preempt on overflow, and check nobody is dropped.
static void test_preempt_flow() {
    KvBudget b(1000, 50);
    std::vector<KvLive> lv = {live(0, 0, 1, 300), live(1, 1, 2, 300), live(2, 1, 3, 300)};
    int used = 900;
    int steps = 0;
    while (used + (int) lv.size() > 1000 && steps++ < 10) {  // each live request needs one more token per step
        int v = b.pick_victim(lv);
        REQUIRE(v >= 0);
        used -= lv[v].tokens;
        lv.erase(lv.begin() + v);
    }
    CHECK(lv.size() == 3);               // 903 tokens still fit, nobody preempted
    lv.push_back(live(3, 2, 4, 150));
    used += 150;
    for (auto & l : lv) l.tokens += 10, used += 10;
    // 1090 > 1000: victim is the background request, then (if still over) the newest normal one
    int v = b.pick_victim(lv);
    CHECK(lv[v].id == 3);
    used -= lv[v].tokens;
    lv.erase(lv.begin() + v);
    CHECK(used <= 1000);
    CHECK(b.pick_victim(lv) >= 0 && lv[b.pick_victim(lv)].id == 2);
}

static void test_ticket_order() {
    TicketQueue q;
    CHECK(q.acquire(1));                        // holder
    std::mutex m;
    std::vector<int> order;
    auto waiter = [&](int prio, int label) {
        q.acquire(prio);
        {
            std::lock_guard lk(m);
            order.push_back(label);
        }
        q.release();
    };
    std::thread a(waiter, 2, 20), b(waiter, 1, 10);
    while (q.waiting() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::thread c(waiter, 0, 0);                // lead arrives last, goes first
    while (q.waiting() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    q.release();
    a.join();
    b.join();
    c.join();
    CHECK((order == std::vector<int>{0, 10, 20}));
    CHECK(q.waiting() == 0);
}

static void test_ticket_fifo_and_cancel() {
    TicketQueue q;
    CHECK(q.acquire(1));
    std::mutex m;
    std::vector<int> order;
    std::atomic<bool> cancel{false};
    bool got = true;
    std::thread canceled([&] { got = q.acquire(1, &cancel); });
    while (q.waiting() < 1) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::thread w1([&] { q.acquire(1); { std::lock_guard lk(m); order.push_back(1); } q.release(); });
    while (q.waiting() < 2) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::thread w2([&] { q.acquire(1); { std::lock_guard lk(m); order.push_back(2); } q.release(); });
    while (q.waiting() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    cancel = true;                              // queued request leaves while the slot is still held
    canceled.join();
    CHECK(!got);
    CHECK(q.waiting() == 2);
    q.release();
    w1.join();
    w2.join();
    CHECK((order == std::vector<int>{1, 2}));   // equal priority stays FIFO
    CHECK(q.acquire(1));                        // the cancelled ticket did not wedge the queue
    q.release();
}

static void test_prefill_duty() {
    PrefillPolicy p;
    p.duty = 0.25;
    p.chunk_alone = 512;
    p.chunk_busy = 64;
    p.max_wait_ms = 2000;
    std::vector<PrefillCandidate> one = {{3, 1, 0}};

    // While someone generates, one step in four carries a prefill chunk of the busy size.
    int runs = 0;
    for (long long step = 0; step < 8; step++) {
        auto c = choose_prefill(one, true, step, p);
        runs += c.run;
        if (c.run) {
            CHECK(c.id == 3 && c.chunk == 64 && !c.guarded);
        }
    }
    CHECK(runs == 2);
    p.duty = 0.5;
    runs = 0;
    for (long long step = 0; step < 10; step++) runs += choose_prefill(one, true, step, p).run;
    CHECK(runs == 5);
    p.duty = 1.0;
    for (long long step = 0; step < 5; step++) {
        auto c = choose_prefill(one, true, step, p);
        CHECK(c.run && c.chunk == 64);
    }

    // Alone on the GPU, prefill runs every step at full batch; a busy chunk of 0 follows the full size.
    p.duty = 0.25;
    auto alone = choose_prefill(one, false, 0, p);
    CHECK(alone.run && alone.chunk == 512 && !alone.guarded);
    p.chunk_busy = 0;
    CHECK(choose_prefill(one, true, 3, p).chunk == 512);
    CHECK(!choose_prefill({}, true, 3, p).run);

    // Lowest priority number wins, equal priorities keep list order.
    std::vector<PrefillCandidate> many = {{1, 2, 0}, {2, 0, 0}, {3, 1, 0}, {4, 1, 99}};
    CHECK(choose_prefill(many, false, 0, p).id == 2);
    CHECK(choose_prefill({{1, 1, 0}, {2, 1, 0}}, false, 0, p).id == 1);
}

static void test_prefill_starvation() {
    PrefillPolicy p;
    p.duty = 0.25;
    p.chunk_alone = 512;
    p.chunk_busy = 64;
    p.max_wait_ms = 2000;
    // Step 0 is a duty-off step, but a foreground prefill past the limit runs anyway.
    auto c = choose_prefill({{7, 0, 2001}}, true, 0, p);
    CHECK(c.run && c.id == 7 && c.chunk == 64 && c.guarded);
    // Exactly the limit is not "more than", and only priority 0 has the guard.
    CHECK(!choose_prefill({{7, 0, 2000}}, true, 0, p).run);
    CHECK(!choose_prefill({{7, 1, 999999}}, true, 0, p).run);
    // The guard still prefers a foreground prefill over a background one.
    auto both = choose_prefill({{1, 2, 999999}, {2, 0, 3000}}, true, 0, p);
    CHECK(both.run && both.id == 2 && both.guarded);
}

int main() {
    test_admission();
    test_victim();
    test_evict();
    test_preempt_flow();
    test_ticket_order();
    test_ticket_fifo_and_cancel();
    test_prefill_duty();
    test_prefill_starvation();
    TEST_MAIN_END();
}
