// Context-borrow admission: soft cap, hard cap, pool fit, victim choice and the generation cap. Pure logic.
#include "core/config.h"
#include "engine/sched.h"
#include "test.h"

using namespace judas;

static KvLive live(int id, int prio, long long adm, int tokens, bool borrow = false, int pending = 0) {
    KvLive l;
    l.id = id;
    l.priority = prio;
    l.admitted = adm;
    l.tokens = tokens;
    l.pending = pending;
    l.borrow = borrow;
    return l;
}

static void test_fits_soft_cap() {
    KvBudget b(10000, 500);
    std::vector<KvLive> none;
    auto d = decide_ctx_admission(4000, 5000, 8000, b, none);
    CHECK(d.allowed && d.start_now());
    CHECK(!d.borrow);
    CHECK(d.limit == 5000);  // under the soft cap, ctx_per_agent still rules
    // The hard cap does not change anything for a prompt that already fits the soft one.
    auto off = decide_ctx_admission(4000, 5000, 0, b, none);
    CHECK(off.allowed && off.start_now() && !off.borrow && off.limit == 5000);
}

static void test_borrows_when_pool_free() {
    KvBudget b(10000, 500);
    std::vector<KvLive> none;
    auto d = decide_ctx_admission(6000, 5000, 8000, b, none);
    CHECK(d.allowed && d.start_now());
    CHECK(d.borrow);
    CHECK(d.limit == 8000);  // the hard cap applies while borrowing
    // Boundary: one live request plus this prompt and both reserves exactly fill the pool.
    std::vector<KvLive> one = {live(0, 1, 1, 3000)};
    auto edge = decide_ctx_admission(6000, 5000, 8000, b, one);  // 3500 + 6000 + 500 == 10000
    CHECK(edge.allowed && edge.start_now() && edge.borrow);
    auto over = decide_ctx_admission(6001, 5000, 8000, b, one);
    CHECK(over.allowed && !over.start_now() && over.borrow);
}

static void test_refused_when_pool_busy() {
    KvBudget b(10000, 500);
    std::vector<KvLive> busy = {live(0, 1, 1, 7000)};  // 7500 committed, 2500 free
    auto d = decide_ctx_admission(6000, 5000, 8000, b, busy);
    CHECK(d.allowed);       // the caps permit it
    CHECK(d.borrow);
    CHECK(!d.start_now());  // but the pool cannot hold it without preempting the live request
    CHECK(!d.fits);
    // A prompt under the soft cap also waits for the pool, as before.
    auto small = decide_ctx_admission(3000, 5000, 8000, b, busy);
    CHECK(small.allowed && !small.fits && !small.start_now() && !small.borrow);
}

static void test_preempted_first() {
    KvBudget b(10000, 0);
    // The borrowing request gives way even when it is the lead and the others are background.
    std::vector<KvLive> lv = {live(0, 0, 1, 100, true), live(1, 2, 2, 100), live(2, 1, 3, 100)};
    CHECK(b.pick_victim(lv) == 0);
    // Among borrowing requests the existing priority and admission order apply.
    lv = {live(0, 1, 1, 100, true), live(1, 1, 9, 100, true), live(2, 0, 3, 100, true)};
    CHECK(b.pick_victim(lv) == 1);
    // No borrowing: today's victim choice is unchanged.
    lv = {live(0, 0, 1, 100), live(1, 2, 2, 100), live(2, 1, 3, 100)};
    CHECK(b.pick_victim(lv) == 1);
    // A borrowing request alone still gives way (the engine then reports the pool failure).
    CHECK(b.pick_victim({live(0, 1, 1, 100, true)}) == 0);
}

static void test_max_tokens_cap() {
    // Borrowing: room is measured against the hard cap, not the soft one.
    CHECK(ctx_generation_cap(6000, 8000, 0, 8192) == 2000);
    CHECK(ctx_generation_cap(6000, 8000, 9000, 8192) == 2000);  // hard cap wins over the request
    CHECK(ctx_generation_cap(6000, 8000, 500, 8192) == 500);    // a smaller request still wins
    // Normal path: unchanged, the soft cap and the engine default still apply.
    CHECK(ctx_generation_cap(4000, 5000, 0, 8192) == 1000);
    CHECK(ctx_generation_cap(4000, 5000, 300, 8192) == 300);
    CHECK(ctx_generation_cap(4000, 5000, 0, 700) == 700);
    CHECK(ctx_generation_cap(5000, 5000, 0, 8192) == 0);
}

static void test_default_off() {
    KvBudget b(10000, 500);
    std::vector<KvLive> none;
    // ctx_per_agent_max = 0: a prompt over the soft cap fails exactly as before.
    auto d = decide_ctx_admission(6000, 5000, 0, b, none);
    CHECK(!d.allowed);
    CHECK(!d.borrow && !d.start_now());
    CHECK(d.limit == 5000);
    // A hard cap equal to the soft one is also off, not a way to admit the same prompt.
    auto same = decide_ctx_admission(6000, 5000, 5000, b, none);
    CHECK(!same.allowed);
    // Over the hard cap is impossible even with an empty pool.
    auto over = decide_ctx_admission(8000, 5000, 8000, b, none);
    CHECK(!over.allowed);
    CHECK(over.limit == 8000);
}

static void test_effective_window() {
    ModelConfig m;
    m.ctx_total = 98304;
    m.ctx_per_agent = 24576;
    ContextConfig c;  // window = auto
    // A lone agent gets the whole pool; the share shrinks as agents go live, never below ctx_per_agent.
    CHECK(effective_ctx_cap(m, c, 1) == 98304);
    CHECK(effective_ctx_cap(m, c, 2) == 49152);
    CHECK(effective_ctx_cap(m, c, 3) == 32768);
    CHECK(effective_ctx_cap(m, c, 4) == 24576);
    CHECK(effective_ctx_cap(m, c, 8) == 24576);
    CHECK(effective_ctx_cap(m, c, 0) == 98304);
    // ctx_per_agent_max is the ceiling when set.
    m.ctx_per_agent_max = 60000;
    CHECK(effective_ctx_cap(m, c, 1) == 60000);
    CHECK(effective_ctx_cap(m, c, 2) == 49152);
    CHECK(ctx_hard_cap(m, c) == 60000);
    // fixed: the old static behavior, and no implicit borrowing without ctx_per_agent_max.
    c.window = "fixed";
    CHECK(effective_ctx_cap(m, c, 1) == 24576);
    CHECK(ctx_hard_cap(m, c) == 60000);
    m.ctx_per_agent_max = 0;
    CHECK(ctx_hard_cap(m, c) == 0);
    c.window = "auto";
    CHECK(ctx_hard_cap(m, c) == 98304);
    // The engine admits a lone agent's big prompt under the auto hard cap, and refuses one over the pool.
    KvBudget b(98304, 500);
    std::vector<KvLive> none;
    CHECK(decide_ctx_admission(80000, m.ctx_per_agent, ctx_hard_cap(m, c), b, none).start_now());
}

int main() {
    test_effective_window();
    test_fits_soft_cap();
    test_borrows_when_pool_free();
    test_refused_when_pool_busy();
    test_preempted_first();
    test_max_tokens_cap();
    test_default_off();
    TEST_MAIN_END();
}
