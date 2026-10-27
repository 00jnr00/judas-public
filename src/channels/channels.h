// Channels: push a short message out (ntfy topic, or a user command on stdin). Used by the heartbeat and by
// long-run/task alerts. Credentials come from config/env, never inline here.
#pragma once
#include <string>
#include <vector>
#include "core/config.h"
namespace judas {
struct ChannelResult { bool ok = true; std::string error; };
class Channels {
public:
    explicit Channels(const ChannelsConfig & cfg) : cfg_(cfg) {}
    // Send title+body to the named channel (or the default if name is empty). title may be ignored by a kind.
    ChannelResult send(const std::string & name, const std::string & title, const std::string & body) const;
    std::vector<std::string> names() const;
private:
    ChannelsConfig cfg_;
};
}
