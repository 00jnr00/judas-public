// Skills: frontmatter parsing, fallbacks, duplicates, trigger matching, index block, enabled=false.
#include <unistd.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "skills/skills.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

static void write(const fs::path & p, const std::string & s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p) << s;
}

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-skills-test-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::path a = root / "a", b = root / "b";
    write(a / "deploy.md",
          "---\nname: deploy\ndescription: Ship it safely\ntriggers: [Deploy, release now]\n"
          "pinned: true\nsource: agent\nversion: 4\n---\n\n# Steps\nRun deploy.sh\n");
    write(a / "plain.md", "\n\nFirst real line\nSecond line\n");
    write(a / "csv.md", "---\ndescription: csv triggers\ntriggers: Alpha, \"Beta\"\n---\nbody csv\n");
    write(a / "notrig.md", "---\ndescription: never auto\n---\nbody nt\n");
    write(a / "dup.md", "---\nname: dup\ndescription: first\n---\nold\n");
    write(b / "zdup.md", "---\nname: dup\ndescription: second\ntriggers: dupword\n---\nnew\n");
    write(a / "logwatch.md", "---\nname: logwatch\ndescription: log boundaries\ntriggers: [log, go]\n---\nbody log\n");
    write(a / ".archive" / "old.md", "---\nname: old\ndescription: archived\ntriggers: deploy\n---\nold body\n");
    write(a / ".quarantine" / "bad.md", "---\nname: bad\ndescription: quarantined\ntriggers: deploy\n---\nbad body\n");
    write(a / "ignored.txt", "not markdown");

    SkillsConfig cfg;
    cfg.paths = {a.string(), b.string(), (root / "missing").string()};
    Skills sk(cfg);
    CHECK(sk.all().size() == 6);

    auto d = sk.by_name("deploy");
    REQUIRE(d.has_value());
    CHECK(d->description == "Ship it safely");
    CHECK(d->triggers.size() == 2);
    CHECK(d->triggers[0] == "deploy");
    CHECK(d->triggers[1] == "release now");
    CHECK(d->body == "# Steps\nRun deploy.sh");
    CHECK(d->pinned);
    CHECK(d->source == "agent");
    CHECK(d->version == 4);

    auto p = sk.by_name("plain");
    REQUIRE(p.has_value());
    CHECK(p->description == "First real line");
    CHECK(p->triggers.empty());
    CHECK(p->body == "First real line\nSecond line");
    CHECK(!p->pinned);
    CHECK(p->source == "local");
    CHECK(p->version == 1);

    auto c = sk.by_name("csv");
    REQUIRE(c.has_value());
    CHECK(c->triggers.size() == 2 && c->triggers[0] == "alpha" && c->triggers[1] == "beta");

    auto dup = sk.by_name("dup");
    REQUIRE(dup.has_value());
    CHECK(dup->description == "second");
    CHECK(dup->body == "new");
    int dups = 0;
    for (const auto & s : sk.all()) dups += s.name == "dup";
    CHECK(dups == 1);
    CHECK(!sk.by_name("nope").has_value());

    // Matching: word boundaries, case-insensitive, no-trigger skills never match.
    auto m = sk.matching("Please DEPLOY the app");
    CHECK(m.size() == 1 && m[0].name == "deploy");
    CHECK(sk.matching("a BETA test").size() == 1);
    CHECK(sk.matching("never auto body nt").empty());
    CHECK(sk.matching("nothing relevant").empty());
    // "log" must not match "blog"; short triggers are ignored; punctuation is a boundary.
    CHECK(sk.matching("read the blog post").empty());
    CHECK(sk.matching("go now").empty());
    CHECK(sk.matching("check the log.").size() == 1);
    CHECK(sk.matching("deploy").size() == 1);
    CHECK(sk.matching("undeployed").empty());
    CHECK(sk.matching("release now please").size() == 1);

    // Archived and quarantined files stay out of the index.
    CHECK(!sk.by_name("old").has_value());
    CHECK(!sk.by_name("bad").has_value());
    CHECK(sk.matching("deploy the app").size() == 1);

    // reload picks up files added after construction.
    write(a / "late.md", "---\nname: late\ndescription: added later\n---\nlate body\n");
    CHECK(!sk.by_name("late").has_value());
    sk.reload();
    CHECK(sk.by_name("late").has_value());
    CHECK(sk.all().size() == 7);

    // reload_if_changed: write a skill file into a temp dir after construction, call it,
    // see the new skill; a second call within ~2 s does no work (debounced).
    {
        SkillsConfig rc;
        rc.paths = {(root / "rcdir").string()};
        fs::create_directories(root / "rcdir");
        Skills s(rc);
        CHECK(s.all().empty());
        write(root / "rcdir" / "addme.md", "---\nname: addme\ndescription: appears after reload\n---\nbody\n");
        CHECK(s.reload_if_changed());       // detects new file, reloads, returns true
        CHECK(s.by_name("addme").has_value());
        CHECK(!s.reload_if_changed());      // debounced: no work, returns false
    }

    // Index block: names and descriptions, no bodies.
    std::string idx = sk.index_block();
    CHECK(idx.find("- deploy: Ship it safely") != std::string::npos);
    CHECK(idx.find("- notrig: never auto") != std::string::npos);
    CHECK(idx.find("Run deploy.sh") == std::string::npos);

    // Empty set.
    SkillsConfig empty;
    empty.paths = {(root / "missing").string()};
    Skills none(empty);
    CHECK(none.all().empty());
    CHECK(none.index_block().empty());

    // Disabled loads nothing.
    SkillsConfig off = cfg;
    off.enabled = false;
    Skills disabled(off);
    CHECK(disabled.all().empty());
    CHECK(disabled.index_block().empty());

    // write_dir: first path, created when missing; ~ expands.
    SkillsConfig wd;
    wd.paths = {(root / "wd-one").string(), (root / "wd-two").string()};
    Skills w(wd);
    CHECK(w.write_dir() == (root / "wd-one").string());
    CHECK(fs::is_directory(root / "wd-one"));
    CHECK(!fs::exists(root / "wd-two"));
    SkillsConfig home_cfg;
    home_cfg.paths = {"~/skills-home"};
    setenv("HOME", (root / "home").string().c_str(), 1);
    Skills hw(home_cfg);
    CHECK(hw.write_dir() == (root / "home" / "skills-home").string());
    CHECK(fs::is_directory(root / "home" / "skills-home"));
    SkillsConfig no_paths;
    no_paths.paths.clear();
    Skills np(no_paths);
    CHECK(np.write_dir().empty());

    fs::remove_all(root);
    TEST_MAIN_END();
}
