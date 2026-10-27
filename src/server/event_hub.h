// EventHub: bounded, replayable event rings for the SSE endpoints. One ring per key (a session id, or "*" for
// the global feed). Every pushed event gets a monotonically increasing id (per ring) and is kept as a
// pre-serialized JSON string. Subscribers do not own queues: they remember the last id they sent and pull
// newer entries from the ring, so a slow client can never grow memory; it just misses evicted entries (gap).
// Rings are bounded by event count and bytes, and the number of rings is bounded too (least recently
// written, idle rings go first).
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace judas {

class EventHub {
public:
    struct Entry {
        uint64_t id = 0;
        std::string type;
        std::string json;   // serialized event object
    };
    struct Batch {
        std::vector<Entry> entries;   // ascending ids, all > the requested `after` (unless the ring was reset)
        uint64_t head = 0;            // newest id in the ring (0 if empty)
        bool gap = false;             // entries between `after` and the first returned one were evicted
    };

    explicit EventHub(size_t max_events = 20000, size_t max_bytes = 8u << 20, size_t max_rings = 16)
        : max_events_(max_events), max_bytes_(max_bytes), max_rings_(max_rings) {}

    // Append an event and return its id. `turn_event` drives the in-progress turn tracking of a session ring:
    // "turn_start" for a lead's spawned, "turn_end" for a lead's done, "turn" for any other lead event,
    // "" for everything else.
    uint64_t push(const std::string & key, const std::string & type, const std::string & json,
                  const std::string & turn_event = "");

    // Entries with id > after (at most `max`). If `after` is beyond the ring head (the ring was dropped and
    // restarted), the whole ring is returned with gap set.
    Batch since(const std::string & key, uint64_t after, size_t max = 512) const;

    // The id to pass to since() so that a replay starts at the beginning of the turn now in progress
    // (start id - 1), or the ring head if no turn is in progress. *in_progress tells which.
    uint64_t replay_floor(const std::string & key, bool * in_progress = nullptr) const;
    uint64_t head(const std::string & key) const;
    size_t ring_count() const;
    size_t ring_events(const std::string & key) const;

    void drop(const std::string & key);   // session deleted

    // Blocks until a push happens after generation `seen` or the timeout passes. Returns the generation.
    uint64_t generation() const;
    uint64_t wait(uint64_t seen, int timeout_ms) const;
    void wake();   // wake all waiters (shutdown)

private:
    struct Ring {
        std::deque<Entry> q;
        size_t bytes = 0;
        uint64_t next_id = 1;
        bool turn_open = false;
        uint64_t turn_start = 0;
        uint64_t last_write = 0;   // generation of the last push, for eviction order
    };
    void evict_rings_locked();

    size_t max_events_, max_bytes_, max_rings_;
    mutable std::mutex mu_;
    mutable std::condition_variable cv_;
    std::map<std::string, Ring> rings_;
    uint64_t gen_ = 0;
};

}  // namespace judas
