#include "models/moefit.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <regex>

#include "gguf.h"

namespace judas {

uint64_t MoeLayout::expert_total() const {
    uint64_t s = 0;
    for (auto b : expert_bytes) s += b;
    return s;
}

namespace {

uint64_t kv_u(const gguf_context * g, const std::string & key) {
    int64_t id = gguf_find_key(g, key.c_str());
    if (id < 0) return 0;
    switch (gguf_get_kv_type(g, id)) {
        case GGUF_TYPE_UINT32: return gguf_get_val_u32(g, id);
        case GGUF_TYPE_INT32: return (uint64_t) std::max(0, gguf_get_val_i32(g, id));
        case GGUF_TYPE_UINT64: return gguf_get_val_u64(g, id);
        case GGUF_TYPE_UINT16: return gguf_get_val_u16(g, id);
        case GGUF_TYPE_ARRAY: {   // per-layer array (head_count_kv on some models): the largest entry
            uint64_t best = 0;
            size_t n = gguf_get_arr_n(g, id);
            auto t = gguf_get_arr_type(g, id);
            const void * d = gguf_get_arr_data(g, id);
            for (size_t i = 0; i < n; i++) {
                uint64_t v = 0;
                if (t == GGUF_TYPE_UINT32) v = ((const uint32_t *) d)[i];
                else if (t == GGUF_TYPE_INT32) v = (uint64_t) std::max(0, ((const int32_t *) d)[i]);
                best = std::max(best, v);
            }
            return best;
        }
        default: return 0;
    }
}

std::string arch_of(const gguf_context * g) {
    int64_t id = gguf_find_key(g, "general.architecture");
    return id >= 0 ? gguf_get_val_str(g, id) : "";
}

// all parts of a split model given part 1; a plain file gives itself
std::vector<std::string> parts_of(const std::string & path) {
    static const std::regex re(R"((.*)-(\d{5})-of-(\d{5})\.gguf$)", std::regex::icase);
    std::smatch sm;
    if (!std::regex_match(path, sm, re)) return {path};
    int total = std::stoi(sm[3].str());
    std::vector<std::string> out;
    for (int i = 1; i <= total; i++) {
        char suf[64];
        std::snprintf(suf, sizeof suf, "-%05d-of-%05d.gguf", i, total);
        out.push_back(sm[1].str() + suf);
    }
    return out;
}

double bytes_per_elem(ggml_type t) { return (double) ggml_row_size(t, 256) / 256.0; }

}  // namespace

MoeLayout read_moe_layout(const std::string & path, int ctx_total, ggml_type type_k, ggml_type type_v) {
    MoeLayout l;
    static const std::regex blk(R"(^blk\.(\d+)\.(.*)$)");
    uint64_t n_kv = 0, key_len = 0, val_len = 0;
    std::vector<bool> has_k, has_qkv;
    bool has_output = false;
    uint64_t tok_embd = 0;
    bool first = true;
    for (const auto & p : parts_of(path)) {
        gguf_init_params ip{true, nullptr};
        gguf_context * g = gguf_init_from_file(p.c_str(), ip);
        if (!g) {
            l.error = "cannot read " + p;
            return l;
        }
        if (first) {
            std::string a = arch_of(g);
            l.n_layers = (int) kv_u(g, a + ".block_count");
            uint64_t heads = kv_u(g, a + ".attention.head_count");
            n_kv = kv_u(g, a + ".attention.head_count_kv");
            if (!n_kv) n_kv = heads;
            key_len = kv_u(g, a + ".attention.key_length");
            val_len = kv_u(g, a + ".attention.value_length");
            uint64_t emb = kv_u(g, a + ".embedding_length");
            if (!key_len && heads) key_len = emb / heads;
            if (!val_len) val_len = key_len;
            l.expert_bytes.assign(std::max(l.n_layers, 0), 0);
            has_k.assign(std::max(l.n_layers, 0), false);
            has_qkv.assign(std::max(l.n_layers, 0), false);
            first = false;
        }
        int64_t nt = gguf_get_n_tensors(g);
        for (int64_t i = 0; i < nt; i++) {
            std::string name = gguf_get_tensor_name(g, i);
            uint64_t sz = gguf_get_tensor_size(g, i);
            std::smatch sm;
            if (std::regex_match(name, sm, blk)) {
                int layer = std::stoi(sm[1].str());
                const std::string rest = sm[2].str();
                if (rest.find("_exps") != std::string::npos && layer < (int) l.expert_bytes.size()) {
                    l.expert_bytes[layer] += sz;
                    continue;
                }
                if (rest == "attn_k.weight" && layer < (int) has_k.size()) has_k[layer] = true;
                if (rest == "attn_qkv.weight" && layer < (int) has_qkv.size()) has_qkv[layer] = true;
            } else if (name == "per_layer_token_embd.weight") {
                l.cpu_fixed_bytes += sz;
                continue;
            } else if (name == "token_embd.weight") {
                tok_embd = sz;
                continue;
            } else if (name == "output.weight") {
                has_output = true;
            }
            l.other_bytes += sz;
        }
        gguf_free(g);
    }
    if (l.n_layers <= 0) {
        l.error = "no block_count in the GGUF";
        return l;
    }
    // token_embd lives in RAM (input layer); with tied embeddings the output projection reuses it on the GPU
    if (has_output) l.cpu_fixed_bytes += tok_embd;
    else l.other_bytes += tok_embd;
    // layers with KV: those with a separate attn_k (hybrid models also have attn_qkv on their recurrent layers)
    for (bool b : has_k) l.attn_layers += b;
    if (l.attn_layers == 0)
        for (bool b : has_qkv) l.attn_layers += b;
    if (l.attn_layers == 0) l.attn_layers = l.n_layers;   // unknown layout: assume every layer has KV
    double per_tok = (double) n_kv * ((double) key_len * bytes_per_elem(type_k) + (double) val_len * bytes_per_elem(type_v));
    l.kv_bytes = (uint64_t) (per_tok * (double) l.attn_layers * (double) ctx_total);
    l.ok = true;
    return l;
}

MoeFit moe_fit(const MoeLayout & l, uint64_t free_vram, uint64_t margin, uint64_t compute) {
    MoeFit f;
    uint64_t total = l.expert_total();
    f.moe = total > 0;
    uint64_t base = l.other_bytes + l.kv_bytes + compute;
    f.base_fits = free_vram > base + margin;
    f.budget = f.base_fits ? free_vram - base - margin : 0;
    int n = (int) l.expert_bytes.size();
    if (!f.moe) {
        f.all_gpu = true;
        f.gpu_total = base;
        return f;
    }
    if (total <= f.budget) {
        f.all_gpu = true;
        f.gpu_expert_layers = n;
        f.gpu_expert_bytes = total;
        f.gpu_total = base + total;
        return f;
    }
    // last layers first, a contiguous suffix; stop at the first layer that no longer fits
    uint64_t left = f.budget;
    int first_gpu = n;
    for (int i = n - 1; i >= 0; i--) {
        if (l.expert_bytes[i] > left) break;
        left -= l.expert_bytes[i];
        f.gpu_expert_bytes += l.expert_bytes[i];
        first_gpu = i;
    }
    f.cpu_layers = first_gpu;
    f.gpu_expert_layers = n - first_gpu;
    f.cpu_expert_bytes = total - f.gpu_expert_bytes;
    f.gpu_total = base + f.gpu_expert_bytes;
    return f;
}

std::string moe_cpu_pattern(int n) {
    std::string alt;
    for (int i = 0; i < n; i++) alt += (i ? "|" : "") + std::to_string(i);
    return "blk\\.(" + alt + ")\\.ffn_(up|down|gate)_exps";
}

int moe_mode_value(const std::string & v) {
    if (v == "false") return 0;
    if (v == "true") return -1;
    if (v == "auto" || v.empty()) return -2;
    char * end = nullptr;
    long n = std::strtol(v.c_str(), &end, 10);
    if (*end != '\0' || n < 0) return -3;
    return (int) n;
}

std::string moe_fit_summary(const MoeFit & f, int n_layers, uint64_t free_vram) {
    auto gb = [](uint64_t b) {
        char s[32];
        std::snprintf(s, sizeof s, "%.1f GB", (double) b / 1e9);
        return std::string(s);
    };
    if (!f.moe) return "no routed experts";
    if (f.all_gpu)
        return "all experts fit on the GPU (" + gb(f.gpu_expert_bytes) + "), GPU total " + gb(f.gpu_total) + " of " +
               gb(free_vram) + " free";
    std::string s = "experts of layers 0.." + std::to_string(f.cpu_layers - 1) + " in RAM (" + gb(f.cpu_expert_bytes) + ")";
    if (f.gpu_expert_layers > 0)
        s += ", " + std::to_string(f.cpu_layers) + ".." + std::to_string(n_layers - 1) + " on the GPU (" +
             gb(f.gpu_expert_bytes) + ")";
    else
        s += ", none on the GPU";
    s += "; GPU total " + gb(f.gpu_total) + " of " + gb(free_vram) + " free";
    if (!f.base_fits) s += " (WARNING: non-expert weights and KV alone exceed free VRAM)";
    return s;
}

}  // namespace judas
