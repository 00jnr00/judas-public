// Auto-resume decision logic (pure) and the progress rule.
#include "agent/autoresume.h"
#include "test.h"

using namespace judas;

static AutoResumeInput base() {
    AutoResumeInput in;
    in.enabled = true;
    in.status = "paused";
    in.paused_for_ms = 31000;
    in.delay_s = 30;
    in.max_per_day = 20;
    return in;
}

int main() {
    using A = AutoResumeAction;
    CHECK(decide_auto_resume(base()) == A::Resume);

    auto off = base(); off.enabled = false;
    CHECK(decide_auto_resume(off) == A::None);

    for (const char * st : {"error", "killed", "running", "done", "idle", "failed"}) {
        auto in = base(); in.status = st;
        CHECK(decide_auto_resume(in) == A::None);   // never anything but paused
    }

    auto early = base(); early.paused_for_ms = 29999;
    CHECK(decide_auto_resume(early) == A::Wait);
    auto edge = base(); edge.paused_for_ms = 30000;
    CHECK(decide_auto_resume(edge) == A::Resume);
    auto zero = base(); zero.delay_s = 0; zero.paused_for_ms = 0;
    CHECK(decide_auto_resume(zero) == A::Resume);

    auto cap = base(); cap.attempts_today = 19;
    CHECK(decide_auto_resume(cap) == A::Resume);
    cap.attempts_today = 20;
    CHECK(decide_auto_resume(cap) == A::None);
    auto nomax = base(); nomax.max_per_day = 0;
    CHECK(decide_auto_resume(nomax) == A::None);

    auto stall = base(); stall.stalled = kAutoResumeStallLimit - 1;
    CHECK(decide_auto_resume(stall) == A::Resume);
    stall.stalled = kAutoResumeStallLimit;
    CHECK(decide_auto_resume(stall) == A::None);
    CHECK(kAutoResumeStallLimit == 3);

    CHECK(!resume_made_progress(5, 5, 3, 3));
    CHECK(resume_made_progress(5, 6, 3, 3));    // a file-changing tool call
    CHECK(resume_made_progress(5, 5, 3, 2));    // a plan item ticked
    CHECK(!resume_made_progress(5, 5, 2, 3));   // plan grew: not progress
    TEST_MAIN_END();
}
