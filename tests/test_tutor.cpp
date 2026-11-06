// Learn mode: context file globs and digest, read-only guarantees, the progress record, tool gating, config.
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "core/config.h"
#include "test.h"
#include "tools/tools.h"
#include "tutor/tutor.h"

using namespace judas;
namespace fs = std::filesystem;

static std::string slurp(const fs::path & p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-tutor-test-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::create_directories(root);
    // copy the fixture so write attempts can never touch the repo
    fs::path fx = root / "planner";
    fs::copy(fs::path(JUDAS_TESTS_DIR) / "fixtures" / "learn", fx, fs::copy_options::recursive);

    Config cfg;
    cfg.data_dir = (root / "data").string();
    cfg.permissions.allow_paths = {root.string()};
    cfg.learn.context_files = {(fx / "goals.md").string(), (fx / "notes.md").string(), (fx / "events.json").string(),
                               (fx / "notes-files" / "*.md").string(), (fx / "missing-*.md").string()};

    // globs expand to existing regular files only, sorted, de-duplicated
    auto paths = learn_context_paths(cfg);
    CHECK(paths.size() == 4);
    cfg.learn.context_files.push_back((fx / "goals.md").string());
    CHECK(learn_context_paths(cfg).size() == 4);
    // a directory entry stands for its direct text files
    Config dir = cfg;
    dir.learn.context_files = {fx.string()};
    CHECK(learn_context_paths(dir).size() == 4);   // goals, notes, events, status
    Config none;
    CHECK(learn_context_paths(none).empty());
    CHECK(learn_context_digest(none).empty());

    // digest: heads of every file, capped
    std::string dg = learn_context_digest(cfg);
    CHECK(dg.find("### goals.md") != std::string::npos);
    CHECK(dg.find("hash table") != std::string::npos);
    CHECK(dg.find("### hash-tables.md") != std::string::npos);
    Config tiny = cfg;
    tiny.learn.max_context_tokens = 100;
    CHECK(learn_context_digest(tiny).size() < 1200);
    tiny.learn.max_context_tokens = 0;
    CHECK(learn_context_digest(tiny).empty());

    // search and read only reach listed files
    auto hits = learn_context_search(cfg, "FOURIER");
    CHECK(hits.size() == 2);
    CHECK(hits[0].rfind("goals.md:", 0) == 0);
    std::string err;
    CHECK(learn_context_read(cfg, "notes.md", 5000, &err).find("Calculus exam") != std::string::npos);
    CHECK(learn_context_read(cfg, "status.json", 5000, &err).empty() && !err.empty());   // exists but not listed
    err.clear();
    CHECK(learn_context_read(cfg, "/etc/passwd", 5000, &err).empty() && !err.empty());

    // read-only: the write path check refuses a context file, still allows a sibling
    ToolContext ctx;
    ctx.cfg = &cfg;
    ctx.cwd = root.string();
    std::string resolved;
    CHECK(!check_write_path(ctx, (fx / "goals.md").string(), resolved).empty());
    CHECK(!check_write_path(ctx, (fx / "notes-files" / "hash-tables.md").string(), resolved).empty());
    CHECK(check_write_path(ctx, (fx / "status.json").string(), resolved).empty());
    std::string before = slurp(fx / "goals.md");
    ToolRegistry reg;
    register_builtin_tools(reg);
    auto w = reg.call("write_file", {{"path", (fx / "goals.md").string()}, {"content", "x"}}, ctx);
    CHECK(!w.ok);
    auto e = reg.call("edit_file", {{"path", (fx / "goals.md").string()}, {"old_string", "Goals"}, {"new_string", "X"}}, ctx);
    CHECK(!e.ok);
    CHECK(slurp(fx / "goals.md") == before);

    // tools
    register_learn_tools(reg);
    auto lst = reg.call("learner_context", json::object(), ctx);
    CHECK(lst.ok && lst.output.find("goals.md") != std::string::npos);
    CHECK(reg.call("learner_context", {{"query", "chaining"}}, ctx).output.find("hash-tables.md:") != std::string::npos);
    CHECK(!reg.call("learner_context", {{"file", "status.json"}}, ctx).ok);

    // learn_progress is gone from the toolset
    CHECK(!reg.call("learn_progress", {{"action", "show"}}, ctx).ok);
    CHECK(fs::path(learn_progress_path(cfg)).string().rfind(cfg.data_dir, 0) == 0);

    // tool gating for Learn sessions: no writes, shell or MCP
    CHECK(learn_tool_allowed("learner_context") && learn_tool_allowed("memory_search") && learn_tool_allowed("read_file"));
    CHECK(!learn_tool_allowed("write_file") && !learn_tool_allowed("edit_file") && !learn_tool_allowed("shell"));
    CHECK(!learn_tool_allowed("snapshot_revert"));
    CHECK(std::string(learn_prompt()).find("\xe2\x80\x94") == std::string::npos);   // no em dash

    // config
    fs::path cf = root / "c.toml";
    std::ofstream(cf) << "[learn]\ncontext_files = [\"~/a/*.md\", \"/b/c.txt\"]\nmax_context_tokens = 1234\n";
    setenv("JUDAS_CONFIG", cf.c_str(), 1);
    Config lc = load_config(root.string());
    unsetenv("JUDAS_CONFIG");
    CHECK(lc.learn.context_files.size() == 2 && lc.learn.max_context_tokens == 1234);
    CHECK(Config().learn.context_files.empty() && Config().learn.max_context_tokens == 3000);
    CHECK(dump_config(lc).find("max_context_tokens = 1234") != std::string::npos);

    fs::remove_all(root);
    TEST_MAIN_END();
}
