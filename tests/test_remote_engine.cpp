// RemoteEngine against a canned in-process HTTP server on loopback: SSE parsing across chunk boundaries,
// tool-call assembly, error paths, cancellation, request body and /api/state. No model, no network.
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

#include "cpp-httplib/httplib.h"
#include "engine/remote_engine.h"
#include "test.h"

using namespace judas;

namespace {

std::string sse(const json & j) { return "data: " + j.dump() + "\n\n"; }

json chunk(const json & delta, const json & finish = nullptr) {
    return {{"id", "x"}, {"object", "chat.completion.chunk"}, {"model", "canned-model"},
            {"choices", json::array({{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}})}};
}

// Sends the pieces as separate HTTP chunks, so the client sees whatever boundaries the pieces have.
void send_pieces(httplib::Response & res, std::vector<std::string> pieces) {
    auto idx = std::make_shared<size_t>(0);
    res.set_chunked_content_provider("text/event-stream", [pieces, idx](size_t, httplib::DataSink & sink) {
        if (*idx >= pieces.size()) {
            sink.done();
            return true;
        }
        const std::string & p = pieces[(*idx)++];
        bool ok = sink.write(p.data(), p.size());   // false once the client left
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return ok;
    });
}

struct Fake {
    httplib::Server svr;
    std::thread th;
    int port = 0;
    std::mutex mu;
    json last_body;
    std::string mode = "text";
    bool state_ok = true;

    Fake() {
        svr.Post("/v1/chat/completions", [this](const httplib::Request & req, httplib::Response & res) {
            std::string m;
            {
                std::lock_guard lk(mu);
                last_body = json::parse(req.body);
                m = mode;
            }
            if (m == "500") {
                res.status = 500;
                res.set_content("{\"error\":{\"message\":\"boom\"}}", "application/json");
                return;
            }
            if (m == "text") {
                std::string a = sse(chunk({{"role", "assistant"}}));
                std::string b = sse(chunk({{"reasoning_content", "think "}}));
                std::string c = sse(chunk({{"reasoning_content", "more"}}));
                std::string d = sse(chunk({{"content", "Hello, "}}));
                std::string e = sse(chunk({{"content", "world"}}));
                std::string f = sse(chunk(json::object(), "stop"));
                json u = {{"id", "x"}, {"model", "canned-model"}, {"choices", json::array()},
                          {"usage", {{"prompt_tokens", 11}, {"completion_tokens", 5},
                                     {"prompt_tokens_details", {{"cached_tokens", 4}}}}}};
                std::string g = sse(u) + "data: [DONE]\n\n";
                // awkward boundaries: mid-line, mid-JSON, inside the "\n\n" separator
                std::string all = a + b + c + d + e + f + g;
                std::vector<std::string> pieces;
                size_t step[] = {3, 17, 1, 40, 2, 9};
                size_t pos = 0, k = 0;
                while (pos < all.size()) {
                    size_t n = std::min(step[k++ % 6], all.size() - pos);
                    pieces.push_back(all.substr(pos, n));
                    pos += n;
                }
                send_pieces(res, pieces);
                return;
            }
            if (m == "tools") {
                auto call = [](const json & fn, bool first) {
                    json c = {{"index", 0}, {"function", fn}};
                    if (first) {
                        c["id"] = "call_1";
                        c["type"] = "function";
                    }
                    return sse(chunk({{"tool_calls", json::array({c})}}));
                };
                send_pieces(res, {sse(chunk({{"role", "assistant"}})),
                                  call({{"name", "read_file"}, {"arguments", "{\"pa"}}, true),
                                  call({{"arguments", "th\": \"/tm"}}, false),
                                  call({{"arguments", "p/x\"}"}}, false),
                                  sse(chunk(json::object(), "tool_calls")), "data: [DONE]\n\n"});
                return;
            }
            if (m == "long") {   // many content chunks, for cancellation
                std::vector<std::string> p;
                for (int i = 0; i < 200; i++) p.push_back(sse(chunk({{"content", "tok "}})));
                send_pieces(res, p);
                return;
            }
        });
        svr.Get("/health", [](const httplib::Request &, httplib::Response & res) { res.set_content("ok", "text/plain"); });
        svr.Get("/api/state", [this](const httplib::Request &, httplib::Response & res) {
            if (!state_ok) {
                res.status = 503;
                return;
            }
            json j = {{"model", "canned-model"}, {"loaded", true}, {"max_agents", 8}, {"active", 2},
                      {"queued", 1}, {"ctx_total", 49152}, {"ctx_used", 1234}, {"gen_tok_s", 25.5},
                      {"seqs", json::array({{{"id", 3}, {"tag", "agent a-1"}, {"phase", "generate"},
                                             {"tokens", 77}, {"priority", 1}}})}};
            res.set_content(j.dump(), "application/json");
        });
        svr.set_keep_alive_timeout(1);   // stop() waits for idle keep-alive connections
        port = svr.bind_to_any_port("127.0.0.1");
        th = std::thread([this] { svr.listen_after_bind(); });
        svr.wait_until_ready();
    }
    ~Fake() {
        svr.stop();
        th.join();
    }
    std::string url() const { return "http://127.0.0.1:" + std::to_string(port) + "/"; }   // trailing slash on purpose
};

ChatRequest simple_req() {
    ChatRequest r;
    ChatMessage m;
    m.role = "user";
    m.content = "hi";
    r.messages.push_back(m);
    return r;
}

void test_stream_text(Fake & f) {
    f.mode = "text";
    RemoteEngine eng(f.url(), "swarm", 30);
    CHECK(eng.model_name() == "swarm");
    std::string content, reasoning;
    auto r = eng.chat(simple_req(), [&](const ChatDelta & d) {
        content += d.content;
        reasoning += d.reasoning;
        return true;
    });
    CHECK(r.finish_reason == "stop");
    CHECK(r.error.empty());
    CHECK(content == "Hello, world");
    CHECK(reasoning == "think more");
    CHECK(r.message.content == "Hello, world");
    CHECK(r.message.reasoning == "think more");
    CHECK(r.message.role == "assistant");
    CHECK(r.prompt_tokens == 11);
    CHECK(r.completion_tokens == 5);
    CHECK(r.cached_tokens == 4);
    CHECK(r.prefill_s >= 0 && r.generate_s >= 0);
    CHECK(eng.model_name() == "canned-model");   // learned from the stream
}

void test_stream_tools(Fake & f) {
    f.mode = "tools";
    RemoteEngine eng(f.url());
    auto r = eng.chat(simple_req());
    CHECK(r.finish_reason == "tool_calls");
    REQUIRE(r.message.tool_calls.size() == 1);
    CHECK(r.message.tool_calls[0].id == "call_1");
    CHECK(r.message.tool_calls[0].name == "read_file");
    CHECK(r.message.tool_calls[0].arguments == "{\"path\": \"/tmp/x\"}");
}

void test_http_500(Fake & f) {
    f.mode = "500";
    RemoteEngine eng(f.url());
    auto r = eng.chat(simple_req());
    CHECK(r.finish_reason == "error");
    CHECK(r.error.find("500") != std::string::npos);
    CHECK(r.error.find("boom") != std::string::npos);
}

void test_unreachable() {
    // a loopback port that was just bound and closed again: nothing listens there
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(fd, (sockaddr *) &a, sizeof a) == 0);
    socklen_t len = sizeof a;
    REQUIRE(::getsockname(fd, (sockaddr *) &a, &len) == 0);
    int port = ntohs(a.sin_port);
    ::close(fd);
    RemoteEngine eng("http://127.0.0.1:" + std::to_string(port), "swarm", 5);
    auto r = eng.chat(simple_req());
    CHECK(r.finish_reason == "error");
    CHECK(!r.error.empty());
    CHECK(!eng.reachable());
    auto st = eng.state();
    CHECK(!st.loaded);
    CHECK(st.model.empty());
}

void test_cancel(Fake & f) {
    f.mode = "long";
    RemoteEngine eng(f.url());
    int n = 0;
    auto r = eng.chat(simple_req(), [&](const ChatDelta &) { return ++n < 3; });
    CHECK(r.finish_reason == "cancelled");
    CHECK(n == 3);
    CHECK(r.message.content == "tok tok tok ");

    std::atomic<bool> stop{false};
    int m = 0;
    auto r2 = eng.chat(simple_req(), [&](const ChatDelta &) {
        if (++m == 2) stop = true;
        return true;
    }, &stop);
    CHECK(r2.finish_reason == "cancelled");
    CHECK(m >= 2 && m < 200);
}

void test_request_body(Fake & f) {
    f.mode = "text";
    RemoteEngine eng(f.url(), "my-model");
    ChatRequest req = simple_req();
    ToolSpec t;
    t.name = "read_file";
    t.description = "Read a file";
    t.parameters = {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}};
    req.tools.push_back(t);
    req.sampling.max_tokens = 321;
    req.sampling.temp = 0.5f;
    req.sampling.top_k = 20;
    req.sampling.seed = 42;
    req.enable_thinking = false;
    eng.chat(req);
    json b;
    {
        std::lock_guard lk(f.mu);
        b = f.last_body;
    }
    CHECK(b["model"] == "my-model");
    CHECK(b["stream"] == true);
    CHECK(b["stream_options"]["include_usage"] == true);
    CHECK(b["max_tokens"] == 321);
    CHECK(b["top_k"] == 20);
    CHECK(b["seed"] == 42);
    CHECK(!b.contains("top_p"));
    CHECK(!b.contains("min_p"));
    CHECK(b["chat_template_kwargs"]["enable_thinking"] == false);
    REQUIRE(b["tools"].size() == 1);
    CHECK(b["tools"][0]["type"] == "function");
    CHECK(b["tools"][0]["function"]["name"] == "read_file");
    CHECK(b["tools"][0]["function"]["description"] == "Read a file");
    CHECK(b["tools"][0]["function"]["parameters"]["type"] == "object");
    CHECK(b["messages"][0]["content"] == "hi");

    // defaults: no tools, no max_tokens, thinking on
    eng.chat(simple_req());
    {
        std::lock_guard lk(f.mu);
        b = f.last_body;
    }
    CHECK(!b.contains("tools"));
    CHECK(!b.contains("max_tokens"));
    CHECK(!b.contains("seed"));
    CHECK(b["chat_template_kwargs"]["enable_thinking"] == true);
}

void test_state(Fake & f) {
    RemoteEngine eng(f.url());
    CHECK(eng.reachable());
    auto st = eng.state();
    CHECK(st.loaded);
    CHECK(st.model == "canned-model");
    CHECK(st.max_agents == 8);
    CHECK(st.active == 2);
    CHECK(st.queued == 1);
    CHECK(st.ctx_total == 49152);
    CHECK(st.ctx_used == 1234);
    CHECK(st.gen_tok_s > 25.4 && st.gen_tok_s < 25.6);
    REQUIRE(st.seqs.size() == 1);
    CHECK(st.seqs[0].tag == "agent a-1");
    CHECK(st.seqs[0].tokens == 77);
    CHECK(eng.model_name() == "canned-model");
    f.state_ok = false;
    CHECK(!eng.state().loaded);
    f.state_ok = true;
}

void test_count_tokens() {
    RemoteEngine eng("http://127.0.0.1:1");
    CHECK(eng.count_tokens("") == 0);
    CHECK(eng.count_tokens("abcd") == 1);
    CHECK(eng.count_tokens("abcde") == 2);
}

}  // namespace

int main() {
    {
        Fake f;
        test_stream_text(f);
        test_stream_tools(f);
        test_http_500(f);
        test_cancel(f);
        test_request_body(f);
        test_state(f);
    }
    test_unreachable();
    test_count_tokens();
    TEST_MAIN_END();
}
