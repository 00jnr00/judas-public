#include "engine/remote_engine.h"

#include <chrono>
#include <mutex>

#include "cpp-httplib/httplib.h"

namespace judas {

namespace {

using Clock = std::chrono::steady_clock;

double secs(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double>(b - a).count(); }

std::string strip_slashes(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

json build_body(const std::string & model, const ChatRequest & req) {
    json msgs = json::array();
    for (auto & m : req.messages) msgs.push_back(message_to_oai(m));
    json body = {{"model", model},
                 {"stream", true},
                 {"stream_options", {{"include_usage", true}}},
                 {"messages", msgs},
                 {"chat_template_kwargs", {{"enable_thinking", req.enable_thinking}}}};
    if (!req.tools.empty()) {
        json tools = json::array();
        for (auto & t : req.tools)
            tools.push_back({{"type", "function"},
                             {"function", {{"name", t.name}, {"description", t.description},
                                           {"parameters", t.parameters}}}});
        body["tools"] = tools;
    }
    const Sampling & s = req.sampling;
    if (s.max_tokens > 0) body["max_tokens"] = s.max_tokens;
    if (s.temp >= 0) body["temperature"] = s.temp;
    if (s.top_p >= 0) body["top_p"] = s.top_p;
    if (s.top_k >= 0) body["top_k"] = s.top_k;
    if (s.min_p >= 0) body["min_p"] = s.min_p;
    if (s.seed != 0xFFFFFFFF) body["seed"] = s.seed;
    return body;
}

int int_of(const json & j, const char * k, int def = 0) {
    return j.contains(k) && j[k].is_number() ? j[k].get<int>() : def;
}

}  // namespace

struct RemoteEngine::Impl {
    std::string base, model;
    int read_timeout_s;
    mutable std::mutex mu;
    mutable std::string remote_model;   // learned from /api/state or stream chunks
};

RemoteEngine::RemoteEngine(std::string base_url, std::string model, int read_timeout_s)
    : impl_(new Impl) {
    impl_->base = strip_slashes(std::move(base_url));
    impl_->model = std::move(model);
    impl_->read_timeout_s = read_timeout_s;
}

RemoteEngine::~RemoteEngine() = default;

std::string RemoteEngine::model_name() const {
    std::lock_guard lk(impl_->mu);
    return impl_->remote_model.empty() ? impl_->model : impl_->remote_model;
}

int RemoteEngine::count_tokens(const std::string & text) const { return (int)((text.size() + 3) / 4); }

bool RemoteEngine::reachable() const {
    httplib::Client cli(impl_->base);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    cli.set_write_timeout(2);
    auto r = cli.Get("/health");
    return r && r->status == 200;
}

EngineState RemoteEngine::state() const {
    EngineState st;
    httplib::Client cli(impl_->base);
    cli.set_connection_timeout(2);
    cli.set_read_timeout(2);
    cli.set_write_timeout(2);
    auto r = cli.Get("/api/state");
    if (!r || r->status != 200) return st;
    try {
        json j = json::parse(r->body);
        if (!j.is_object()) return st;
        if (j.contains("model") && j["model"].is_string()) st.model = j["model"].get<std::string>();
        st.loaded = j.value("loaded", false);
        st.max_agents = int_of(j, "max_agents");
        st.active = int_of(j, "active");
        st.queued = int_of(j, "queued");
        st.ctx_total = int_of(j, "ctx_total");
        st.ctx_used = int_of(j, "ctx_used");
        if (j.contains("gen_tok_s") && j["gen_tok_s"].is_number()) st.gen_tok_s = j["gen_tok_s"].get<double>();
        if (j.contains("seqs") && j["seqs"].is_array())
            for (auto & q : j["seqs"]) {
                if (!q.is_object()) continue;
                EngineState::Seq s{};
                s.id = int_of(q, "id");
                s.tag = q.value("tag", std::string());
                s.phase = q.value("phase", std::string());
                s.tokens = int_of(q, "tokens");
                s.priority = int_of(q, "priority");
                st.seqs.push_back(std::move(s));
            }
        if (!st.model.empty()) {
            std::lock_guard lk(impl_->mu);
            impl_->remote_model = st.model;
        }
    } catch (const std::exception &) {
        return EngineState{};
    }
    return st;
}

ChatResult RemoteEngine::chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> & on_delta,
                              const std::atomic<bool> * cancel) {
    ChatResult res;
    res.message.role = "assistant";
    std::string body = build_body(impl_->model, req).dump(-1, ' ', false, json::error_handler_t::replace);

    httplib::Client cli(impl_->base);
    cli.set_connection_timeout(3);
    cli.set_read_timeout(impl_->read_timeout_s);
    cli.set_write_timeout(30);

    auto t0 = Clock::now();
    Clock::time_point t_first{};
    bool got_first = false, cancelled = false, saw_data = false, bad_json = false;
    std::string finish, raw_head, line_buf, parse_err;
    std::vector<ChatMessage::ToolCall> calls;

    auto mark_first = [&] {
        if (!got_first) {
            got_first = true;
            t_first = Clock::now();
        }
    };
    auto handle_data = [&](const std::string & data) {
        if (data == "[DONE]") return;
        json j;
        try {
            j = json::parse(data);
        } catch (const std::exception & e) {
            bad_json = true;
            parse_err = e.what();
            return;
        }
        if (!j.is_object()) return;
        saw_data = true;
        if (j.contains("error")) {
            const json & e = j["error"];
            res.error = e.is_object() && e.contains("message") && e["message"].is_string()
                            ? e["message"].get<std::string>() : e.dump();
            finish = "error";
            return;
        }
        if (j.contains("model") && j["model"].is_string() && !j["model"].get<std::string>().empty()) {
            std::lock_guard lk(impl_->mu);
            impl_->remote_model = j["model"].get<std::string>();
        }
        if (j.contains("usage") && j["usage"].is_object()) {
            const json & u = j["usage"];
            res.prompt_tokens = int_of(u, "prompt_tokens", res.prompt_tokens);
            res.completion_tokens = int_of(u, "completion_tokens", res.completion_tokens);
            if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object())
                res.cached_tokens = int_of(u["prompt_tokens_details"], "cached_tokens", res.cached_tokens);
        }
        if (!j.contains("choices") || !j["choices"].is_array() || j["choices"].empty()) return;
        const json & ch = j["choices"][0];
        if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) finish = ch["finish_reason"].get<std::string>();
        if (!ch.contains("delta") || !ch["delta"].is_object()) return;
        const json & d = ch["delta"];
        ChatDelta cd;
        if (d.contains("content") && d["content"].is_string()) cd.content = d["content"].get<std::string>();
        if (d.contains("reasoning_content") && d["reasoning_content"].is_string())
            cd.reasoning = d["reasoning_content"].get<std::string>();
        if (d.contains("tool_calls") && d["tool_calls"].is_array()) {
            for (auto & tc : d["tool_calls"]) {
                if (!tc.is_object()) continue;
                mark_first();
                size_t idx = tc.contains("index") && tc["index"].is_number() ? (size_t) tc["index"].get<int>() : 0;
                if (idx > 256) continue;
                if (calls.size() <= idx) calls.resize(idx + 1);
                auto & c = calls[idx];
                if (tc.contains("id") && tc["id"].is_string() && c.id.empty()) c.id = tc["id"].get<std::string>();
                if (tc.contains("function") && tc["function"].is_object()) {
                    const json & f = tc["function"];
                    if (f.contains("name") && f["name"].is_string()) {
                        std::string n = f["name"].get<std::string>();
                        if (c.name.empty()) c.name = n;
                        else if (n != c.name) c.name += n;
                    }
                    if (f.contains("arguments") && f["arguments"].is_string()) c.arguments += f["arguments"].get<std::string>();
                }
            }
        }
        if (!cd.content.empty() || !cd.reasoning.empty()) {
            mark_first();
            res.message.content += cd.content;
            res.message.reasoning += cd.reasoning;
            if (on_delta && !on_delta(cd)) cancelled = true;
        }
    };
    auto handle_line = [&](std::string line) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, 5, "data:") != 0) return;
        size_t p = 5;
        while (p < line.size() && line[p] == ' ') p++;
        handle_data(line.substr(p));
    };

    httplib::Headers headers = {{"Accept", "text/event-stream"}};
    auto r = cli.Post(
        "/v1/chat/completions", headers, body, "application/json",
        [&](const char * data, size_t n) {
            if (cancel && cancel->load()) {
                cancelled = true;
                return false;
            }
            if (raw_head.size() < 300) raw_head.append(data, std::min(n, 300 - raw_head.size()));
            line_buf.append(data, n);
            size_t pos;
            while (!cancelled && (pos = line_buf.find('\n')) != std::string::npos) {
                std::string line = line_buf.substr(0, pos);
                line_buf.erase(0, pos + 1);
                handle_line(std::move(line));
            }
            if (cancel && cancel->load()) cancelled = true;
            return !cancelled;
        });
    if (!cancelled && !line_buf.empty()) handle_line(line_buf);   // last line without a newline

    auto t_end = Clock::now();
    if (got_first) {
        res.prefill_s = secs(t0, t_first);
        res.generate_s = secs(t_first, t_end);
    } else {
        res.prefill_s = secs(t0, t_end);
    }
    for (auto & c : calls) {
        if (c.name.empty() && c.arguments.empty() && c.id.empty()) continue;
        res.message.tool_calls.push_back(std::move(c));
    }

    auto fail = [&](const std::string & msg) {
        res.finish_reason = "error";
        res.error = msg;
        return res;
    };
    if (cancelled) {
        res.finish_reason = "cancelled";
        return res;
    }
    if (!r) return fail("remote engine unreachable at " + impl_->base + " (" + httplib::to_string(r.error()) + ")");
    if (r->status != 200) return fail("remote engine HTTP " + std::to_string(r->status) + ": " + raw_head.substr(0, 300));
    if (finish == "error") return fail(res.error.empty() ? "remote engine error" : res.error);
    if (bad_json && finish.empty()) return fail("bad JSON in remote stream: " + parse_err);
    if (!saw_data) return fail("empty reply from remote engine");
    res.finish_reason = finish.empty() ? "stop" : finish;
    return res;
}

}  // namespace judas
