// judas models: names, fit, and writing [models.x] blocks into a config file.
#include <filesystem>
#include <fstream>
#include <sstream>

#include "core/config.h"
#include "models/models.h"
#include <unistd.h>

#include "test.h"

using namespace judas;
namespace fs = std::filesystem;

static std::string slurp(const fs::path & p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

int main() {
    CHECK(suggest_model_name("Qwen3-30B-A3B-Q4_K_M.gguf") == "qwen3-30b-a3b");
    CHECK(suggest_model_name("gemma-4-E4B-it-UD-Q4_K_XL.gguf") == "gemma-4-e4b-it");
    CHECK(suggest_model_name("Big-Model-IQ2_XS-00001-of-00003.gguf") == "big-model");
    CHECK(suggest_model_name("ollama:llama3.2:3b") == "llama3-2-3b");
    CHECK(suggest_model_name("model.gguf") == "model");
    CHECK(suggest_model_name("a-very-long-model-name-that-keeps-going-and-going-Q8_0.gguf").size() <= 40);

    const uint64_t G = 1024ull * 1024 * 1024;
    CHECK(fit_verdict(5 * G, 16 * G) == "fits");
    CHECK(fit_verdict(13 * G, 16 * G) == "tight" || fit_verdict(13 * G, 16 * G) == "too big");
    CHECK(fit_verdict(20 * G, 16 * G) == "too big");
    CHECK(fit_verdict(5 * G, 0) == "?");

    fs::path root = fs::path(JUDAS_TEST_TMP) / ("judas-models-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path model = root / "Tiny-Q4_K_M.gguf";
    { std::ofstream(model) << "not really a gguf"; }
    GgufInfo m;
    m.path = model.string();
    m.label = "Tiny-Q4_K_M.gguf";
    m.ok = true;
    m.bytes = 400ull * 1024 * 1024;
    m.ctx_train = 8192;

    // a fresh config is created from the starter template, and the missing-file placeholder [models.main] is replaced
    fs::path cfgp = root / "config.toml";
    CHECK(add_model_to_config(cfgp.string(), "main", m, 16 * G).empty());
    std::string text = slurp(cfgp);
    CHECK(text.find("path = \"" + model.string() + "\"") != std::string::npos);
    CHECK(text.find("ctx_per_agent = 8192") != std::string::npos);   // capped at the trained context
    size_t first = text.find("[models.main]");
    CHECK(first != std::string::npos && text.find("[models.main]", first + 1) == std::string::npos);
    setenv("JUDAS_CONFIG", cfgp.c_str(), 1);
    Config c = load_config(root.string());
    CHECK(c.models.count("main") && c.models["main"].path == model.string());
    CHECK(c.models["main"].temp > 0.69f && c.models["main"].temp < 0.71f);

    // a name already pointing at a real file is refused; bad names and projectors too
    CHECK(!add_model_to_config(cfgp.string(), "main", m, 16 * G).empty());
    CHECK(!add_model_to_config(cfgp.string(), "bad name", m, 16 * G).empty());
    GgufInfo proj = m;
    proj.projector = true;
    CHECK(!add_model_to_config(cfgp.string(), "vision", proj, 16 * G).empty());
    CHECK(add_model_to_config(cfgp.string(), "second", m, 16 * G).empty());
    CHECK(set_engine_model_in_file(cfgp.string(), "second"));
    c = load_config(root.string());
    CHECK(c.engine.model == "second" && c.models.count("second"));

    // too big for the GPU: an MoE keeps experts on the CPU
    m.bytes = 30 * G;
    m.experts = 128;
    CHECK(model_block("big", m, 16 * G).find("cpu_moe = true") != std::string::npos);

    // scanning finds a file in a JUDAS_MODEL_DIRS folder (unreadable header is reported, not fatal)
    setenv("JUDAS_MODEL_DIRS", root.c_str(), 1);
    Config empty;
    empty.models.clear();
    bool found = false;
    for (auto & f : scan_models(empty))
        if (f.path == model.string()) found = !f.ok && !f.error.empty();
    CHECK(found);

    fs::remove_all(root);
    TEST_MAIN_END();
}
