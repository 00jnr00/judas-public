// Shared memory: section parsing, search, append (dash rule, placement, locking), prompt block.
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <thread>

#include "test.h"
#include "tools/memory_tools.h"

using namespace judas;
namespace fs = std::filesystem;

static std::string slurp(const fs::path & p) {
    std::ifstream f(p);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

static const char * SAMPLE =
    "# Shared memory\n\nintro line\n\n"
    "## Rules\n\n- always run the tests\n- keep it short\n\n"
    "## How to work\n\n- 2026-09-25 [notes] plans come first\n\n### Sub heading\n- sub bullet about CAD\n\n"
    "## Projects\n\n- Widget: the CLI\n\n"
    "## Log\n\n- 2026-10-01 [notes] something happened\n";

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-memory-test-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::create_directories(root);
    fs::path file = root / "SHARED.md";
    { std::ofstream(file) << SAMPLE; }

    // pure helpers
    auto names = memory_section_names(SAMPLE);
    CHECK((names == std::vector<std::string>{"Rules", "How to work", "Projects", "Log"}));
    CHECK(memory_section_text(SAMPLE, "rules") == "## Rules\n\n- always run the tests\n- keep it short\n");
    CHECK(memory_section_text(SAMPLE, "How to work").find("sub bullet about CAD") != std::string::npos);
    CHECK(memory_section_text(SAMPLE, "Nope").empty());
    auto hits = memory_search_text(SAMPLE, "CAD");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0] == "[How to work] - sub bullet about CAD");
    CHECK(memory_search_text(SAMPLE, "zzz").empty());
    CHECK(!memory_check_fact("plain fact, with comma").size());
    CHECK(!memory_check_fact("em \xE2\x80\x94 dash").empty());
    CHECK(!memory_check_fact("en \xE2\x80\x93 dash").empty());

    std::string ins = memory_insert_bullet(SAMPLE, "Projects", "- NEW");
    CHECK(ins.find("- Widget: the CLI\n- NEW\n\n## Log") != std::string::npos);
    std::string mk = memory_insert_bullet(SAMPLE, "Ideas", "- IDEA");
    CHECK(mk.find("## Ideas\n\n- IDEA\n\n## Log") != std::string::npos);   // before Log
    std::string nolog = memory_insert_bullet("## A\n\n- x\n", "B", "- y");
    CHECK(nolog == "## A\n\n- x\n\n## B\n\n- y\n");
    std::string empty = memory_insert_bullet("## A\n\n## B\n\n- z\n", "A", "- q");
    CHECK(empty.find("## A\n\n- q\n\n## B") != std::string::npos);
    CHECK(memory_insert_bullet("", "Log", "- l") == "## Log\n\n- l\n");

    // tools
    Config cfg;
    cfg.memory.path = file.string();
    cfg.memory.reject_dashes = true;
    ToolRegistry reg;
    register_memory_tools(reg);
    CHECK(reg.find("memory_read") && reg.find("memory_search") && reg.find("memory_append"));
    ToolContext ctx;
    ctx.cfg = &cfg;
    auto list = reg.call("memory_read", json::object(), ctx);
    CHECK(list.ok && list.output.find("- Projects") != std::string::npos);
    auto sec = reg.call("memory_read", {{"section", "Projects"}}, ctx);
    CHECK(sec.ok && sec.output.find("Widget") != std::string::npos);
    auto miss = reg.call("memory_read", {{"section", "Nope"}}, ctx);
    CHECK(!miss.ok && miss.output.find("Projects") != std::string::npos);
    auto se = reg.call("memory_search", {{"query", "plans"}}, ctx);
    CHECK(se.ok && se.output.find("[How to work]") != std::string::npos);
    CHECK(!reg.call("memory_search", {{"query", ""}}, ctx).ok);

    auto bad = reg.call("memory_append", {{"section", "Projects"}, {"fact", "uses \xE2\x80\x94 dashes"}}, ctx);
    CHECK(!bad.ok && bad.output.find("dash") != std::string::npos);
    CHECK(slurp(file) == SAMPLE);   // nothing written
    auto ok = reg.call("memory_append", {{"section", "Projects"}, {"fact", "Judas: the harness\nsecond line"}}, ctx);
    CHECK(ok.ok);
    std::string now = slurp(file);
    auto pos = now.find("- Widget: the CLI\n- 20");
    REQUIRE(pos != std::string::npos);
    CHECK(now.find("[judas] Judas: the harness second line\n\n## Log") != std::string::npos);
    CHECK(now.find("[judas]") > pos);
    CHECK(!fs::exists(file.string() + ".tmp." + std::to_string(getpid())));
    auto created = reg.call("memory_append", {{"section", "Fresh"}, {"fact", "first"}}, ctx);
    CHECK(created.ok);
    now = slurp(file);
    CHECK(now.find("## Fresh\n\n- ") != std::string::npos);
    CHECK(now.find("## Fresh") < now.find("## Log"));

    // concurrent appends keep every line (flock + atomic rename)
    std::vector<std::thread> ts;
    for (int i = 0; i < 8; i++)
        ts.emplace_back([&, i] {
            ToolContext c;
            c.cfg = &cfg;
            reg.call("memory_append", {{"section", "Projects"}, {"fact", "parallel " + std::to_string(i)}}, c);
        });
    for (auto & t : ts) t.join();
    now = slurp(file);
    for (int i = 0; i < 8; i++) CHECK(now.find("parallel " + std::to_string(i) + "\n") != std::string::npos);

    // a missing file is created by append, not by read
    Config c2;
    c2.memory.path = (root / "sub" / "NEW.md").string();
    ToolContext cx2;
    cx2.cfg = &c2;
    CHECK(!reg.call("memory_read", json::object(), cx2).ok);
    CHECK(reg.call("memory_append", {{"section", "Log"}, {"fact", "hello"}}, cx2).ok);
    CHECK(slurp(c2.memory.path).rfind("## Log\n\n- 20", 0) == 0);

    // prompt block: chosen sections, cap, mtime refresh
    Config pc;
    pc.memory.path = file.string();
    pc.memory.inject_sections = {"Rules", "How to work"};
    std::string blk = memory_prompt_block(pc);
    CHECK(blk.find("always run the tests") != std::string::npos);
    CHECK(blk.find("plans come first") != std::string::npos);
    CHECK(blk.find("Widget: the CLI") == std::string::npos);   // Projects is not injected
    pc.prompt.max_memory_tokens = 10;   // ~40 bytes
    std::string small = memory_prompt_block(pc);
    CHECK(small.find("memory truncated") != std::string::npos);
    CHECK(small.find("plans come first") == std::string::npos);
    pc.prompt.max_memory_tokens = 3000;
    CHECK(Config().prompt.max_memory_tokens == 3000);
    // edit the file: the block follows (mtime differs)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    { std::ofstream(file) << "## Rules\n\n- brand new rule\n"; }
    blk = memory_prompt_block(pc);
    CHECK(blk.find("brand new rule") != std::string::npos && blk.find("always run the tests") == std::string::npos);
    pc.memory.path = (root / "none.md").string();
    CHECK(memory_prompt_block(pc).empty());
    pc.memory.path = file.string();
    pc.memory.inject_sections = {};
    CHECK(memory_prompt_block(pc).empty());

    // forget: only [judas] bullets, exact line, file otherwise untouched
    bool hit = false;
    CHECK(memory_remove_line("a\n- x\nb\n", "  - x ", &hit) == "a\nb\n" && hit);
    CHECK(memory_remove_line("a\n", "- x", &hit) == "a\n" && !hit);
    { std::ofstream(file) << "## Rules\n\n- 2026-10-01 [notes] keep\n- 2026-10-02 [judas] drop me\n\n## Log\n"; }
    Config fc;
    fc.memory.path = file.string();
    CHECK(!memory_forget(fc, "- 2026-10-01 [notes] keep").empty());
    CHECK(memory_forget(fc, "- 2026-10-02 [judas] drop me").empty());
    CHECK(slurp(file) == "## Rules\n\n- 2026-10-01 [notes] keep\n\n## Log\n");
    CHECK(!memory_forget(fc, "- 2026-10-02 [judas] drop me").empty());   // already gone
    CHECK(memory_is_judas_line("- 2026-10-02 [judas] x"));
    CHECK(!memory_is_judas_line("- Tag the writer: [notes] [judas] [other]"));
    CHECK(!memory_forget(fc, "- Tag the writer: [notes] [judas] [other]").empty());

    // a symlinked memory file keeps its link; an unreadable one is never replaced
    {
        fs::path target = root / "real.md", link = root / "link.md";
        { std::ofstream(target) << "## Log\n\n- 2026-10-01 [notes] keep\n"; }
        fs::create_symlink(target, link);
        Config lc;
        lc.memory.path = link.string();
        ToolRegistry lr;
        register_memory_tools(lr);
        ToolContext lx;
        lx.cfg = &lc;
        CHECK(lr.call("memory_append", {{"section", "Log"}, {"fact", "via link"}}, lx).ok);
        CHECK(fs::is_symlink(link) && slurp(target).find("via link") != std::string::npos &&
              slurp(target).find("keep") != std::string::npos);
        if (::geteuid() != 0) {
            fs::permissions(target, fs::perms::none);
            CHECK(!lr.call("memory_append", {{"section", "Log"}, {"fact", "lost?"}}, lx).ok);
            fs::permissions(target, fs::perms::owner_read | fs::perms::owner_write);
            CHECK(slurp(target).find("keep") != std::string::npos);
        }
    }

    fs::remove_all(root);
    TEST_MAIN_END();
}
