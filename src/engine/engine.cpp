// Engine implementation: libllama in-process, one scheduler thread, continuous batching across agents.
// Design findings this follows: batching N agents is cheap, prefix reuse is the big win,
// prefill calls have a fixed cost so chunking does not buy fairness, and hybrid models cannot cut KV mid-sequence.

#include "engine/engine.h"
#include "engine/sched.h"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <fstream>
#include <cctype>
#include <filesystem>
#include <list>
#include <mutex>
#include <stdexcept>
#include <thread>

#include "cpp-httplib/httplib.h"

#include "chat.h"
#include "common.h"
#include "ggml-backend.h"
#include "llama.h"
#include "mtmd.h"
#include "mtmd-helper.h"
#include "sampling.h"

namespace judas {

namespace {

using clk = std::chrono::steady_clock;
double secs_since(clk::time_point t) { return std::chrono::duration<double>(clk::now() - t).count(); }

long read_sysfs_mib(const char * path) {
    std::ifstream f(path);
    long v = -1;
    if (f >> v) return v / 1048576;
    return -1;
}

common_chat_msg to_common(const ChatMessage & m) {
    common_chat_msg c;
    c.role = m.role;
    c.content = m.content;
    c.reasoning_content = m.reasoning;
    for (const auto & tc : m.tool_calls) {
        // A malformed call the model once emitted must not poison every later render of the history
        // (the template parses arguments as JSON and throws). Replace it with a valid stub.
        std::string args = tc.arguments;
        if (!json::accept(args.empty() ? "{}" : args))
            args = json({{"invalid_json_truncated", args.substr(0, 200)}}).dump();
        c.tool_calls.push_back({tc.name, args, tc.id});
    }
    c.tool_call_id = m.tool_call_id;
    c.tool_name = m.tool_name;
    return c;
}

ChatMessage from_common(const common_chat_msg & c) {
    ChatMessage m;
    m.role = c.role.empty() ? "assistant" : c.role;
    m.content = c.content;
    m.reasoning = c.reasoning_content;
    for (const auto & tc : c.tool_calls) m.tool_calls.push_back({tc.id, tc.name, tc.arguments});
    return m;
}

ggml_type kv_ggml_type(const std::string & t) {
    if (t == "f16") return GGML_TYPE_F16;
    if (t == "q5_1") return GGML_TYPE_Q5_1;
    if (t == "q5_0") return GGML_TYPE_Q5_0;
    if (t == "q4_1") return GGML_TYPE_Q4_1;
    if (t == "q4_0") return GGML_TYPE_Q4_0;
    return GGML_TYPE_Q8_0;
}

// ---- base64 and image sniffing (vision) ----

std::string b64_encode(const std::string & in) {
    static const char * tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        unsigned v = ((unsigned char) in[i] << 16) | ((unsigned char) in[i + 1] << 8) | (unsigned char) in[i + 2];
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i + 1 == in.size()) {
        unsigned v = (unsigned char) in[i] << 16;
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += "==";
    } else if (i + 2 == in.size()) {
        unsigned v = ((unsigned char) in[i] << 16) | ((unsigned char) in[i + 1] << 8);
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += '=';
    }
    return out;
}

// Lenient: skips whitespace and stops at padding; any other stray character yields false.
bool b64_decode(const std::string & in, std::string & out) {
    out.clear();
    unsigned acc = 0;
    int bits = 0;
    for (char ch : in) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+' || ch == '-') v = 62;
        else if (ch == '/' || ch == '_') v = 63;
        else if (ch == '=') break;
        else if (std::isspace((unsigned char) ch)) continue;
        else return false;
        acc = (acc << 6) | (unsigned) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += (char) ((acc >> bits) & 0xff);
        }
    }
    return true;
}

const char * sniff_image_mime(const std::string & d) {
    if (d.compare(0, 8, "\x89PNG\r\n\x1a\n") == 0) return "image/png";
    if (d.size() >= 3 && (unsigned char) d[0] == 0xff && (unsigned char) d[1] == 0xd8) return "image/jpeg";
    if (d.compare(0, 4, "GIF8") == 0) return "image/gif";
    if (d.size() >= 12 && d.compare(0, 4, "RIFF") == 0 && d.compare(8, 4, "WEBP") == 0) return "image/webp";
    if (d.compare(0, 2, "BM") == 0) return "image/bmp";
    return "image/png";
}

uint32_t fnv1a(const std::string & s) {
    uint32_t h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

size_t common_prefix(const std::vector<llama_token> & a, const std::vector<llama_token> & b) {
    size_t n = std::min(a.size(), b.size()), i = 0;
    while (i < n && a[i] == b[i]) i++;
    return i;
}

enum class Phase { queued, prefill, generate, done };

struct Request {
    ChatRequest req;
    std::function<bool(const ChatDelta &)> on_delta;
    const std::atomic<bool> * cancel = nullptr;

    std::vector<llama_token> prompt;
    // Images (vision): each is one span of prompt, filled with unique negative placeholder tokens so that a
    // token index is a llama position everywhere. The scheduler never batches a span; it evaluates the chunk
    // through mtmd instead (the placeholders only enter the sequence's token list afterwards).
    struct Media {
        size_t start = 0, n_pos = 0;
        const mtmd_input_chunk * chunk = nullptr;
    };
    std::vector<Media> media;
    std::unique_ptr<mtmd_input_chunks, void (*)(mtmd_input_chunks *)> chunks{nullptr, mtmd_input_chunks_free};
    size_t kv_extra = 0;                 // KV cells the images need beyond their positions (M-RoPE uses fewer positions)
    size_t prompt_base = 0;              // rendered prompt length; after a preemption prompt = base + generated
    size_t boundary = 0;                 // end of the shared prefix (system prompt) worth snapshotting
    common_chat_parser_params parser;
    std::vector<std::string> stops;
    common_sampler * smpl = nullptr;

    Phase phase = Phase::queued;
    int seq = -1;
    size_t n_done = 0;                   // prompt tokens already in the KV for this request
    size_t ckpt_at = 0;                  // hybrid: take a checkpoint when the prefill reaches this many tokens
    int idx = -1;                        // logits index in the current batch
    llama_token last = -1;               // sampled, not yet decoded
    std::string text;                    // generated text so far
    std::vector<llama_token> generated;
    int max_tokens = 0;
    int ctx_limit = 0;                   // effective context cap: ctx_per_agent, or ctx_per_agent_max while borrowing
    bool borrowing = false;              // prompt is over the soft cap; first to be preempted
    common_chat_msg partial;
    std::vector<std::string> tool_ids;
    bool deferred = false;
    size_t parsed_len = 0;               // length of `text` at the last streaming parse
    ChatDelta pend;                      // streamed text not yet handed to on_delta (delivered outside mu_)
    int admissions = 0, preempted = 0;
    long long admit_order = 0;

    ChatResult result;
    clk::time_point t_submit, t_start, t_gen;
    clk::time_point t_chunk;             // last prefill chunk (or admission): the starvation guard measures from here
    bool finished = false;
    std::mutex mu;
    std::condition_variable cv;
};

// Hybrid/recurrent models cannot cut a sequence mid-way. A checkpoint holds only the recurrent part of the
// state (LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY) at a token count; restoring it and then trimming the attention KV
// rolls the sequence back to that point (the same trick as llama-server's context checkpoints).
struct Ckpt {
    size_t n = 0;
    std::vector<uint8_t> data;
};

struct Seq {
    std::vector<llama_token> tokens;     // exactly what is in the KV for this sequence
    std::vector<Ckpt> ckpts;             // oldest first, hybrid models only
    Request * owner = nullptr;
    long long last_used = 0;

    void truncate(size_t n) {
        tokens.resize(std::min(tokens.size(), n));
        std::erase_if(ckpts, [&](const Ckpt & c) { return c.n > tokens.size(); });
    }
};

struct Snapshot {
    std::vector<llama_token> tokens;
    std::vector<uint8_t> data;
    long long last_used = 0;
};

class LlamaEngine final : public Engine {
public:
    explicit LlamaEngine(const Config & cfg) : cfg_(cfg), mcfg_(cfg.active_model()) {
        if (!getenv("JUDAS_DEBUG"))
            llama_log_set(
                [](ggml_log_level level, const char * text, void *) {
                    if (level >= GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
                },
                nullptr);
        llama_backend_init();
        auto mp = llama_model_default_params();
        mp.n_gpu_layers = mcfg_.gpu_layers;
        if (mcfg_.cpu_moe) {
            overrides_[0] = {"\\.ffn_(up|down|gate)_exps",
                             ggml_backend_dev_buffer_type(ggml_backend_dev_by_type(GGML_BACKEND_DEVICE_TYPE_CPU))};
            overrides_[1] = {nullptr, nullptr};
            mp.tensor_buft_overrides = overrides_;
        }
        std::string path = expand_home(mcfg_.path);
        model_ = llama_model_load_from_file(path.c_str(), mp);
        if (!model_) throw std::runtime_error("cannot load model " + path);
        vocab_ = llama_model_get_vocab(model_);

        n_seq_ = std::clamp(cfg.engine.max_agents, 1, 8);
        auto cp = llama_context_default_params();
        cp.n_ctx = mcfg_.ctx_total;
        cp.n_batch = mcfg_.batch;
        cp.n_ubatch = mcfg_.ubatch;
        cp.n_seq_max = n_seq_;
        cp.kv_unified = true;
        cp.swa_full = true;
        cp.flash_attn_type = mcfg_.flash_attn ? LLAMA_FLASH_ATTN_TYPE_ENABLED : LLAMA_FLASH_ATTN_TYPE_DISABLED;
        cp.type_k = kv_ggml_type(mcfg_.kv_type_k.empty() ? mcfg_.kv_type : mcfg_.kv_type_k);
        cp.type_v = kv_ggml_type(mcfg_.kv_type_v.empty() ? mcfg_.kv_type : mcfg_.kv_type_v);
        cp.n_threads = cp.n_threads_batch = mcfg_.threads;
        ctx_ = llama_init_from_model(model_, cp);
        if (!ctx_) throw std::runtime_error("cannot create context for " + path);
        mem_ = llama_get_memory(ctx_);
        hybrid_ = llama_model_is_hybrid(model_) || llama_model_is_recurrent(model_);
        seqs_.resize(n_seq_);
        kv_ = KvBudget(mcfg_.ctx_total, cfg.engine.kv_reserve);
        batch_ = llama_batch_init(std::max(mcfg_.batch, cfg.engine.prefill_chunk + n_seq_), 0, 1);

        std::string tmpl_override;
        if (!mcfg_.chat_template_file.empty()) {
            std::ifstream f(expand_home(mcfg_.chat_template_file));
            tmpl_override.assign(std::istreambuf_iterator<char>(f), {});
        }
        tmpls_ = common_chat_templates_init(model_, tmpl_override);
        path_ = path;
        init_vision(path);
        thread_ = std::thread([this] { loop(); });
    }

    // Projector for image input. A missing or broken projector only turns vision off.
    void init_vision(const std::string & model_path) {
        if (mcfg_.mmproj == "none") return;
        std::string proj;
        if (mcfg_.mmproj.empty()) {
            namespace fs = std::filesystem;
            std::error_code ec;
            for (const auto & e : fs::directory_iterator(fs::path(model_path).parent_path(), ec)) {
                std::string n = e.path().filename().string();
                for (auto & c : n) c = (char) std::tolower((unsigned char) c);
                if (n.rfind("mmproj", 0) == 0 && n.size() > 5 && n.compare(n.size() - 5, 5, ".gguf") == 0) {
                    proj = e.path().string();
                    break;
                }
            }
            if (proj.empty()) return;
        } else {
            proj = expand_home(mcfg_.mmproj);
        }
        if (!getenv("JUDAS_DEBUG"))
            mtmd_helper_log_set(
                [](ggml_log_level level, const char * text, void *) {
                    if (level >= GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
                },
                nullptr);
        auto mp = mtmd_context_params_default();
        mp.use_gpu = mcfg_.mmproj_gpu;
        mp.n_threads = mcfg_.threads;
        if (mcfg_.image_max_tokens > 0) mp.image_max_tokens = mcfg_.image_max_tokens;
        mp.print_timings = false;
        mp.warmup = false;
        mp.flash_attn_type = mcfg_.flash_attn ? LLAMA_FLASH_ATTN_TYPE_ENABLED : LLAMA_FLASH_ATTN_TYPE_DISABLED;
        mctx_ = mtmd_init_from_file(proj.c_str(), model_, mp);
        if (!mctx_) fprintf(stderr, "judas: could not load vision projector %s, images are off\n", proj.c_str());
    }

    ~LlamaEngine() override {
        {
            std::lock_guard lk(mu_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        llama_batch_free(batch_);
        if (mctx_) mtmd_free(mctx_);
        llama_free(ctx_);
        llama_model_free(model_);
    }

    std::string model_name() const override { return cfg_.engine.model; }

    bool vision() const override { return mctx_ && mtmd_support_vision(mctx_); }

    int count_tokens(const std::string & text) const override {
        int n = -llama_tokenize(vocab_, text.c_str(), text.size(), nullptr, 0, false, true);
        return n < 0 ? -n : n;
    }

    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> & on_delta,
                    const std::atomic<bool> * cancel) override {
        auto r = std::make_unique<Request>();
        r->req = req;
        r->on_delta = on_delta;
        r->cancel = cancel;
        r->t_submit = clk::now();
        try {
            prepare(*r);
        } catch (const std::exception & e) {
            r->result.finish_reason = "error";
            r->result.error = e.what();
            return r->result;
        }
        if (r->finished) return r->result;
        {
            std::lock_guard lk(mu_);
            queue_.push_back(r.get());
        }
        cv_.notify_all();
        std::unique_lock lk(r->mu);
        r->cv.wait(lk, [&] { return r->finished; });
        if (r->smpl) common_sampler_free(r->smpl);
        return r->result;
    }

    EngineState state() const override {
        std::lock_guard lk(mu_);
        EngineState s;
        s.model = cfg_.engine.model;
        s.model_path = path_;
        s.loaded = true;
        s.hybrid = hybrid_;
        s.max_agents = n_seq_;
        s.queued = (int) queue_.size();
        s.ctx_total = mcfg_.ctx_total;
        auto live = live_set();
        for (const auto & l : live) s.ctx_active += l.tokens;
        s.ctx_free = std::max(0, kv_.free_tokens(live));
        s.kv_reserve = kv_.reserve();
        s.prefill_duty = cfg_.engine.prefill_duty;
        s.prefill_chunk_busy = cfg_.engine.busy_prefill_chunk();
        s.prefill_max_wait_ms = cfg_.engine.prefill_max_wait_ms;
        s.preemptions = preemptions_;
        for (const auto * r : queue_)
            s.queue.push_back({r->req.tag, r->req.priority, (int) r->prompt.size(), secs_since(r->t_submit), r->preempted});
        s.vram_used_mib = read_sysfs_mib("/sys/class/drm/card1/device/mem_info_vram_used");
        s.vram_total_mib = read_sysfs_mib("/sys/class/drm/card1/device/mem_info_vram_total");
        for (int i = 0; i < n_seq_; i++) {
            const auto & q = seqs_[i];
            s.ctx_used += (int) q.tokens.size();
            std::string phase = "idle", tag;
            int prio = -1;
            if (q.owner) {
                s.active++;
                phase = q.owner->phase == Phase::prefill ? "prefill" : "generate";
                tag = q.owner->req.tag;
                prio = q.owner->req.priority;
            }
            s.seqs.push_back({i, tag, phase, (int) q.tokens.size(), prio});
        }
        double window = std::max(1e-3, std::chrono::duration<double>(clk::now() - rate_t0_).count());
        s.gen_tok_s = window < 10 ? rate_prev_ : rate_tokens_ / window;
        s.total_prompt_tokens = total_prompt_;
        s.total_completion_tokens = total_completion_;
        s.requests = total_requests_;
        return s;
    }

private:
    // Render the prompt with the model's template, tokenize, and set up parsing and sampling.
    void prepare(Request & r) {
        common_chat_templates_inputs in;
        // Many chat templates (Qwen among them) reject a system message that is not at the very start.
        // Judas injects mid-conversation system notes (the plan, skills, a compaction summary, clm nudges), so
        // demote any non-leading system message to a user message with a [system] marker, keeping its guidance.
        bool seen_nonsystem = false;
        bool use_media = false;
        std::vector<const std::string *> images;   // in marker order
        if (vision())
            for (const auto & m : r.req.messages)
                if (!m.images.empty()) use_media = true;
        for (const auto & m : r.req.messages) {
            common_chat_msg c = to_common(m);
            if (use_media && !m.images.empty()) {
                std::string marks;
                for (const auto & img : m.images) {
                    marks += mtmd_default_marker();
                    marks += "\n";
                    images.push_back(&img);
                }
                c.content = marks + c.content;
            }
            if (c.role == "system" && seen_nonsystem) {
                c.role = "user";
                c.content = "[system] " + c.content;
            } else if (c.role == "system" && !in.messages.empty()) {
                // A second leading system message (e.g. the [plan] block inserted at index 1) is also refused:
                // these templates allow exactly one system message, at index 0. Fold it into the first.
                in.messages[0].content += "\n\n" + c.content;
                continue;
            }
            if (c.role != "system") seen_nonsystem = true;
            in.messages.push_back(std::move(c));
        }
        for (const auto & t : r.req.tools) in.tools.push_back({t.name, t.description, t.parameters.dump()});
        in.add_generation_prompt = true;
        in.use_jinja = true;
        in.enable_thinking = r.req.enable_thinking;
        in.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
        in.parallel_tool_calls = true;
        for (const auto & [k, v] : mcfg_.template_kwargs) in.chat_template_kwargs[k] = json(v).dump();
        auto params = common_chat_templates_apply(tmpls_.get(), in);

        if (use_media) {
            std::string err = tokenize_media(r, params.prompt, images);
            if (!err.empty()) {
                r.result.finish_reason = "error";
                r.result.error = err;
                r.finished = true;
                return;
            }
        } else {
            r.prompt = tokenize(params.prompt);
        }
        r.parser = common_chat_parser_params(params);
        r.parser.reasoning_format = COMMON_REASONING_FORMAT_DEEPSEEK;
        r.parser.parse_tool_calls = !r.req.tools.empty();
        if (!params.parser.empty()) r.parser.parser.load(params.parser);
        r.stops = params.additional_stops;
        r.partial = common_chat_parse("", true, r.parser);

        // Shared-prefix boundary: everything before the first user message's content (system prompt, tools,
        // template preamble). Rendered with a placeholder user turn, because many templates refuse a
        // conversation with no user message.
        if (!r.req.messages.empty() && r.req.messages[0].role == "system" && r.req.messages.size() > 1) {
            common_chat_templates_inputs sys = in;
            sys.messages.resize(1);
            common_chat_msg probe;
            probe.role = "user";
            probe.content = "\x01\x02";
            sys.messages.push_back(probe);
            try {
                auto st = tokenize(common_chat_templates_apply(tmpls_.get(), sys).prompt);
                size_t b = common_prefix(st, r.prompt);
                if (b >= 256 && b < r.prompt.size()) r.boundary = b;
            } catch (...) {
            }
        }

        // The scheduler decides pool admission; here only the configured caps are checked. A prompt over the
        // soft cap may borrow up to ctx_per_agent_max and waits in the queue until the pool can hold it.
        std::vector<KvLive> no_live;
        CtxDecision ctx = decide_ctx_admission((int) r.prompt.size(), mcfg_.ctx_per_agent, mcfg_.ctx_per_agent_max,
                                               kv_, no_live);
        if (!ctx.allowed) {
            r.result.finish_reason = "error";
            if (mcfg_.ctx_per_agent_max > mcfg_.ctx_per_agent)
                r.result.error = "prompt is " + std::to_string(r.prompt.size()) + " tokens, over ctx_per_agent_max " +
                                 std::to_string(mcfg_.ctx_per_agent_max);
            else
                r.result.error = "prompt is " + std::to_string(r.prompt.size()) + " tokens, over ctx_per_agent " +
                                 std::to_string(mcfg_.ctx_per_agent);
            r.finished = true;
            return;
        }
        r.ctx_limit = ctx.limit;
        r.borrowing = ctx.borrow;

        r.prompt_base = r.prompt.size();
        if (!kv_.can_ever_fit((int) (r.prompt.size() + r.kv_extra))) {
            r.result.finish_reason = "error";
            r.result.error = "prompt is " + std::to_string(r.prompt.size()) + " tokens, larger than the whole KV pool (ctx_total " +
                             std::to_string(kv_.total()) + "); shorten the conversation or raise ctx_total";
            r.finished = true;
            return;
        }

        common_params_sampling sp;
        sp.temp = r.req.sampling.temp >= 0 ? r.req.sampling.temp : mcfg_.temp;
        sp.top_p = r.req.sampling.top_p >= 0 ? r.req.sampling.top_p : mcfg_.top_p;
        sp.min_p = r.req.sampling.min_p >= 0 ? r.req.sampling.min_p : mcfg_.min_p;
        sp.top_k = r.req.sampling.top_k >= 0 ? r.req.sampling.top_k : mcfg_.top_k;
        sp.seed = r.req.sampling.seed;
        for (const auto & [id, bias] : mcfg_.logit_bias) sp.logit_bias.push_back({id, bias});
        r.smpl = common_sampler_init(model_, sp);
        r.max_tokens = ctx_generation_cap((int) r.prompt.size(), r.ctx_limit, r.req.sampling.max_tokens,
                                          cfg_.engine.max_tokens);
    }

    // Tokenize a prompt that holds image markers through mtmd. Text chunks keep their tokens; every image chunk
    // becomes n_pos placeholder tokens (negative, unique per image content and offset, so prefix matching never
    // confuses two images). Returns an error text, empty on success.
    std::string tokenize_media(Request & r, const std::string & text, const std::vector<const std::string *> & images) {
        std::vector<mtmd_bitmap *> bitmaps;
        auto free_bitmaps = [&] {
            for (auto * b : bitmaps) mtmd_bitmap_free(b);
        };
        for (size_t i = 0; i < images.size(); i++) {
            auto w = mtmd_helper_bitmap_init_from_buf(mctx_, (const unsigned char *) images[i]->data(), images[i]->size(),
                                                      false, mtmd_helper_init_opt_default());
            if (w.video_ctx) mtmd_helper_video_free(w.video_ctx);
            if (!w.bitmap) {
                free_bitmaps();
                return "could not read image " + std::to_string(i + 1);
            }
            bitmaps.push_back(w.bitmap);
        }
        r.chunks.reset(mtmd_input_chunks_init());
        mtmd_input_text it;
        it.text = text.c_str();
        it.text_len = text.size();
        it.add_special = llama_vocab_get_add_bos(vocab_);
        it.parse_special = true;
        int32_t rc = mtmd_tokenize(mctx_, r.chunks.get(), &it, bitmaps.data(), bitmaps.size());
        free_bitmaps();
        if (rc != 0) return "image tokenization failed (rc " + std::to_string(rc) + ")";
        size_t n = mtmd_input_chunks_size(r.chunks.get());
        r.prompt.clear();
        r.media.clear();
        r.kv_extra = 0;
        bool last_text = false;
        for (size_t c = 0; c < n; c++) {
            const mtmd_input_chunk * ch = mtmd_input_chunks_get(r.chunks.get(), c);
            auto type = mtmd_input_chunk_get_type(ch);
            if (type == MTMD_INPUT_CHUNK_TYPE_TEXT) {
                size_t nt = 0;
                const llama_token * t = mtmd_input_chunk_get_tokens_text(ch, &nt);
                r.prompt.insert(r.prompt.end(), t, t + nt);
                last_text = nt > 0;
            } else if (type == MTMD_INPUT_CHUNK_TYPE_IMAGE) {
                size_t np = (size_t) std::max<llama_pos>(1, mtmd_input_chunk_get_n_pos(ch));
                const char * id = mtmd_input_chunk_get_id(ch);
                uint32_t h = fnv1a(id ? id : std::to_string(c));
                r.media.push_back({r.prompt.size(), np, ch});
                for (size_t i = 0; i < np; i++)
                    r.prompt.push_back((llama_token) (0x80000000u | ((h + (uint32_t) i * 2654435761u) & 0x7fffffffu)));
                r.kv_extra += mtmd_input_chunk_get_n_tokens(ch) > np ? mtmd_input_chunk_get_n_tokens(ch) - np : 0;
                last_text = false;
            } else {
                return "unsupported media in the prompt";
            }
        }
        if (!last_text) return "the prompt ends with an image; the chat template must add text after it";
        return "";
    }

    // The image span of r that starts at `pos`, or null.
    static const Request::Media * media_at(const Request & r, size_t pos) {
        for (const auto & m : r.media)
            if (m.start == pos) return &m;
        return nullptr;
    }

    // Move pos to the start of the image span that strictly contains it (no change if it is outside every span).
    static size_t outside_media(const Request & r, size_t pos) {
        for (const auto & m : r.media)
            if (pos > m.start && pos < m.start + m.n_pos) return m.start;
        return pos;
    }

    std::vector<llama_token> tokenize(const std::string & s) const {
        bool add_bos = llama_vocab_get_add_bos(vocab_);
        int n = -llama_tokenize(vocab_, s.c_str(), s.size(), nullptr, 0, add_bos, true);
        std::vector<llama_token> t(n);
        llama_tokenize(vocab_, s.c_str(), s.size(), t.data(), n, add_bos, true);
        return t;
    }

    std::string piece(llama_token t) const {
        char buf[256];
        int n = llama_token_to_piece(vocab_, t, buf, sizeof buf, 0, false);
        if (n < 0) {
            std::string big(-n, '\0');
            llama_token_to_piece(vocab_, t, big.data(), big.size(), 0, false);
            return big;
        }
        return std::string(buf, n);
    }

    // --- scheduler (runs on thread_) ---

    // Pick a free sequence for r and reuse as much of its cached KV as possible.
    // How much of `prompt` sequence q can keep. Hybrid models need an exact prefix or a checkpoint.
    size_t usable(const Seq & q, const std::vector<llama_token> & prompt, int * ckpt) const {
        *ckpt = -1;
        size_t lcp = common_prefix(q.tokens, prompt);
        if (!hybrid_ || lcp == q.tokens.size()) return lcp;
        for (int i = (int) q.ckpts.size() - 1; i >= 0; i--)
            if (q.ckpts[i].n <= lcp && q.ckpts[i].n > 0) {
                *ckpt = i;
                return q.ckpts[i].n;
            }
        return 0;
    }

    // The requests holding a sequence right now, as the KV budget sees them. Caller holds mu_.
    std::vector<KvLive> live_set() const {
        std::vector<KvLive> v;
        for (int i = 0; i < n_seq_; i++) {
            const Request * o = seqs_[i].owner;
            if (!o) continue;
            KvLive l;
            l.id = i;
            l.priority = o->req.priority;
            l.admitted = o->admit_order;
            l.tokens = (int) seqs_[i].tokens.size();
            l.pending = o->phase == Phase::prefill ? (int) (o->prompt.size() - std::min(o->n_done, o->prompt.size())) : 0;
            l.borrow = o->borrowing;
            v.push_back(l);
        }
        return v;
    }

    void drop_cache(int i) {
        llama_memory_seq_rm(mem_, i, -1, -1);
        seqs_[i].truncate(0);
    }

    bool admit(Request & r) {
        int best = -1, best_ckpt = -1;
        size_t best_reuse = 0;
        long long oldest = LLONG_MAX;
        int lru = -1;
        for (int i = 0; i < n_seq_; i++) {
            auto & q = seqs_[i];
            if (q.owner) continue;
            int ck;
            size_t reuse = usable(q, r.prompt, &ck);
            if (best < 0 || reuse > best_reuse) {
                best = i;
                best_reuse = reuse;
                best_ckpt = ck;
            }
            if (q.last_used < oldest) {
                oldest = q.last_used;
                lru = i;
            }
        }
        if (best < 0) return false;
        if (best_reuse == 0) {  // nothing to reuse: take the least recently used sequence
            best = lru;
            best_ckpt = -1;
        }
        auto & q = seqs_[best];

        // Make room: the physical pool also holds idle sequences' cached tokens. Drop the least recently used
        // of those (never the one we are about to reuse) until this prompt fits next to everything live.
        {
            long long held = 0;
            std::vector<KvIdle> idle;
            for (int i = 0; i < n_seq_; i++) {
                if (i == best) continue;
                held += (long long) seqs_[i].tokens.size();
                if (seqs_[i].owner) {
                    if (seqs_[i].owner->phase == Phase::prefill)
                        held += (long long) (seqs_[i].owner->prompt.size() - seqs_[i].owner->n_done);
                } else {
                    idle.push_back({i, seqs_[i].last_used, (int) seqs_[i].tokens.size()});
                }
            }
            long long over = held + (long long) r.prompt.size() - kv_.total();
            if (over > 0)
                for (int i : KvBudget::idle_to_evict(idle, (int) over)) drop_cache(i);
        }

        // A RAM snapshot of the shared prefix may beat what the sequence has.
        Snapshot * snap = nullptr;
        for (auto & s : snaps_) {
            if (s.tokens.size() > best_reuse && s.tokens.size() < r.prompt.size() &&
                common_prefix(s.tokens, r.prompt) == s.tokens.size() && (!snap || s.tokens.size() > snap->tokens.size()))
                snap = &s;
        }

        size_t reuse = best_reuse;
        if (reuse >= r.prompt.size()) reuse = r.prompt.size() - 1;  // must decode at least one token for logits
        reuse = outside_media(r, reuse);                            // an image is all or nothing
        if (snap) {
            llama_memory_seq_rm(mem_, best, -1, -1);
            if (llama_state_seq_set_data(ctx_, snap->data.data(), snap->data.size(), best) > 0) {
                q.ckpts.clear();
                q.tokens = snap->tokens;
                reuse = q.tokens.size();
                snap->last_used = ++clock_;
            } else {
                q.truncate(0);
                reuse = 0;
            }
        } else if (best_ckpt >= 0 && reuse == q.ckpts[best_ckpt].n) {
            // roll back to the checkpoint: recurrent state from the checkpoint, attention KV trimmed after it
            const auto & c = q.ckpts[best_ckpt];
            bool ok = llama_state_seq_set_data_ext(ctx_, c.data.data(), c.data.size(), best,
                                                   LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY) > 0 &&
                      llama_memory_seq_rm(mem_, best, (llama_pos) c.n, -1);
            if (!ok) {
                llama_memory_seq_rm(mem_, best, -1, -1);
                reuse = 0;
            }
            q.truncate(reuse);
        } else if (reuse < q.tokens.size()) {
            if (!llama_memory_seq_rm(mem_, best, reuse, -1)) {
                llama_memory_seq_rm(mem_, best, -1, -1);
                reuse = 0;
            }
            q.truncate(reuse);
        }
        if (hybrid_ && r.prompt.size() > reuse + 64) r.ckpt_at = outside_media(r, r.prompt.size() - 32);
        q.owner = &r;
        q.last_used = ++clock_;
        r.seq = best;
        r.n_done = reuse;
        r.phase = Phase::prefill;
        r.admit_order = ++admit_clock_;
        if (r.admissions++ == 0) {  // a request re-admitted after a preemption keeps its first-admission stats
            r.t_start = clk::now();
            r.t_chunk = r.t_start;
            r.result.cached_tokens = (int) reuse;
            r.result.prompt_tokens = (int) r.prompt_base;
            r.result.queue_s = secs_since(r.t_submit);
        }
        return true;
    }

    void finish(Request & r, const std::string & reason, const std::string & err = "") {
        r.phase = Phase::done;
        r.result.finish_reason = reason;
        r.result.error = err;
        r.result.completion_tokens = (int) r.generated.size();
        if (r.t_gen.time_since_epoch().count()) r.result.generate_s = secs_since(r.t_gen);
        common_chat_msg msg;
        try {
            msg = common_chat_parse(r.text, false, r.parser);
            msg.set_tool_call_ids(r.tool_ids, [&] { return "call_" + std::to_string(++call_ids_); });
        } catch (...) {
            msg.role = "assistant";
            msg.content = r.text;
        }
        // Whatever the throttled streaming parse has not shown yet goes out as one last delta.
        if (r.on_delta && (reason == "stop" || reason == "length")) {
            try {
                for (const auto & x : common_chat_msg_diff::compute_diffs(r.partial, msg)) {
                    r.pend.content += x.content_delta;
                    r.pend.reasoning += x.reasoning_content_delta;
                }
            } catch (...) {
            }
        }
        r.result.message = from_common(msg);
        if (reason == "stop" && !r.result.message.tool_calls.empty()) r.result.finish_reason = "tool_calls";
        total_completion_ += r.generated.size();
        total_prompt_ += r.prompt_base - std::min<size_t>(r.prompt_base, r.result.cached_tokens);
        total_requests_++;
        if (r.seq >= 0) {
            seqs_[r.seq].owner = nullptr;
            seqs_[r.seq].last_used = ++clock_;
        }
        // Not woken yet: the scheduler may still owe on_delta a last chunk, and the waiter frees `r` as soon
        // as it wakes. flush_completed() wakes it after those callbacks ran.
        completed_.push_back(&r);
    }

    // Wake the callers of finished requests. Scheduler thread only, mu_ not needed. The notify happens while
    // r.mu is held, so the waiter cannot return (and destroy the Request, and its mutex) before we are done.
    void flush_completed() {
        for (auto * r : completed_) {
            std::lock_guard lk(r->mu);
            r->finished = true;
            r->cv.notify_all();
        }
        completed_.clear();
    }

    void save_snapshot(Request & r) {
        std::vector<llama_token> toks(r.prompt.begin(), r.prompt.begin() + r.boundary);
        for (auto & s : snaps_)
            if (s.tokens == toks) {
                s.last_used = ++clock_;
                return;
            }
        Snapshot s;
        s.tokens = std::move(toks);
        s.data.resize(llama_state_seq_get_size(ctx_, r.seq));
        size_t n = llama_state_seq_get_data(ctx_, s.data.data(), s.data.size(), r.seq);
        if (n == 0) return;
        s.data.resize(n);
        s.last_used = ++clock_;
        if ((int) snaps_.size() >= std::max(1, cfg_.engine.prefix_cache)) {
            auto it = std::min_element(snaps_.begin(), snaps_.end(),
                                       [](const Snapshot & a, const Snapshot & b) { return a.last_used < b.last_used; });
            snaps_.erase(it);
        }
        snaps_.push_back(std::move(s));
    }

    void save_ckpt(Seq & q) {
        Ckpt c;
        c.n = q.tokens.size();
        c.data.resize(llama_state_seq_get_size_ext(ctx_, &q - seqs_.data(), LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY));
        size_t n = llama_state_seq_get_data_ext(ctx_, c.data.data(), c.data.size(), &q - seqs_.data(),
                                                LLAMA_STATE_SEQ_FLAGS_PARTIAL_ONLY);
        if (n == 0) return;
        c.data.resize(n);
        q.ckpts.push_back(std::move(c));
        while (q.ckpts.size() > 4) q.ckpts.erase(q.ckpts.begin());
    }

    void add_tok(llama_token t, llama_pos p, int seq, bool logits) {
        auto & b = batch_;
        b.token[b.n_tokens] = t;
        b.pos[b.n_tokens] = p;
        b.n_seq_id[b.n_tokens] = 1;
        b.seq_id[b.n_tokens][0] = seq;
        b.logits[b.n_tokens] = logits;
        b.n_tokens++;
    }

    // Parse the partial output and queue the new visible text and thinking in r.pend; the scheduler hands it to
    // on_delta after releasing mu_. Re-parsing everything per token is quadratic for a 16k-char tool call, so
    // past 2k chars it re-parses every size/16 chars (at most 256). finish() always parses the full text.
    void stream_partial(Request & r) {
        if (!r.on_delta) return;
        size_t every = r.text.size() < 2048 ? 1 : std::min<size_t>(256, r.text.size() / 16);
        if (r.text.size() - r.parsed_len < every) return;
        r.parsed_len = r.text.size();
        try {
            auto msg = common_chat_parse(r.text, true, r.parser);
            auto diffs = common_chat_msg_diff::compute_diffs(r.partial, msg);
            r.partial = msg;
            for (const auto & x : diffs) {
                r.pend.content += x.content_delta;
                r.pend.reasoning += x.reasoning_content_delta;
            }
        } catch (...) {
            // partial text that does not parse yet; the final parse decides
        }
    }

    // Hand queued deltas to their callbacks. No lock held: callbacks write SSE and emit agent events.
    void deliver(const std::vector<Request *> & touched) {
        for (auto * r : touched) {
            if (r->pend.content.empty() && r->pend.reasoning.empty()) continue;
            ChatDelta d = std::move(r->pend);
            r->pend = {};
            bool keep = true;
            try {
                keep = r->on_delta(d);
            } catch (...) {
            }
            if (!keep && r->phase != Phase::done) {
                std::lock_guard lk(mu_);
                finish(*r, "cancelled");
            }
        }
    }

    // Handle one sampled token: stream it, check stops. Returns false if the request finished.
    bool accept_token(Request & r, llama_token t) {
        common_sampler_accept(r.smpl, t, true);
        if (!r.t_gen.time_since_epoch().count()) {
            r.t_gen = clk::now();
            r.result.prefill_s = secs_since(r.t_start);
        }
        if (llama_vocab_is_eog(vocab_, t)) {
            finish(r, "stop");
            return false;
        }
        r.generated.push_back(t);
        rate_tokens_++;
        std::string p = piece(t);
        r.text += p;
        for (const auto & s : r.stops) {
            if (!s.empty() && r.text.size() >= s.size() && r.text.compare(r.text.size() - s.size(), s.size(), s) == 0) {
                r.text.resize(r.text.size() - s.size());
                finish(r, "stop");
                return false;
            }
        }
        stream_partial(r);
        if (r.cancel && r.cancel->load()) {
            finish(r, "cancelled");
            return false;
        }
        if ((int) r.generated.size() >= r.max_tokens ||
            (int) seqs_[r.seq].tokens.size() + 1 >= r.ctx_limit) {
            finish(r, "length");
            return false;
        }
        r.last = t;
        return true;
    }

    void loop() {
        rate_t0_ = clk::now();
        while (true) {
            std::vector<Request *> active;
            {
                std::unique_lock lk(mu_);
                cv_.wait(lk, [&] { return stop_ || !queue_.empty() || any_active(); });
                if (stop_) {
                    for (auto * r : queue_) finish(*r, "error", "engine stopped");
                    for (auto & q : seqs_)
                        if (q.owner) finish(*q.owner, "error", "engine stopped");
                    queue_.clear();
                    flush_completed();
                    return;
                }
                // Admit queued requests: foreground first, then FIFO. A request whose shared prefix is being
                // prefilled right now waits for that snapshot instead of prefilling the same tokens again.
                // A request that does not fit next to the live ones stays queued (and holds back the ones
                // behind it, so priority order is kept) until something finishes or is preempted.
                std::stable_sort(queue_.begin(), queue_.end(),
                                 [](Request * a, Request * b) { return a->req.priority < b->req.priority; });
                for (auto it = queue_.begin(); it != queue_.end();) {
                    Request * r = *it;
                    if (!kv_.can_ever_fit((int) r->prompt.size())) {  // a preempted request that outgrew the pool
                        finish(*r, "error", "sequence is " + std::to_string(r->prompt.size()) +
                                                " tokens, larger than the whole KV pool (ctx_total " +
                                                std::to_string(kv_.total()) + ")");
                        it = queue_.erase(it);
                        continue;
                    }
                    CtxDecision ctx = decide_ctx_admission((int) r->prompt.size(), mcfg_.ctx_per_agent,
                                                           mcfg_.ctx_per_agent_max, kv_, live_set());
                    if (!ctx.allowed) {  // over the hard cap: waiting cannot help
                        bool hard_cap = mcfg_.ctx_per_agent_max > mcfg_.ctx_per_agent;
                        finish(*r, "error", "sequence is " + std::to_string(r->prompt.size()) + " tokens, over " +
                                                (hard_cap ? "ctx_per_agent_max " : "ctx_per_agent ") +
                                                std::to_string(ctx.limit));
                        it = queue_.erase(it);
                        continue;
                    }
                    if (r->boundary && prefix_in_flight(*r)) {
                        ++it;
                        continue;
                    }
                    if (!ctx.fits) break;
                    if (!admit(*r)) break;
                    it = queue_.erase(it);
                }
                for (auto & q : seqs_)
                    if (q.owner) active.push_back(q.owner);
                roll_rate();
            }
            flush_completed();
            if (active.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            step(active);
        }
    }

    bool any_active() const {
        for (auto & q : seqs_)
            if (q.owner) return true;
        return false;
    }

    bool prefix_in_flight(const Request & r) const {
        for (auto & q : seqs_) {
            const Request * o = q.owner;
            if (o && o->phase == Phase::prefill && o->boundary == r.boundary && o->n_done < o->boundary &&
                std::equal(r.prompt.begin(), r.prompt.begin() + r.boundary, o->prompt.begin()))
                return true;
        }
        return false;
    }

    void roll_rate() {
        double w = secs_since(rate_t0_);
        if (w >= 5) {
            rate_prev_ = rate_tokens_ / w;
            rate_tokens_ = 0;
            rate_t0_ = clk::now();
        }
    }

    // Batch for one decode: every generating request's next token, plus one prefill chunk when the fairness
    // policy allows one (lowest priority number first). A step without a chunk is pure decode: the generators
    // run alone and their token rate does not pay for someone else's document.
    void build_batch(const std::vector<Request *> & active, std::vector<Request *> & gen, Request *& pre, size_t & pre_end) {
        batch_.n_tokens = 0;
        gen.clear();
        pre = nullptr;
        pre_end = 0;
        media_pending_ = nullptr;
        std::vector<PrefillCandidate> waiting;
        for (auto * r : active) {
            if (r->phase == Phase::generate) {
                r->idx = batch_.n_tokens;
                add_tok(r->last, (llama_pos) seqs_[r->seq].tokens.size(), r->seq, true);
                gen.push_back(r);
            } else if (r->phase == Phase::prefill) {
                waiting.push_back({r->seq, r->req.priority, (long long) (secs_since(r->t_chunk) * 1000.0)});
            }
        }
        PrefillPolicy policy;
        policy.duty = cfg_.engine.prefill_duty;
        policy.chunk_alone = mcfg_.batch;  // alone on the GPU, prefill at full batch
        policy.chunk_busy = cfg_.engine.busy_prefill_chunk();
        policy.max_wait_ms = cfg_.engine.prefill_max_wait_ms;
        PrefillChoice pick = choose_prefill(waiting, !gen.empty(), step_clock_, policy);
        if (pick.run)
            for (auto * r : active)
                if (r->phase == Phase::prefill && r->seq == pick.id) {
                    pre = r;
                    break;
                }
        if (pre && media_at(*pre, pre->n_done)) {
            // The next piece is an image: nothing of it goes in this batch, step() evaluates it after the decode.
            media_pending_ = pre;
            pre = nullptr;
        }
        if (pre) {
            size_t chunk = (size_t) std::max(1, pick.chunk);
            pre_end = std::min(pre->prompt.size(), pre->n_done + chunk);
            for (const auto & m : pre->media)
                if (m.start > pre->n_done && m.start < pre_end) pre_end = m.start;  // stop in front of an image
            if (pre->boundary > pre->n_done && pre->boundary < pre_end) pre_end = pre->boundary;  // stop at the snapshot point
            if (pre->ckpt_at > pre->n_done && pre->ckpt_at < pre_end) pre_end = pre->ckpt_at;     // and at the checkpoint
            for (size_t j = pre->n_done; j < pre_end; j++)
                add_tok(pre->prompt[j], (llama_pos) j, pre->seq, j == pre->prompt.size() - 1);
            pre->idx = batch_.n_tokens - 1;
        }
    }

    // Take a request off its sequence and put it back in the queue. It re-prefills from its prompt plus
    // everything generated so far (the sampler already saw those tokens), so generation continues where it
    // stopped. Hybrid models cannot cut KV mid-sequence, so the whole sequence is cleared rather than trimmed;
    // the shared-prefix snapshot makes the re-read cheap. Caller holds mu_.
    void preempt(Request & r) {
        drop_cache(r.seq);
        seqs_[r.seq].owner = nullptr;
        seqs_[r.seq].last_used = ++clock_;
        r.prompt.resize(r.prompt_base);
        r.prompt.insert(r.prompt.end(), r.generated.begin(), r.generated.end());
        r.phase = Phase::queued;
        r.seq = -1;
        r.n_done = 0;
        r.ckpt_at = 0;
        r.idx = -1;
        r.preempted++;
        preemptions_++;
        queue_.push_back(&r);
    }

    // A decode that failed for lack of KV space may have applied some of its micro-batches. Cut every live
    // sequence back to the tokens we know it holds; one that cannot be cut (recurrent state already advanced)
    // is preempted instead. Caller holds mu_.
    void roll_back_live(const std::vector<Request *> & active) {
        for (auto * r : active) {
            if (r->phase != Phase::generate && r->phase != Phase::prefill) continue;
            if (!llama_memory_seq_rm(mem_, r->seq, (llama_pos) seqs_[r->seq].tokens.size(), -1)) preempt(*r);
        }
    }

    // llama_decode returned 1: the unified KV pool is full. Free what is free (idle caches, prefix snapshots),
    // and if that is not enough preempt the least important live request and retry, until the batch fits.
    // Only a request that is alone and still does not fit fails, with a message saying why. Returns the final
    // decode result (0 = batch in gen/pre decoded) or -1 when nothing is left to decode. Caller holds lk.
    int recover_kv(std::unique_lock<std::mutex> & lk, std::vector<Request *> & active, std::vector<Request *> & gen,
                   Request *& pre, size_t & pre_end) {
        for (int i = 0; i < n_seq_; i++)
            if (!seqs_[i].owner && !seqs_[i].tokens.empty()) drop_cache(i);
        snaps_.clear();
        for (;;) {
            roll_back_live(active);
            build_batch(active, gen, pre, pre_end);
            if (batch_.n_tokens == 0) return -1;
            lk.unlock();
            int rc = llama_decode(ctx_, batch_);
            lk.lock();
            if (rc != 1) return rc;
            auto live = live_set();
            int v = kv_.pick_victim(live);
            if (v < 0) return -1;
            Request * victim = seqs_[live[v].id].owner;
            if (live.size() == 1) {
                finish(*victim, "error",
                       "KV pool exhausted: this sequence alone needs more than the pool holds (" +
                           std::to_string(live[0].tokens + live[0].pending) + " of ctx_total " +
                           std::to_string(kv_.total()) + " tokens); raise ctx_total or use a smaller kv_type");
                return -1;
            }
            preempt(*victim);
        }
    }

    // One decode: every generating request's next token, plus one prefill chunk (foreground first).
    void step(std::vector<Request *> & active) {
        std::vector<Request *> gen;
        Request * pre = nullptr;
        size_t pre_end = 0;
        ++step_clock_;
        build_batch(active, gen, pre, pre_end);

        int rc = batch_.n_tokens > 0 ? llama_decode(ctx_, batch_) : 0;  // an image-only step has no batch
        std::unique_lock lk(mu_);
        if (rc == 1) rc = recover_kv(lk, active, gen, pre, pre_end);
        if (rc != 0) {
            if (rc > 0)  // some other failure: nothing is known about the batch, fail what was in it
                for (auto * r : active)
                    if (r->phase != Phase::done && r->phase != Phase::queued)
                        finish(*r, "error", "llama_decode failed (" + std::to_string(rc) + ")");
            lk.unlock();
            flush_completed();
            return;
        }
        std::vector<Request *> touched;
        for (auto * r : gen) {
            seqs_[r->seq].tokens.push_back(r->last);
            llama_token t = common_sampler_sample(r->smpl, ctx_, r->idx);
            accept_token(*r, t);
            touched.push_back(r);
        }
        if (pre) {
            auto & q = seqs_[pre->seq];
            q.tokens.insert(q.tokens.end(), pre->prompt.begin() + pre->n_done, pre->prompt.begin() + pre_end);
            pre->n_done = pre_end;
            pre->t_chunk = clk::now();
            if (pre->boundary && pre->n_done == pre->boundary) save_snapshot(*pre);
            if (pre->ckpt_at && pre->n_done == pre->ckpt_at) save_ckpt(q);
            if (pre->cancel && pre->cancel->load()) {
                finish(*pre, "cancelled");
            } else if (pre->n_done == pre->prompt.size()) {
                pre->phase = Phase::generate;
                llama_token t = common_sampler_sample(pre->smpl, ctx_, pre->idx);
                accept_token(*pre, t);
            }
            touched.push_back(pre);
        }
        cv_.notify_all();
        // Callbacks (SSE writes, agent events) run without mu_, so state() and chat() never wait on a slow
        // client. Only this thread changes request phases, so nothing moves under us meanwhile.
        lk.unlock();
        deliver(touched);
        flush_completed();
        // An image is evaluated after the text work of this step (and after its callbacks), and without mu_:
        // the vision encoder can take a while and state() must stay answerable. Only this thread moves phases.
        if (media_pending_ && media_pending_->phase == Phase::prefill) eval_media(*media_pending_);
        media_pending_ = nullptr;
    }

    // Encode and decode the image chunk that starts at r.n_done into r's sequence.
    void eval_media(Request & r) {
        const Request::Media * m = media_at(r, r.n_done);
        if (!m) return;
        if (r.cancel && r.cancel->load()) {
            std::lock_guard lk(mu_);
            finish(r, "cancelled");
            flush_completed();
            return;
        }
        llama_pos new_past = (llama_pos) r.n_done;
        int32_t rc = mtmd_helper_eval_chunk_single(mctx_, ctx_, m->chunk, (llama_pos) r.n_done, r.seq, mcfg_.batch, false,
                                                   &new_past);
        {
            std::lock_guard lk(mu_);
            auto & q = seqs_[r.seq];
            if (rc != 0) {
                // drop whatever of the image reached the KV; a sequence that cannot be cut is cleared
                if (!llama_memory_seq_rm(mem_, r.seq, (llama_pos) q.tokens.size(), -1)) drop_cache(r.seq);
                finish(r, "error", "image did not fit or failed to encode (rc " + std::to_string(rc) + ")");
            } else if (m->start + m->n_pos >= r.prompt.size()) {
                finish(r, "error", "the prompt ends with an image");
            } else {
                q.tokens.insert(q.tokens.end(), r.prompt.begin() + m->start, r.prompt.begin() + m->start + m->n_pos);
                r.n_done = m->start + m->n_pos;
                r.t_chunk = clk::now();
                if (r.cancel && r.cancel->load()) finish(r, "cancelled");
            }
        }
        flush_completed();
    }

    const Config & cfg_;
    const ModelConfig & mcfg_;
    llama_model_tensor_buft_override overrides_[2] = {{nullptr, nullptr}, {nullptr, nullptr}};
    llama_model * model_ = nullptr;
    llama_context * ctx_ = nullptr;
    const llama_vocab * vocab_ = nullptr;
    llama_memory_t mem_ = nullptr;
    common_chat_templates_ptr tmpls_;
    llama_batch batch_{};
    mtmd_context * mctx_ = nullptr;
    Request * media_pending_ = nullptr;  // prefill request whose next piece is an image (set by build_batch)
    std::string path_;
    bool hybrid_ = false;
    int n_seq_ = 1;

    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Request *> queue_;
    std::vector<Seq> seqs_;
    std::list<Snapshot> snaps_;
    KvBudget kv_{1, 0};
    std::vector<Request *> completed_;   // finished, caller not yet woken (scheduler thread only)
    std::thread thread_;
    bool stop_ = false;
    long long clock_ = 0, call_ids_ = 0, admit_clock_ = 0, preemptions_ = 0;
    long long step_clock_ = 0;           // decode steps taken, drives the prefill duty pattern

    clk::time_point rate_t0_;
    long long rate_tokens_ = 0;
    double rate_prev_ = 0;
    long long total_prompt_ = 0, total_completion_ = 0, total_requests_ = 0;
};

}  // namespace

// ---------- remote engine (OpenAI-compatible upstream) ----------
// One request at a time (the upstream may serve one sequence); agents wait in a priority queue (lead first, FIFO within
// a priority) and a request cancelled while queued leaves without ever reaching the upstream. Streams SSE through.
class RemoteEngine : public Engine {
public:
    explicit RemoteEngine(const Config & cfg) : m_(cfg.active_model()), name_(cfg.engine.model) {
        auto u = m_.url;
        auto p = u.find("://");
        if (p != std::string::npos) u = u.substr(p + 3);
        auto c = u.find(':');
        host_ = c == std::string::npos ? u : u.substr(0, c);
        port_ = c == std::string::npos ? 80 : std::atoi(u.substr(c + 1).c_str());
        if (!m_.unit.empty()) {
            std::string cmd = "systemctl --user start " + m_.unit;
            if (std::system(cmd.c_str()) != 0) std::fprintf(stderr, "judas: could not start %s\n", m_.unit.c_str());
        }
        for (int i = 0; i < 120; i++) {  // wait for the upstream API (lazy servers answer before loading)
            httplib::Client cl(host_, port_);
            cl.set_connection_timeout(1, 0);
            if (auto r = cl.Get("/health"); r && r->status == 200) return;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        throw std::runtime_error("remote model " + name_ + " not reachable at " + m_.url);
    }

    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> & on_delta,
                    const std::atomic<bool> * cancel) override {
        if (!turn_.acquire(req.priority, cancel)) {
            ChatResult c;
            c.finish_reason = "cancelled";
            return c;
        }
        struct Release {
            TicketQueue & q;
            ~Release() { q.release(); }
        } release{turn_};
        active_ = 1;
        auto t0 = std::chrono::steady_clock::now();
        ChatResult res;
        json msgs = json::array();
        for (const auto & m : req.messages) {
            json j = {{"role", m.role}, {"content", m.content}};
            if (!m.images.empty() && m_.vision) {
                json parts = json::array();
                parts.push_back({{"type", "text"}, {"text", m.content}});
                for (const auto & img : m.images)
                    parts.push_back({{"type", "image_url"},
                                     {"image_url", {{"url", std::string("data:") + sniff_image_mime(img) + ";base64," + b64_encode(img)}}}});
                j["content"] = parts;
            }
            if (!m.tool_calls.empty()) {
                json tcs = json::array();
                for (const auto & tc : m.tool_calls)
                    tcs.push_back({{"id", tc.id}, {"type", "function"},
                                   {"function", {{"name", tc.name},
                                                 {"arguments", json::accept(tc.arguments) ? tc.arguments : "{}"}}}});
                j["tool_calls"] = tcs;
            }
            if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
            msgs.push_back(j);
        }
        json body = {{"model", m_.remote_model.empty() ? name_ : m_.remote_model}, {"messages", msgs},
                     {"stream", true}, {"stream_options", {{"include_usage", true}}}};
        if (!req.tools.empty()) {
            json ts = json::array();
            for (const auto & t : req.tools)
                ts.push_back({{"type", "function"},
                              {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
            body["tools"] = ts;
        }
        float temp = req.sampling.temp >= 0 ? req.sampling.temp : m_.temp;
        float top_p = req.sampling.top_p >= 0 ? req.sampling.top_p : m_.top_p;
        body["temperature"] = temp;
        body["top_p"] = top_p;
        if (req.sampling.max_tokens > 0) body["max_tokens"] = req.sampling.max_tokens;

        std::string buf, finish = "stop";
        std::vector<ChatMessage::ToolCall> calls;
        bool stopped = false;
        auto handle = [&](const std::string & line) {
            if (line.rfind("data: ", 0) != 0) return;
            std::string d = line.substr(6);
            if (d == "[DONE]") return;
            json j = json::parse(d, nullptr, false);
            if (j.is_discarded()) return;
            if (j.contains("usage") && j["usage"].is_object()) {
                res.prompt_tokens = j["usage"].value("prompt_tokens", 0);
                res.completion_tokens = j["usage"].value("completion_tokens", 0);
            }
            if (!j.contains("choices") || j["choices"].empty()) return;
            const auto & ch = j["choices"][0];
            if (ch.contains("finish_reason") && ch["finish_reason"].is_string()) finish = ch["finish_reason"];
            if (!ch.contains("delta")) return;
            const auto & dl = ch["delta"];
            ChatDelta out;
            if (dl.contains("content") && dl["content"].is_string()) out.content = dl["content"];
            for (const char * k : {"reasoning_content", "reasoning"})
                if (dl.contains(k) && dl[k].is_string()) out.reasoning += dl[k].get<std::string>();
            if (dl.contains("tool_calls"))
                for (const auto & tc : dl["tool_calls"]) {
                    size_t i = tc.value("index", (int) calls.size());
                    if (calls.size() <= i) calls.resize(i + 1);
                    if (tc.contains("id") && tc["id"].is_string()) calls[i].id = tc["id"];
                    if (tc.contains("function")) {
                        const auto & f = tc["function"];
                        if (f.contains("name") && f["name"].is_string()) calls[i].name += f["name"].get<std::string>();
                        if (f.contains("arguments") && f["arguments"].is_string())
                            calls[i].arguments += f["arguments"].get<std::string>();
                    }
                }
            res.message.content += out.content;
            res.message.reasoning += out.reasoning;
            if ((!out.content.empty() || !out.reasoning.empty()) && on_delta && !on_delta(out)) stopped = true;
        };
        httplib::Client cl(host_, port_);
        cl.set_read_timeout(1800, 0);
        httplib::Request hr;
        hr.method = "POST";
        hr.path = "/v1/chat/completions";
        hr.headers = {{"Content-Type", "application/json"}};
        hr.body = body.dump();
        hr.content_receiver = [&](const char * data, size_t len, uint64_t, uint64_t) {
            buf.append(data, len);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                std::string line = buf.substr(0, nl);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                buf.erase(0, nl + 1);
                handle(line);
            }
            return !stopped && !(cancel && cancel->load());
        };
        auto r = cl.send(hr);
        active_ = 0;
        res.generate_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (stopped || (cancel && cancel->load())) {
            res.finish_reason = "cancelled";
        } else if (!r || r->status != 200) {
            res.finish_reason = "error";
            res.error = "remote " + m_.url + ": " + (r ? "HTTP " + std::to_string(r->status) + " " + r->body.substr(0, 300)
                                                         : httplib::to_string(r.error()));
            return res;
        } else {
            res.finish_reason = calls.empty() ? finish : "tool_calls";
        }
        int n = 0;
        for (auto & c : calls) {
            if (c.name.empty()) continue;
            if (c.id.empty()) c.id = "call_r" + std::to_string(n);
            n++;
            res.message.tool_calls.push_back(c);
        }
        res.message.role = "assistant";
        total_p_ += res.prompt_tokens;
        total_c_ += res.completion_tokens;
        requests_++;
        if (res.generate_s > 0) last_tps_ = res.completion_tokens / res.generate_s;
        return res;
    }

    EngineState state() const override {
        EngineState s;
        s.model = name_;
        s.model_path = m_.url;
        s.loaded = true;
        s.max_agents = 1;
        s.active = active_;
        s.queued = (int) turn_.waiting();
        s.ctx_total = m_.ctx_total;
        s.gen_tok_s = last_tps_;
        s.total_prompt_tokens = total_p_;
        s.total_completion_tokens = total_c_;
        s.requests = requests_;
        return s;
    }
    std::string model_name() const override { return name_; }
    int count_tokens(const std::string & text) const override { return (int) (text.size() / 3.5) + 1; }
    bool vision() const override { return m_.vision; }

private:
    ModelConfig m_;
    std::string name_, host_;
    int port_ = 80;
    TicketQueue turn_;
    std::atomic<int> active_{0};
    std::atomic<long long> total_p_{0}, total_c_{0}, requests_{0};
    double last_tps_ = 0;
};

std::unique_ptr<Engine> Engine::create(const Config & cfg) {
    const auto & m = cfg.active_model();
    if (!m.url.empty()) return std::make_unique<RemoteEngine>(cfg);
    // a local model needs the GPU: stop any remote model's unit first so its VRAM is freed
    for (const auto & [k, other] : cfg.models)
        if (!other.unit.empty()) std::system(("systemctl --user stop " + other.unit + " 2>/dev/null").c_str());
    return std::make_unique<LlamaEngine>(cfg);
}

std::vector<ChatMessage> messages_from_oai(const json & messages) {
    std::vector<ChatMessage> out;
    for (const auto & m : messages) {
        ChatMessage c;
        c.role = m.value("role", "user");
        if (m.contains("content")) {
            const auto & ct = m["content"];
            if (ct.is_string()) {
                c.content = ct.get<std::string>();
            } else if (ct.is_array()) {
                for (const auto & part : ct) {
                    std::string type = part.value("type", "");
                    if (type == "text") {
                        c.content += part.value("text", "");
                    } else if (type == "image_url" && part.contains("image_url")) {
                        // only inline data: URLs are used; the engine never fetches a remote URL
                        const auto & iu = part["image_url"];
                        std::string url = iu.is_string() ? iu.get<std::string>() : iu.value("url", "");
                        size_t comma = url.find(',');
                        if (url.rfind("data:", 0) == 0 && comma != std::string::npos &&
                            url.substr(0, comma).find(";base64") != std::string::npos) {
                            std::string bytes;
                            if (b64_decode(url.substr(comma + 1), bytes) && !bytes.empty()) c.images.push_back(std::move(bytes));
                        }
                    }
                }
            }
        }
        c.reasoning = m.value("reasoning_content", "");
        if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
            for (const auto & tc : m["tool_calls"]) {
                ChatMessage::ToolCall t;
                t.id = tc.value("id", "");
                if (tc.contains("function")) {
                    t.name = tc["function"].value("name", "");
                    const auto & a = tc["function"]["arguments"];
                    t.arguments = a.is_string() ? a.get<std::string>() : a.dump();
                }
                c.tool_calls.push_back(t);
            }
        }
        c.tool_call_id = m.value("tool_call_id", "");
        c.tool_name = m.value("name", "");
        out.push_back(std::move(c));
    }
    return out;
}

std::vector<ToolSpec> tools_from_oai(const json & tools) {
    std::vector<ToolSpec> out;
    for (const auto & t : tools) {
        const auto & f = t.contains("function") ? t["function"] : t;
        out.push_back({f.value("name", ""), f.value("description", ""), f.value("parameters", json::object())});
    }
    return out;
}

json message_to_oai(const ChatMessage & m) {
    json j = {{"role", m.role}, {"content", m.content}};
    if (!m.reasoning.empty()) j["reasoning_content"] = m.reasoning;
    if (!m.tool_calls.empty()) {
        j["tool_calls"] = json::array();
        for (const auto & tc : m.tool_calls)
            j["tool_calls"].push_back(
                {{"id", tc.id}, {"type", "function"}, {"function", {{"name", tc.name}, {"arguments", tc.arguments}}}});
    }
    if (!m.tool_call_id.empty()) j["tool_call_id"] = m.tool_call_id;
    return j;
}

}  // namespace judas
