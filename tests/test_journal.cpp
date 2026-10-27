// Journal tests: record, commit, revert, diff, gc on a scratch dir under JUDAS_TEST_TMP.
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "snap/journal.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

static std::string slurp(const fs::path & p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
static void put(const fs::path & p, const std::string & s) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    f << s;
}
static unsigned mode(const fs::path & p) {
    struct stat sb;
    stat(p.c_str(), &sb);
    return sb.st_mode & 07777;
}
static size_t count_objects(const fs::path & root) {
    size_t n = 0;
    for (auto & e : fs::directory_iterator(root / "objects")) { (void)e; n++; }
    return n;
}

int main() {
    fs::path base = JUDAS_TEST_TMP "/judas-journal-test-" + std::to_string(getpid());
    fs::remove_all(base);
    fs::create_directories(base / "work");
    Journal j(base / "journal");
    fs::path w = base / "work";

    // edit then revert: bytes and mode come back
    {
        put(w / "a.txt", "one\ntwo\nthree\n");
        chmod((w / "a.txt").c_str(), 0640);
        auto id = j.begin("s1", "main", "edit a");
        j.record(id, w / "a.txt");
        j.record(id, w / "a.txt");  // idempotent
        put(w / "a.txt", "one\nTWO\nthree\nfour\n");
        chmod((w / "a.txt").c_str(), 0600);
        j.commit(id);
        auto info = j.get(id);
        REQUIRE(info && info->committed && info->files.size() == 1);
        CHECK(!info->files[0].after_hash.empty());
        CHECK(info->files[0].before_hash != info->files[0].after_hash);
        std::string d = j.diff(id);
        CHECK(d.find("-two") != std::string::npos);
        CHECK(d.find("+TWO") != std::string::npos);
        CHECK(d.find("+four") != std::string::npos);
        CHECK(d.find("--- " + (w / "a.txt").string()) != std::string::npos);
        auto r = j.revert(id);
        CHECK(r.ok && r.conflicts.empty() && r.restored.size() == 1);
        CHECK(slurp(w / "a.txt") == "one\ntwo\nthree\n");
        CHECK(mode(w / "a.txt") == 0640);
        CHECK(j.get(id)->reverted);
        CHECK(!j.revert(id).ok);  // already reverted
    }
    // create new file then revert deletes it
    {
        auto id = j.begin("s1", "main", "create");
        j.record(id, w / "sub/new.txt");
        put(w / "sub/new.txt", "hello\n");
        j.commit(id);
        CHECK(j.diff(id).find("--- /dev/null") != std::string::npos);
        auto r = j.revert(id);
        CHECK(r.ok && r.removed.size() == 1);
        CHECK(!fs::exists(w / "sub/new.txt"));
    }
    // delete then revert restores
    {
        put(w / "gone.txt", "precious\n");
        auto id = j.begin("s1", "main", "delete");
        j.record(id, w / "gone.txt");
        fs::remove(w / "gone.txt");
        j.commit(id);
        CHECK(j.diff(id).find("+++ /dev/null") != std::string::npos);
        auto r = j.revert(id);
        CHECK(r.ok);
        CHECK(slurp(w / "gone.txt") == "precious\n");
    }
    // directory record and revert (including an empty dir and a nested path)
    {
        put(w / "d/x.txt", "x\n");
        put(w / "d/in/y.txt", "y\n");
        fs::create_directories(w / "d/empty");
        auto id = j.begin("s1", "main", "rm -r d");
        j.record(id, w / "d");
        fs::remove_all(w / "d");
        j.commit(id);
        auto info = j.get(id);
        REQUIRE(info);
        CHECK(info->files.size() >= 5);
        auto r = j.revert(id);
        CHECK(r.ok);
        CHECK(slurp(w / "d/x.txt") == "x\n");
        CHECK(slurp(w / "d/in/y.txt") == "y\n");
        CHECK(fs::is_directory(w / "d/empty"));
    }
    // symlink is recorded as a link, not followed
    {
        fs::create_symlink("a.txt", w / "link");
        auto id = j.begin("s1", "main", "relink");
        j.record(id, w / "link");
        fs::remove(w / "link");
        fs::create_symlink("elsewhere", w / "link");
        j.commit(id);
        CHECK(j.revert(id).ok);
        CHECK(fs::is_symlink(w / "link") && fs::read_symlink(w / "link") == "a.txt");
    }
    // revert_to across 3 checkpoints, in order, with a session filter
    {
        put(w / "r.txt", "v0\n");
        std::string ids[3];
        for (int i = 0; i < 3; i++) {
            ids[i] = j.begin(i == 1 ? "other" : "s2", "main", "step");
            j.record(ids[i], w / "r.txt");
            put(w / "r.txt", "v" + std::to_string(i + 1) + "\n");
            j.commit(ids[i]);
        }
        CHECK(ids[0] < ids[1] && ids[1] < ids[2]);
        auto l = j.list();
        REQUIRE(l.size() >= 3);
        CHECK(l[0].id == ids[2]);  // newest first
        CHECK(j.list("other").size() == 1);
        // revert_to from ids[0] without session filter: v3 -> v2 -> v1 -> v0
        auto r = j.revert_to(ids[0]);
        CHECK(r.ok && r.restored.size() == 3);
        CHECK(slurp(w / "r.txt") == "v0\n");
        CHECK(j.get(ids[0])->reverted && j.get(ids[2])->reverted);
    }
    // conflict detection
    {
        put(w / "c.txt", "base\n");
        auto id = j.begin("s3", "main", "edit c");
        j.record(id, w / "c.txt");
        put(w / "c.txt", "mine\n");
        j.commit(id);
        put(w / "c.txt", "someone else\n");
        auto r = j.revert(id, true);
        CHECK(!r.ok && r.conflicts.size() == 1);
        CHECK(slurp(w / "c.txt") == "someone else\n");
        CHECK(!j.get(id)->reverted);
        auto r2 = j.revert(id, false);
        CHECK(r2.ok && r2.conflicts.size() == 1);
        CHECK(slurp(w / "c.txt") == "base\n");
    }
    // empty checkpoint discarded; irreversible notes kept and reported
    {
        auto id = j.begin("s4", "main", "nothing");
        j.commit(id);
        CHECK(!j.get(id).has_value());
        auto id2 = j.begin("s4", "main", "restart");
        j.note_irreversible(id2, "systemctl --user restart waybar");
        j.commit(id2);
        auto r = j.revert(id2);
        CHECK(r.ok && r.irreversible.size() == 1);
    }
    // reflink_copy, mode preserved, dedupe of identical content
    {
        put(w / "big.bin", std::string(200000, 'q'));
        chmod((w / "big.bin").c_str(), 0750);
        CHECK(reflink_copy(w / "big.bin", w / "big.copy"));
        CHECK(slurp(w / "big.copy") == slurp(w / "big.bin"));
        CHECK(mode(w / "big.copy") == 0750);
        CHECK(sha256_file(w / "big.copy") == sha256_file(w / "big.bin"));
        CHECK(sha256_file(w / "nope").empty());
        CHECK(sha256_file(w / "empty-none") == "");
        put(w / "e", "");
        CHECK(sha256_file(w / "e") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

        Journal j2(base / "journal2");
        put(w / "p1", "same content\n");
        put(w / "p2", "same content\n");
        auto id = j2.begin("s", "m", "dedupe");
        j2.record(id, w / "p1");
        j2.record(id, w / "p2");
        j2.commit(id);
        CHECK(count_objects(base / "journal2") == 1);
    }
    // binary diff summary
    {
        put(w / "bin", std::string("ab\0cd", 5));
        auto id = j.begin("s5", "main", "bin");
        j.record(id, w / "bin");
        put(w / "bin", std::string("ab\0ce", 5));
        j.commit(id);
        CHECK(j.diff(id).find("Binary") != std::string::npos);
    }
    // gc removes orphan objects, keeps referenced ones, and drops old checkpoints
    {
        Journal g(base / "journal3");
        put(w / "g.txt", "g1\n");
        auto id = g.begin("s", "m", "g");
        g.record(id, w / "g.txt");
        put(w / "g.txt", "g2\n");
        g.commit(id);
        put(base / "journal3/objects/deadbeef", "orphan");
        size_t before = count_objects(base / "journal3");
        CHECK(g.gc(30) == 1);
        CHECK(count_objects(base / "journal3") == before - 1);
        CHECK(g.get(id).has_value());
        usleep(20000);
        CHECK(g.gc(0) == 2);  // everything is older than "now": checkpoint and both objects go
        CHECK(!g.get(id).has_value());
        CHECK(count_objects(base / "journal3") == 0);
    }
    // ids stay unique across a restart of the journal object
    {
        Journal k(base / "journal4");
        put(w / "k.txt", "k\n");
        std::string last;
        for (int i = 0; i < 5; i++) {
            Journal kk(base / "journal4");
            auto id = kk.begin("s", "m", "k");
            CHECK(id != last);
            kk.record(id, w / "k.txt");
            put(w / "k.txt", "k" + std::to_string(i) + "\n");
            kk.commit(id);
            last = id;
        }
        CHECK(k.list().size() == 5);
    }

    fs::remove_all(base);
    TEST_MAIN_END();
}
