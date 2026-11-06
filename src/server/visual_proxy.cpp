#include "server/visual_proxy.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <deque>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "core/config.h"

namespace judas {

std::string visual_origin(const std::string & u) {
    size_t a = u.find("://");
    if (a == std::string::npos) return "";
    size_t e = u.find('/', a + 3);
    return u.substr(0, e == std::string::npos ? std::string::npos : e);
}

int visual_proxy_port(const LearnConfig & L, int main_port) {
    if (!L.show_visual) return 0;
    std::string origin = visual_origin(L.visual_url);
    if (origin.rfind("http://", 0) != 0 || L.visual_proxy_port == 0) return 0;
    int port = L.visual_proxy_port;
    if (port < 0) {
        size_t c = origin.rfind(':');
        int up = c != std::string::npos && c > 6 ? std::atoi(origin.c_str() + c + 1) : 80;
        port = up + 100;
    }
    if (port <= 0 || port > 65535 || port == main_port) return 0;
    return port;
}

namespace {

// One upstream exchange. The upstream client runs on its own thread and fills this; the handler thread waits for
// the response head, then streams the queued body chunks to the browser. A slow browser holds the upstream back
// (bounded queue) and a vanished browser cancels the upstream read.
struct Pipe {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> q;
    size_t qbytes = 0;
    bool head = false, done = false, cancel = false;
    int status = 502;
    httplib::Headers headers;
    std::string error;
};

bool ieq(const std::string & a, const char * b) {
    size_t n = std::char_traits<char>::length(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (std::tolower((unsigned char) a[i]) != std::tolower((unsigned char) b[i])) return false;
    return true;
}

bool hop_by_hop(const std::string & k) {
    return ieq(k, "connection") || ieq(k, "keep-alive") || ieq(k, "transfer-encoding") || ieq(k, "content-length") ||
           ieq(k, "host") || ieq(k, "upgrade") || ieq(k, "te") || ieq(k, "trailer") || ieq(k, "proxy-authorization");
}

// The page is served from this proxy; the tutor sees its own origin so same-origin checks (Origin, Referer) pass.
std::string to_upstream(const std::string & v, const std::string & self, const std::string & origin) {
    if (!self.empty() && v.compare(0, self.size(), self) == 0) return origin + v.substr(self.size());
    return v;
}

void forward(const httplib::Request & req, httplib::Response & res, const std::string & origin, std::string body,
             const std::string & content_type) {
    auto pipe = std::make_shared<Pipe>();
    httplib::Request up;
    up.method = req.method;
    up.path = req.target;
    std::string self = "http://" + req.get_header_value("Host");
    for (auto & h : req.headers) {
        if (hop_by_hop(h.first)) continue;
        if (ieq(h.first, "content-type") && !content_type.empty()) continue;
        std::string v = h.second;
        if (ieq(h.first, "origin") || ieq(h.first, "referer")) v = to_upstream(v, self, origin);
        up.headers.emplace(h.first, v);
    }
    if (!content_type.empty()) up.headers.emplace("Content-Type", content_type);
    up.body = std::move(body);
    up.response_handler = [pipe](const httplib::Response & r) {
        std::lock_guard<std::mutex> lk(pipe->m);
        pipe->status = r.status;
        pipe->headers = r.headers;
        pipe->head = true;
        pipe->cv.notify_all();
        return true;
    };
    up.content_receiver = [pipe](const char * d, size_t n, size_t, size_t) {
        std::unique_lock<std::mutex> lk(pipe->m);
        pipe->cv.wait(lk, [&] { return pipe->cancel || pipe->qbytes < (1u << 20); });
        if (pipe->cancel) return false;
        pipe->q.emplace_back(d, n);
        pipe->qbytes += n;
        pipe->cv.notify_all();
        return true;
    };
    std::thread([pipe, origin, up]() mutable {
        httplib::Client c(origin);
        c.set_connection_timeout(2, 0);
        c.set_read_timeout(3600, 0);   // an SSE stream may idle for a long time
        c.set_decompress(false);       // pass bytes and Content-Encoding through untouched
        httplib::Response ur;
        httplib::Error err = httplib::Error::Success;
        bool ok = c.send(up, ur, err);
        std::lock_guard<std::mutex> lk(pipe->m);
        if (!pipe->head) {
            pipe->head = true;
            pipe->status = ok ? ur.status : 502;
            if (!ok) pipe->error = std::string("the visual tutor did not answer: ") + httplib::to_string(err);
        }
        pipe->done = true;
        pipe->cv.notify_all();
    }).detach();

    {
        std::unique_lock<std::mutex> lk(pipe->m);
        if (!pipe->cv.wait_for(lk, std::chrono::seconds(120), [&] { return pipe->head; })) {
            pipe->cancel = true;
            pipe->cv.notify_all();
            res.status = 504;
            res.set_content("the visual tutor took too long to answer", "text/plain");
            return;
        }
    }
    std::string ctype = "application/octet-stream";
    {
        std::lock_guard<std::mutex> lk(pipe->m);
        res.status = pipe->status;
        if (!pipe->error.empty()) {
            res.set_content(pipe->error, "text/plain");
            return;
        }
        for (auto & h : pipe->headers) {
            if (hop_by_hop(h.first)) continue;
            if (ieq(h.first, "content-type")) { ctype = h.second; continue; }
            std::string v = h.second;
            if (ieq(h.first, "location") && v.compare(0, origin.size(), origin) == 0) v = v.substr(origin.size());
            if (ieq(h.first, "content-security-policy") || ieq(h.first, "x-frame-options")) continue;   // framed by Judas pages
            res.headers.emplace(h.first, v);
        }
    }
    if (!res.has_header("Cache-Control")) res.set_header("Cache-Control", "no-store");
    res.set_chunked_content_provider(
        ctype,
        [pipe](size_t, httplib::DataSink & sink) {
            std::string chunk;
            {
                std::unique_lock<std::mutex> lk(pipe->m);
                pipe->cv.wait(lk, [&] { return !pipe->q.empty() || pipe->done; });
                if (pipe->q.empty()) {
                    lk.unlock();
                    sink.done();
                    return true;
                }
                chunk = std::move(pipe->q.front());
                pipe->q.pop_front();
                pipe->qbytes -= chunk.size();
                pipe->cv.notify_all();
            }
            if (!sink.write(chunk.data(), chunk.size())) {
                std::lock_guard<std::mutex> lk(pipe->m);
                pipe->cancel = true;
                pipe->cv.notify_all();
                return false;
            }
            return true;
        },
        [pipe](bool) {
            std::lock_guard<std::mutex> lk(pipe->m);
            pipe->cancel = true;
            pipe->cv.notify_all();
        });
}

std::string hdr_quote(const std::string & s) {
    std::string o;
    for (char c : s) { if (c == '"' || c == '\\') o += '\\'; if (c != '\r' && c != '\n') o += c; }
    return o;
}

}  // namespace

void visual_proxy_setup(httplib::Server & svr, const std::string & origin, std::function<bool(const std::string &)> host_ok) {
    svr.new_task_queue = [] { return new httplib::ThreadPool(16, 128); };
    svr.set_payload_max_length(64u * 1024 * 1024);
    // The forwarder is a window onto one configured origin and must never be a way for another site to write to
    // it: a state-changing request carrying a foreign Origin (or marked cross-site by the browser) is refused, the
    // request target must be an origin-form path (never an absolute URL naming another host).
    svr.set_pre_routing_handler([host_ok](const httplib::Request & req, httplib::Response & res) {
        auto deny = [&](int status, const char * why) {
            res.status = status;
            res.set_content(why, "text/plain");
            return httplib::Server::HandlerResponse::Handled;
        };
        std::string h = req.get_header_value("Host");
        if (!h.empty() && h[0] == '[') h = h.substr(1, h.find(']') - 1);
        else if (h.find(':') != std::string::npos && h.find(':') == h.rfind(':')) h = h.substr(0, h.rfind(':'));
        for (auto & ch : h) ch = (char) std::tolower((unsigned char) ch);
        if (!h.empty() && !host_ok(h)) return deny(403, "host not allowed; add it to server.allowed_hosts");
        if (req.target.empty() || req.target[0] != '/') return deny(400, "bad request target");
        bool safe = req.method == "GET" || req.method == "HEAD";
        if (!safe) {
            std::string origin_h = req.get_header_value("Origin");
            if (!origin_h.empty() && origin_h != "http://" + req.get_header_value("Host") && origin_h != "https://" + req.get_header_value("Host"))
                return deny(403, "cross-origin write refused");
            if (origin_h.empty() && req.get_header_value("Sec-Fetch-Site") == "cross-site") return deny(403, "cross-site write refused");
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    svr.Get(".*", [origin](const httplib::Request & req, httplib::Response & res) { forward(req, res, origin, "", ""); });
    auto with_body = [origin](const httplib::Request & req, httplib::Response & res, const httplib::ContentReader & cr) {
        std::string body, ctype;
        if (req.is_multipart_form_data()) {
            // httplib parses multipart bodies; rebuild one for the tutor (the file import uses it)
            struct Part { httplib::FormData meta; std::string data; };
            std::vector<Part> parts;
            bool ok = cr([&](const httplib::FormData & f) { parts.push_back({f, ""}); return true; },
                         [&](const char * d, size_t n) { if (!parts.empty()) parts.back().data.append(d, n); return true; });
            if (!ok) { res.status = 400; res.set_content("bad multipart body", "text/plain"); return; }
            std::random_device rd;
            char b[40];
            std::snprintf(b, sizeof b, "----judasproxy%08x%08x", rd(), rd());
            for (auto & p : parts) {
                body += std::string("--") + b + "\r\nContent-Disposition: form-data; name=\"" + hdr_quote(p.meta.name) + "\"";
                if (!p.meta.filename.empty()) body += "; filename=\"" + hdr_quote(p.meta.filename) + "\"";
                body += "\r\n";
                if (!p.meta.content_type.empty()) body += "Content-Type: " + p.meta.content_type + "\r\n";
                body += "\r\n" + p.data + "\r\n";
            }
            body += std::string("--") + b + "--\r\n";
            ctype = std::string("multipart/form-data; boundary=") + b;
        } else {
            cr([&](const char * d, size_t n) { body.append(d, n); return true; });
        }
        forward(req, res, origin, std::move(body), ctype);
    };
    svr.Post(".*", with_body);
    svr.Put(".*", with_body);
    svr.Patch(".*", with_body);
    svr.Delete(".*", with_body);
}

}  // namespace judas
