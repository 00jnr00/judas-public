// Pure scheduling bookkeeping for the engine, kept free of libllama so it can be unit-tested:
//   KvBudget       admission control and victim choice for the unified KV pool
//   choose_prefill which request gets a prefill chunk in this decode step, and how big it is
//   TicketQueue    fair priority queue in front of a one-at-a-time resource (the remote engine)
#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <vector>

namespace judas {

// One live (admitted) request as the budget sees it.
struct KvLive {
    int id = 0;
    int priority = 1;            // 0 = foreground, larger = less important
    long long admitted = 0;      // admission order, larger = more recent
    int tokens = 0;              // tokens in its sequence's KV right now
    int pending = 0;             // prompt tokens it still has to prefill
    bool borrow = false;         // uses ctx_per_agent_max, over the soft ctx_per_agent cap
};

// An idle sequence: cached tokens nobody is using, evictable at any time.
struct KvIdle {
    int id = 0;
    long long last_used = 0;
    int tokens = 0;
};

class KvBudget {
public:
    KvBudget(int total, int reserve) : total_(std::max(1, total)), reserve_(std::max(0, reserve)) {}

    int total() const { return total_; }
    int reserve() const { return reserve_; }

    // KV the live requests hold or will hold once their prefill is done, plus a generation reserve each.
    int committed(const std::vector<KvLive> & live) const {
        long long n = 0;
        for (const auto & l : live) n += (long long) l.tokens + l.pending + reserve_;
        return (int) std::min<long long>(n, INT32_MAX);
    }
    // Tokens a new request could still claim (idle caches count as free, they are evictable). May be negative.
    int free_tokens(const std::vector<KvLive> & live) const { return total_ - committed(live); }

    // A prompt that cannot fit even in an empty pool (with one token left to generate) never will.
    bool can_ever_fit(int prompt) const { return prompt < total_; }

    // May a request with `prompt` tokens (full prompt, reused prefix included) start now? With nothing live
    // it always may if it can ever fit: the reserve is a guard against crowding out others, not a hard floor.
    bool fits(const std::vector<KvLive> & live, int prompt) const {
        if (!can_ever_fit(prompt)) return false;
        if (live.empty()) return true;
        return (long long) committed(live) + prompt + reserve_ <= total_;
    }

    // Who gives way when the pool runs out: a borrowing request first (it uses the pool allowance past the
    // soft cap), then the least important request (largest priority number), and among equals the most recently
    // admitted one (it has the least sunk work). -1 when nothing is live.
    int pick_victim(const std::vector<KvLive> & live) const {
        auto worse = [](const KvLive & a, const KvLive & b) {
            if (a.borrow != b.borrow) return a.borrow;
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.admitted > b.admitted;
        };
        int v = -1;
        for (int i = 0; i < (int) live.size(); i++)
            if (v < 0 || worse(live[i], live[v])) v = i;
        return v;
    }

    // Which idle caches to drop, least recently used first, until `need` tokens are freed (or none are left).
    static std::vector<int> idle_to_evict(std::vector<KvIdle> idle, int need) {
        std::sort(idle.begin(), idle.end(), [](const KvIdle & a, const KvIdle & b) { return a.last_used < b.last_used; });
        std::vector<int> out;
        long long freed = 0;
        for (const auto & q : idle) {
            if (freed >= need) break;
            if (q.tokens <= 0) continue;
            out.push_back(q.id);
            freed += q.tokens;
        }
        return out;
    }

private:
    int total_, reserve_;
};

// Prefill fairness while a long document and generating agents share the GPU. Without a limit every decode
// step also carries a prefill chunk, so the generators pay the chunk's time on every token. Duty spreads the
// chunks over steps; alone on the GPU prefill still runs every step with a full chunk.
struct PrefillPolicy {
    double duty = 1.0;       // fraction of busy decode steps that may carry a prefill chunk
    int chunk_alone = 256;   // tokens per chunk when nothing is generating
    int chunk_busy = 0;      // tokens per chunk while others generate; 0 follows chunk_alone
    int max_wait_ms = 2000;  // a priority-0 prefill waiting longer than this skips the duty pattern
};

// One request that still has prompt tokens to prefill, as the step chooser sees it.
struct PrefillCandidate {
    int id = 0;              // engine sequence id, unique among live requests
    int priority = 1;        // 0 = foreground, larger = less important
    long long waited_ms = 0; // time since this request last got a chunk (not since it started)
};

struct PrefillChoice {
    bool run = false;        // carry a prefill chunk in this step
    int id = -1;             // candidate to prefill, -1 when none
    int chunk = 0;           // tokens to prefill
    bool guarded = false;    // starvation guard overrode the duty pattern
};

// Pick the prefill for one decode step. Lowest priority number wins, earliest in the list on a tie (the
// engine passes sequence order, so equal priorities stay stable). With no generators prefill runs every step
// with a full chunk. With generators the duty pattern decides: over any run of steps the number of prefills
// grows by `duty` per step, so 0.25 gives one step in four. A priority-0 request that has waited past
// max_wait_ms always runs, whatever the pattern says.
inline PrefillChoice choose_prefill(const std::vector<PrefillCandidate> & waiting, bool generating, long long step,
                                    const PrefillPolicy & policy) {
    PrefillChoice c;
    if (waiting.empty()) return c;
    size_t best = 0;
    for (size_t i = 1; i < waiting.size(); i++)
        if (waiting[i].priority < waiting[best].priority) best = i;
    c.id = waiting[best].id;
    int busy_chunk = policy.chunk_busy > 0 ? policy.chunk_busy : policy.chunk_alone;
    if (!generating) {
        c.run = true;
        c.chunk = policy.chunk_alone;
        return c;
    }
    if (waiting[best].priority == 0 && waiting[best].waited_ms > policy.max_wait_ms) {
        c.run = true;
        c.chunk = busy_chunk;
        c.guarded = true;
        return c;
    }
    double duty = std::clamp(policy.duty, 0.0, 1.0);
    // Count the whole steps the duty fraction has accumulated so far: a new one is allowed this step.
    double before = std::floor((double) step * duty);
    double after = std::floor((double) (step + 1) * duty);
    if (after > before) {
        c.run = true;
        c.chunk = busy_chunk;
    }
    return c;
}

// Context admission for one request. `soft` is ctx_per_agent (the normal cap and compaction trigger), `hard` is
// ctx_per_agent_max (0 or anything not above soft = borrowing off). A prompt under the soft cap is allowed;
// over it, a request may "borrow" from the free pool up to the hard cap, but only while KvBudget::fits says the
// live requests plus the reserve still fit, so borrowing never evicts anyone. `fits` false with `allowed` true
// means wait (pool busy); false with `allowed` false means this prompt can never start.
struct CtxDecision {
    bool allowed = false;  // the configured caps permit the prompt at all
    bool borrow = false;   // prompt is over the soft cap and uses the hard cap
    bool fits = false;     // the pool can hold it now without preempting anyone
    int limit = 0;         // effective per-request context cap (soft, or hard while borrowing)

    bool start_now() const { return allowed && fits; }
};

inline CtxDecision decide_ctx_admission(int prompt, int soft, int hard, const KvBudget & kv,
                                        const std::vector<KvLive> & live) {
    CtxDecision d;
    d.limit = soft;
    if (prompt < soft) {
        d.allowed = true;
        d.fits = kv.fits(live, prompt);
        return d;
    }
    if (hard <= soft) return d;  // borrowing off
    d.borrow = true;
    d.limit = hard;
    if (prompt >= hard) return d;  // over the hard cap
    d.allowed = true;
    d.fits = kv.fits(live, prompt);  // someone live would have to give way: not a borrow
    return d;
}

// Generation room left under the effective context cap. `requested` <= 0 means the engine default.
inline int ctx_generation_cap(int prompt, int limit, int requested, int fallback) {
    int cap = requested > 0 ? requested : fallback;
    return std::max(0, std::min(cap, limit - prompt));
}

// Hands out one slot at a time in priority order (lower number first), FIFO within a priority. A waiter that
// is cancelled while queued leaves without ever holding the slot.
class TicketQueue {
public:
    // Blocks until it is this caller's turn. Returns false if `cancel` was raised while waiting (no slot held).
    bool acquire(int priority, const std::atomic<bool> * cancel = nullptr) {
        std::unique_lock lk(mu_);
        Waiter me{priority, ++next_};
        waiters_.push_back(me);
        for (;;) {
            if (cancel && cancel->load()) {
                remove(me.ticket);
                cv_.notify_all();
                return false;
            }
            if (!busy_ && front().ticket == me.ticket) {
                remove(me.ticket);
                busy_ = true;
                return true;
            }
            cv_.wait_for(lk, std::chrono::milliseconds(50));
        }
    }
    void release() {
        {
            std::lock_guard lk(mu_);
            busy_ = false;
        }
        cv_.notify_all();
    }
    size_t waiting() const {
        std::lock_guard lk(mu_);
        return waiters_.size();
    }

private:
    struct Waiter {
        int priority;
        unsigned long long ticket;
    };
    const Waiter & front() const {
        return *std::min_element(waiters_.begin(), waiters_.end(), [](const Waiter & a, const Waiter & b) {
            return a.priority != b.priority ? a.priority < b.priority : a.ticket < b.ticket;
        });
    }
    void remove(unsigned long long t) {
        std::erase_if(waiters_, [&](const Waiter & w) { return w.ticket == t; });
    }
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::vector<Waiter> waiters_;
    unsigned long long next_ = 0;
    bool busy_ = false;
};

}  // namespace judas
