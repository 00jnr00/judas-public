#include "server/event_hub.h"

#include <chrono>

namespace judas {

uint64_t EventHub::push(const std::string & key, const std::string & type, const std::string & json,
                        const std::string & turn_event) {
    uint64_t id;
    {
        std::lock_guard lk(mu_);
        Ring & r = rings_[key];
        id = r.next_id++;
        if (turn_event == "turn_start" || turn_event == "turn") {
            // "turn": a lead event while no turn is marked open means a queued follow-up message started
            // without its own spawned event, so this event opens the turn
            if (!r.turn_open) { r.turn_open = true; r.turn_start = id; }
        } else if (turn_event == "turn_end") {
            r.turn_open = false;
        }
        r.bytes += json.size() + type.size() + 32;
        r.q.push_back({id, type, json});
        while (r.q.size() > 1 && (r.q.size() > max_events_ || r.bytes > max_bytes_)) {
            r.bytes -= r.q.front().json.size() + r.q.front().type.size() + 32;
            r.q.pop_front();
        }
        r.last_write = ++gen_;
        evict_rings_locked();
    }
    cv_.notify_all();
    return id;
}

// Keep the number of rings bounded: drop the least recently written ring that has no turn in progress.
void EventHub::evict_rings_locked() {
    while (rings_.size() > max_rings_) {
        auto victim = rings_.end();
        for (auto it = rings_.begin(); it != rings_.end(); ++it) {
            if (it->first == "*" || it->second.turn_open) continue;
            if (victim == rings_.end() || it->second.last_write < victim->second.last_write) victim = it;
        }
        if (victim == rings_.end()) return;   // everything is live: allow the overshoot
        rings_.erase(victim);
    }
}

EventHub::Batch EventHub::since(const std::string & key, uint64_t after, size_t max) const {
    Batch b;
    std::lock_guard lk(mu_);
    auto it = rings_.find(key);
    if (it == rings_.end()) return b;
    const Ring & r = it->second;
    b.head = r.next_id - 1;
    if (r.q.empty()) return b;
    if (after > b.head) {   // the client remembers ids from a ring that no longer exists
        after = 0;
        b.gap = true;
    }
    uint64_t first = r.q.front().id;
    if (after + 1 < first) b.gap = true;
    // ids are dense, so the entry for id N sits at index N - first
    size_t start = after + 1 > first ? (size_t) (after + 1 - first) : 0;
    for (size_t i = start; i < r.q.size() && b.entries.size() < max; i++) b.entries.push_back(r.q[i]);
    return b;
}

uint64_t EventHub::replay_floor(const std::string & key, bool * in_progress) const {
    std::lock_guard lk(mu_);
    if (in_progress) *in_progress = false;
    auto it = rings_.find(key);
    if (it == rings_.end()) return 0;
    const Ring & r = it->second;
    if (r.turn_open) {
        if (in_progress) *in_progress = true;
        return r.turn_start - 1;
    }
    return r.next_id - 1;
}

uint64_t EventHub::head(const std::string & key) const {
    std::lock_guard lk(mu_);
    auto it = rings_.find(key);
    return it == rings_.end() ? 0 : it->second.next_id - 1;
}

size_t EventHub::ring_count() const {
    std::lock_guard lk(mu_);
    return rings_.size();
}

size_t EventHub::ring_events(const std::string & key) const {
    std::lock_guard lk(mu_);
    auto it = rings_.find(key);
    return it == rings_.end() ? 0 : it->second.q.size();
}

void EventHub::drop(const std::string & key) {
    std::lock_guard lk(mu_);
    rings_.erase(key);
}

uint64_t EventHub::generation() const {
    std::lock_guard lk(mu_);
    return gen_;
}

uint64_t EventHub::wait(uint64_t seen, int timeout_ms) const {
    std::unique_lock lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms), [&] { return gen_ != seen; });
    return gen_;
}

void EventHub::wake() {
    {
        std::lock_guard lk(mu_);
        gen_++;
    }
    cv_.notify_all();
}

}  // namespace judas
