// skill_manage: every action, the write policy, pinned refusals, validation and journaling.
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "skills/skill_tools.h"
#include "skills/skills.h"
#include "skills/usage.h"
#include "snap/journal.h"
#include "test.h"
#include "tools/tools.h"

namespace fs = std::filesystem;
using namespace judas;

static std::string slurp(const fs::path & p) {
    std::ifstream f(p, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(f)), {});
}

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-skills-tools-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::path skills_dir = root / "skills";
    fs::create_directories(skills_dir);

    Journal journal(root / "journal");
    SkillsConfig sc;
    sc.paths = {skills_dir.string()};
    Skills skills(sc);
    SkillUsage usage((root / "data" / "skills.db").string());

    Config cfg;
    cfg.skills = sc;
    cfg.permissions.allow_paths = {root.string()};

    ToolRegistry reg;
    register_skill_tools(reg, skills, &usage);

    ToolContext ctx;
    ctx.cwd = root.string();
    ctx.cfg = &cfg;
    ctx.journal = &journal;
    ctx.session = "s1";
    ctx.agent = "a1";
    ctx.checkpoint = journal.begin(ctx.session, ctx.agent, "skill_manage test");

    auto call = [&](json args) { return reg.call("skill_manage", args, ctx); };

    auto list = call({{"action", "list"}});
    CHECK(list.ok);
    CHECK(list.output == "no skills");

    // create
    auto r = call({{"action", "create"},
                   {"name", "deploy"},
                   {"description", "Ship it safely"},
                   {"triggers", {"Deploy", "release now"}},
                   {"body", "# Steps\nRun deploy.sh"}});
    CHECK(r.ok);
    auto created = skills.by_name("deploy");
    REQUIRE(created.has_value());
    CHECK(created->source == "agent");
    CHECK(created->version == 1);
    CHECK(!created->pinned);
    CHECK(created->triggers.size() == 2);
    CHECK(created->body == "# Steps\nRun deploy.sh");
    CHECK(fs::exists(skills_dir / "deploy.md"));
    std::string deploy_file = slurp(skills_dir / "deploy.md");
    CHECK(deploy_file.find("source: agent") != std::string::npos);
    CHECK(deploy_file.find("version: 1") != std::string::npos);

    // duplicate create
    auto dup = call({{"action", "create"}, {"name", "deploy"}, {"description", "again"}, {"body", "x"}});
    CHECK(!dup.ok);
    CHECK(dup.output.find("already exists") != std::string::npos);

    // bad names
    for (const char * bad : {"Bad", "-bad", "a_b", "a.b", "", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"}) {
        auto badr = call({{"action", "create"}, {"name", bad}, {"description", "d"}, {"body", "b"}});
        CHECK(!badr.ok);
        CHECK(badr.output.find("invalid skill name") != std::string::npos);
    }

    // description and body limits
    auto long_desc = call({{"action", "create"},
                           {"name", "long-desc"},
                           {"description", std::string(201, 'd')},
                           {"body", "b"}});
    CHECK(!long_desc.ok);
    auto two_line = call({{"action", "create"},
                          {"name", "two-line"},
                          {"description", "one\ntwo"},
                          {"body", "b"}});
    CHECK(!two_line.ok);
    auto big = call({{"action", "create"},
                     {"name", "big"},
                     {"description", "d"},
                     {"body", std::string(16 * 1024 + 1, 'x')}});
    CHECK(!big.ok);
    CHECK(!skills.by_name("long-desc").has_value());
    CHECK(!skills.by_name("two-line").has_value());
    CHECK(!skills.by_name("big").has_value());

    // view
    auto view = call({{"action", "view"}, {"name", "deploy"}});
    CHECK(view.ok);
    CHECK(view.output.find("# skill: deploy") != std::string::npos);
    CHECK(view.output.find("Run deploy.sh") != std::string::npos);
    CHECK(!call({{"action", "view"}, {"name", "nope"}}).ok);
    CHECK(!call({{"action", "view"}}).ok);

    // patch: exact, unique, version bump
    auto patch = call({{"action", "patch"}, {"name", "deploy"}, {"old_string", "# Steps"}, {"new_string", "# New Steps"}});
    CHECK(patch.ok);
    created = skills.by_name("deploy");
    REQUIRE(created.has_value());
    CHECK(created->version == 2);
    CHECK(created->body.find("# New Steps") != std::string::npos);
    CHECK(!call({{"action", "patch"}, {"name", "deploy"}, {"old_string", "missing"}, {"new_string", "x"}}).ok);
    CHECK(!call({{"action", "patch"}, {"name", "deploy"}, {"old_string", ""}, {"new_string", "x"}}).ok);
    CHECK(!call({{"action", "patch"}, {"name", "deploy"}, {"old_string", "same"}, {"new_string", "same"}}).ok);
    CHECK(!call({{"action", "patch"}, {"name", "nope"}, {"old_string", "a"}, {"new_string", "b"}}).ok);

    // non-unique patch
    CHECK(call({{"action", "create"}, {"name", "dup"}, {"description", "duplicate text"}, {"body", "same same"}}).ok);
    auto non_unique = call({{"action", "patch"}, {"name", "dup"}, {"old_string", "same"}, {"new_string", "diff"}});
    CHECK(!non_unique.ok);
    CHECK(non_unique.output.find("occurs 2 times") != std::string::npos);

    // rewrite replaces the body and bumps the version
    auto rewrite = call({{"action", "rewrite"}, {"name", "dup"}, {"body", "rewritten body"}});
    CHECK(rewrite.ok);
    auto d = skills.by_name("dup");
    REQUIRE(d.has_value());
    CHECK(d->body == "rewritten body");
    CHECK(d->version == 2);
    CHECK(d->description == "duplicate text");
    CHECK(!call({{"action", "rewrite"}, {"name", "dup"}, {"body", std::string(16 * 1024 + 1, 'x')}}).ok);
    CHECK(!call({{"action", "rewrite"}, {"name", "nope"}, {"body", "x"}}).ok);

    // pin: content writes are refused, then unpin allows them
    auto pin = call({{"action", "pin"}, {"name", "deploy"}});
    CHECK(pin.ok);
    created = skills.by_name("deploy");
    REQUIRE(created.has_value());
    CHECK(created->pinned);
    CHECK(created->version == 3);
    CHECK(!call({{"action", "patch"}, {"name", "deploy"}, {"old_string", "# New Steps"}, {"new_string", "x"}}).ok);
    CHECK(!call({{"action", "rewrite"}, {"name", "deploy"}, {"body", "x"}}).ok);
    CHECK(!call({{"action", "archive"}, {"name", "deploy"}}).ok);
    CHECK(call({{"action", "pin"}, {"name", "deploy"}}).ok);  // already pinned, no write
    auto unpin = call({{"action", "unpin"}, {"name", "deploy"}});
    CHECK(unpin.ok);
    created = skills.by_name("deploy");
    REQUIRE(created.has_value());
    CHECK(!created->pinned);
    CHECK(created->version == 4);
    CHECK(call({{"action", "unpin"}, {"name", "deploy"}}).ok);  // already unpinned, no write
    CHECK(!call({{"action", "pin"}, {"name", "nope"}}).ok);

    // archive: the file moves to .archive and is no longer loaded
    auto archive = call({{"action", "archive"}, {"name", "deploy"}});
    CHECK(archive.ok);
    CHECK(!skills.by_name("deploy").has_value());
    CHECK(fs::exists(skills_dir / ".archive" / "deploy.md"));
    CHECK(!fs::exists(skills_dir / "deploy.md"));
    CHECK(slurp(skills_dir / ".archive" / "deploy.md").find("version: 5") != std::string::npos);
    CHECK(!call({{"action", "archive"}, {"name", "deploy"}}).ok);
    CHECK(!call({{"action", "archive"}, {"name", "nope"}}).ok);
    auto after = call({{"action", "list"}});
    CHECK(after.ok);
    CHECK(after.output.find("deploy") == std::string::npos);
    CHECK(after.output.find("dup") != std::string::npos);

    // usage counters
    auto u = usage.get("deploy");
    REQUIRE(u.has_value());
    CHECK(u->created_ms > 0);
    CHECK(u->patches == 1);
    auto ud = usage.get("dup");
    REQUIRE(ud.has_value());
    CHECK(ud->patches == 1);

    // unknown action is rejected by the schema
    auto unknown = call({{"action", "destroy"}});
    CHECK(!unknown.ok);

    // rewrite a hand-written skill whose frontmatter has no newline after the closing dashes
    {
        std::ofstream f(skills_dir / "raw.md", std::ios::binary);
        f << "---\nname: raw\ndescription: hand written\n---";
    }
    skills.reload();
    REQUIRE(skills.by_name("raw").has_value());
    auto raw_rewrite = call({{"action", "rewrite"}, {"name", "raw"}, {"body", "new body"}});
    CHECK(raw_rewrite.ok);
    auto raw = skills.by_name("raw");
    REQUIRE(raw.has_value());
    CHECK(raw->body == "new body");
    CHECK(raw->description == "hand written");

    // the journal recorded every written path
    journal.commit(ctx.checkpoint);
    auto info = journal.get(ctx.checkpoint);
    REQUIRE(info.has_value());
    CHECK(info->files.size() >= 3);

    // write policy: a skills dir outside permissions.allow_paths is refused
    Config strict = cfg;
    strict.permissions.allow_paths = {(root / "elsewhere").string()};
    ToolContext ctx2 = ctx;
    ctx2.cfg = &strict;
    ctx2.checkpoint = journal.begin(ctx.session, ctx.agent, "refused write");
    auto refused = reg.call("skill_manage",
                            {{"action", "create"}, {"name", "blocked"}, {"description", "d"}, {"body", "b"}}, ctx2);
    CHECK(!refused.ok);
    CHECK(refused.output.find("refused") != std::string::npos);
    CHECK(!fs::exists(skills_dir / "blocked.md"));

    fs::remove_all(root);
    TEST_MAIN_END();
}
