// Reverse proxy for the Learn visual tutor ([learn] visual_url, e.g. LearnHarness on 127.0.0.1:8024).
//
// The tutor is a Next.js app that links to its assets and API with absolute paths (/_next/..., /api/...), and it
// cannot be rebuilt with a base path, so it cannot live under a sub-path of the main server. Instead Judas
// listens on one extra port (on the same bind addresses) and forwards every request there to the tutor's origin.
// Only that origin is ever contacted. Bodies and responses are streamed (SSE works).
#pragma once
#include <functional>
#include <string>

#include "cpp-httplib/httplib.h"

namespace judas {

struct LearnConfig;

// "http://host:port" part of a URL, or "" when there is no scheme.
std::string visual_origin(const std::string & url);

// The port the proxy listens on: 0 when off (no http visual_url, visual_proxy_port = 0, or it would collide).
int visual_proxy_port(const LearnConfig & learn, int main_port);

// Install the forwarding handlers on svr. host_ok vets the Host header (DNS rebinding), as on the main server.
void visual_proxy_setup(httplib::Server & svr, const std::string & upstream_origin,
                        std::function<bool(const std::string &)> host_ok);

}  // namespace judas
