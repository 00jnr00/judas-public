#include <cmath>
#include <set>
// Cowork work modes (design, research, write): store migration, prompts, tools, mode gating, suggestions
// (accept exactly once, stale, snapshot), the session file helpers (path escape, size, binary, hash conflict).
#include <sqlite3.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "cowork/cowork.h"
#include "snap/journal.h"
#include "store/store.h"
#include "test.h"
#include "tools/tools.h"

namespace fs = std::filesystem;
using namespace judas;

static fs::path g_base;

static bool has(const std::string & h, const std::string & n) { return h.find(n) != std::string::npos; }
static void put(const fs::path & p, const std::string & t) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << t;
}
static std::string slurp(const fs::path & p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static void test_modes_and_prompts() {
    for (const char * m : {"chat", "cowork", "code", "learn", "swarm", "engineering", "design", "research", "write"})
        CHECK(cowork::valid_mode(m));
    CHECK(!cowork::valid_mode("zzz") && !cowork::valid_mode(""));
    std::string d = cowork::design_prompt(), r = cowork::research_prompt(), w = cowork::write_prompt();
    CHECK(has(d, "artifact_render") && has(d, "artifact_update") && has(d, "concept"));
    CHECK(has(r, "source_add") && has(r, "[n]") && has(r, "artifact_update") && has(r, "unknown"));
    CHECK(has(w, "doc_suggest") && has(w, "spelling") && has(w, "note") && has(w, "Never rewrite"));
    for (auto & p : {d, r, w}) CHECK(!has(p, "\xe2\x80\x94"));
}

static void test_gating() {
    // cad: engineering and design, any agent
    for (const char * m : {"engineering", "design"}) CHECK(cowork::tool_allowed("cad_state", m, true) && cowork::tool_allowed("cad_feature", m, false));
    for (const char * m : {"chat", "cowork", "code", "research", "write"}) CHECK(!cowork::tool_allowed("cad_state", m, true));
    // calc_record: engineering, lead only
    CHECK(cowork::tool_allowed("calc_record", "engineering", true) && !cowork::tool_allowed("calc_record", "engineering", false));
    for (const char * m : {"design", "research", "write", "chat"}) CHECK(!cowork::tool_allowed("calc_record", m, true));
    // source_add: research, lead only
    CHECK(cowork::tool_allowed("source_add", "research", true) && !cowork::tool_allowed("source_add", "research", false));
    for (const char * m : {"design", "engineering", "write", "cowork"}) CHECK(!cowork::tool_allowed("source_add", m, true));
    // doc_suggest: write, lead only
    CHECK(cowork::tool_allowed("doc_suggest", "write", true) && !cowork::tool_allowed("doc_suggest", "write", false));
    for (const char * m : {"design", "engineering", "research", "chat"}) CHECK(!cowork::tool_allowed("doc_suggest", m, true));
    CHECK(cowork::tool_allowed("read_file", "write", false) && cowork::tool_allowed("artifact_create", "design", true));
}

static void test_migration() {
    fs::path path = g_base / "old.db";
    {
        sqlite3 * db = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        const char * sql =
            "CREATE TABLE sessions (id TEXT PRIMARY KEY, title TEXT NOT NULL, cwd TEXT NOT NULL, model TEXT NOT NULL,"
            " created_ms INTEGER NOT NULL, updated_ms INTEGER NOT NULL, mode TEXT NOT NULL DEFAULT 'chat');"
            "INSERT INTO sessions VALUES ('s-old', 'old', '/tmp/old', 'm', 1, 2, 'engineering');";
        REQUIRE(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    Store st(path.string());
    CHECK(st.get_session("s-old").has_value());
    CHECK(st.list_calcs("s-old").empty() && st.list_sources("s-old").empty() && st.list_suggestions("s-old").empty());
    CalcRow c;
    c.name = "n"; c.expr = "1+1"; c.value = "2";
    CHECK(st.add_calc("s-old", c) == 1);
    {
        Store again(path.string());   // reopening is a no-op and keeps the rows
        CHECK(again.list_calcs("s-old").size() == 1);
        SuggestionRow g;
        g.path = "a.txt"; g.kind = "note"; g.find = "x";
        CHECK(again.add_suggestion("s-old", g) == "g1");
        CHECK(again.add_suggestion("s-old", g) == "g2");
        CHECK(again.get_suggestion("s-old", "g2").has_value() && !again.get_suggestion("s-old", "g3") && !again.get_suggestion("s-old", "x1"));
        again.delete_session("s-old");
        CHECK(again.list_calcs("s-old").empty() && again.list_suggestions("s-old").empty());
    }
}

static void test_tools() {
    fs::path root = g_base / "tools";
    fs::create_directories(root);
    Store st((g_base / "tools.db").string());
    auto sess = st.create_session("t", root.string(), "m", "engineering");
    ToolRegistry reg;
    cowork::register_cowork_tools(reg, st);
    for (const char * n : {"calc_record", "source_add", "doc_suggest"}) CHECK(reg.find(n) != nullptr);
    Config cfg;
    ToolContext ctx;
    ctx.cwd = root.string();
    ctx.cfg = &cfg;
    ctx.session = sess.id;

    // calc_record: engineering only, numeric values accepted, row numbers count up
    ctx.mode = "engineering";
    auto r1 = reg.call("calc_record", {{"name", "preload"}, {"expr", "0.2*d*F"}, {"value", 12.5}, {"unit", "kN"}}, ctx);
    CHECK(r1.ok && has(r1.output, "row 1"));
    auto r2 = reg.call("calc_record", {{"name", "stress"}, {"expr", "F/A"}, {"value", "88"}, {"unit", "MPa"}, {"note", "assumed A"}}, ctx);
    CHECK(r2.ok && has(r2.output, "row 2"));
    CHECK(!reg.call("calc_record", {{"name", "x"}}, ctx).ok);
    // the shape seen live: no name, the expression's left side names it
    auto nn = reg.call("calc_record", {{"expr", "sigma_allow = 276/2"}, {"value", "138"}, {"unit", "MPa"}}, ctx);
    CHECK(nn.ok && has(nn.output, "sigma_allow = 138") && has(nn.output, "arithmetic checked"));
    auto calcs = st.list_calcs(sess.id);
    REQUIRE(calcs.size() == 3);
    CHECK(calcs[0].value == "12.5" && calcs[1].note == "assumed A" && calcs[1].n == 2 && calcs[0].created_ms > 0);
    CHECK(cowork::calc_json(calcs[1])["unit"] == "MPa");
    CHECK(has(r1.output, "not checked"));
    // the arithmetic after the last '=' is checked (seen live: 17939*2.7/1000 recorded as 4.84 g)
    auto bad = reg.call("calc_record", {{"name", "mass"}, {"expr", "m = V*rho = 17939*2.70/1000"}, {"value", 4.84}, {"unit", "g"}}, ctx);
    CHECK(!bad.ok && has(bad.output, "48.4"));
    auto good = reg.call("calc_record", {{"name", "mass"}, {"expr", "m = V*rho = 17939*2.70/1000"}, {"value", 48.44}, {"unit", "g"}}, ctx);
    CHECK(good.ok && has(good.output, "arithmetic checked"));
    auto txt = reg.call("calc_record", {{"name", "sigma"}, {"expr", "6*40000/(30*8^2)"}, {"value", "about 125"}, {"unit", "MPa"}}, ctx);
    CHECK(txt.ok && st.list_calcs(sess.id).back().value == "125");
    CHECK(cowork::eval_arith("2*pi*3.25^2") && std::fabs(*cowork::eval_arith("2*pi*3.25^2") - 66.366) < 0.01);
    CHECK(cowork::eval_arith("sqrt(16)+2**3") == 12.0 && cowork::eval_arith("-(1.5e3)/3") == -500.0);
    CHECK(cowork::eval_arith("M = F*L = 500*80 = 40000 N*mm") == 40000.0);   // the shape the model writes
    CHECK(cowork::eval_arith("t = sqrt(6*M/(b*s)) = sqrt(6*40000/(30*138)) = 7.61 mm") && std::fabs(*cowork::eval_arith("t = sqrt(6*40000/(30*138)) = 7.61 mm") - 7.614) < 0.001);
    CHECK(!cowork::eval_arith("x = 42"));
    CHECK(!cowork::eval_arith("F/A") && !cowork::eval_arith("17939 mm^3 * 2.7") && !cowork::eval_arith("1/0") && !cowork::eval_arith("(2"));
    ctx.mode = "design";
    CHECK(!reg.call("calc_record", {{"name", "a"}, {"expr", "b"}, {"value", "c"}, {"unit", ""}}, ctx).ok);
    CHECK(st.list_calcs(sess.id).size() == 5);

    // source_add: research only, dedupe by url with a stable number, auto rows get enriched
    ctx.mode = "engineering";
    CHECK(!reg.call("source_add", {{"url", "https://a.example/x"}, {"title", "A"}}, ctx).ok);
    ctx.mode = "research";
    auto s1 = reg.call("source_add", {{"url", "https://a.example/x"}, {"title", "A"}, {"quote", "q"}}, ctx);
    CHECK(s1.ok && has(s1.output, "[1]"));
    auto s2 = reg.call("source_add", {{"url", "https://b.example/y"}, {"title", "B"}}, ctx);
    CHECK(s2.ok && has(s2.output, "[2]"));
    auto s3 = reg.call("source_add", {{"url", "https://a.example/x"}, {"title", "A again"}, {"note", "n"}}, ctx);
    CHECK(s3.ok && has(s3.output, "[1]"));
    CHECK(!reg.call("source_add", {{"url", "ftp://a"}, {"title", "A"}}, ctx).ok);
    SourceRow au;
    au.url = "https://c.example/z"; au.autoadded = true;
    bool added = false;
    CHECK(st.add_source(sess.id, au, &added) == 3 && added);
    CHECK(st.add_source(sess.id, au, &added) == 3 && !added);   // a repeated auto record changes nothing
    auto s4 = reg.call("source_add", {{"url", "https://c.example/z"}, {"title", "C"}}, ctx);
    CHECK(s4.ok && has(s4.output, "[3]"));
    auto srcs = st.list_sources(sess.id);
    REQUIRE(srcs.size() == 3);
    CHECK(srcs[0].title == "A" && srcs[0].quote == "q" && srcs[0].note == "n" && !srcs[0].autoadded);
    CHECK(srcs[2].title == "C" && !srcs[2].autoadded && srcs[2].n == 3);
    CHECK(cowork::source_json(srcs[1])["auto"] == false);

    // doc_suggest: write only, path must exist inside the folder, find exactly once
    put(root / "doc.txt", "The quick brown fox jumpd over the lazy dog. The end. Teh end.\n");
    ctx.mode = "research";
    CHECK(!reg.call("doc_suggest", {{"path", "doc.txt"}, {"suggestions", json::array()}}, ctx).ok);
    ctx.mode = "write";
    json sug = json::array({
        {{"find", "jumpd"}, {"replace", "jumped"}, {"kind", "spelling"}, {"why", "typo"}},
        {{"find", "The end."}, {"replace", ""}, {"kind", "note"}, {"why", "weak ending"}},
        {{"find", "lazy"}, {"replace", "idle"}, {"kind", "spelling"}, {"why", "x"}},
        {{"find", "end"}, {"replace", "END"}, {"kind", "spelling"}, {"why", "ambiguous"}},
        {{"find", "missing text"}, {"replace", "y"}, {"kind", "spelling"}, {"why", "gone"}},
        {{"find", "fox"}, {"replace", "fox"}, {"kind", "spelling"}, {"why", "no change"}},
        {{"find", "dog"}, {"replace", ""}, {"kind", "spelling"}, {"why", "empty fix"}},
    });
    auto d = reg.call("doc_suggest", {{"path", "doc.txt"}, {"suggestions", sug}}, ctx);
    CHECK(d.ok && has(d.output, "3 stored, 4 rejected") && has(d.output, "occurs 2 times") && has(d.output, "does not occur"));
    CHECK(!reg.call("doc_suggest", {{"path", "doc.txt"}, {"suggestions", json::array({{{"find", "fox"}, {"kind", "weird"}, {"why", "k"}}})}}, ctx).ok);
    auto again = reg.call("doc_suggest", {{"path", "doc.txt"}, {"suggestions", json::array({sug[0]})}}, ctx);   // a repeat
    CHECK(!again.ok && has(again.output, "already suggested as g1"));
    auto gs = st.list_suggestions(sess.id);
    REQUIRE(gs.size() == 3);
    CHECK(gs[0].id == "g1" && gs[0].status == "open" && gs[1].kind == "note" && gs[1].replace.empty());
    CHECK(!reg.call("doc_suggest", {{"path", "nope.txt"}, {"suggestions", sug}}, ctx).ok);
    CHECK(!reg.call("doc_suggest", {{"path", "../doc.txt"}, {"suggestions", sug}}, ctx).ok);
    CHECK(!reg.call("doc_suggest", {{"path", (root / "doc.txt").string()}, {"suggestions", sug}}, ctx).ok);
    CHECK(slurp(root / "doc.txt").find("jumpd") != std::string::npos);   // the file is untouched
    CHECK(st.list_suggestions(sess.id).size() == 3);
}

static void test_mode_change() {
    CHECK(cowork::mode_change_allowed("chat", "cowork") && cowork::mode_change_allowed("cowork", "swarm"));
    CHECK(cowork::mode_change_allowed("write", "write"));
    for (const char * w : {"engineering", "design", "research", "write"}) {
        CHECK(!cowork::mode_change_allowed(w, "chat") && !cowork::mode_change_allowed("cowork", w));
    }
    CHECK(!cowork::mode_change_allowed("design", "engineering"));
}

static void test_list_documents() {
    fs::path root = g_base / "docs";
    fs::create_directories(root / "notes" / "deep");
    fs::create_directories(root / ".git");
    fs::create_directories(root / "node_modules");
    put(root / "a.md", "a"); put(root / "notes" / "b.txt", "b"); put(root / "notes" / "deep" / "c.tex", "c");
    put(root / "img.png", "x"); put(root / ".git" / "d.md", "d"); put(root / "node_modules" / "e.md", "e"); put(root / ".hidden.md", "h");
    auto l = cowork::list_documents(root.string());
    std::set<std::string> got;
    for (auto & [p, ms] : l) got.insert(p);
    CHECK(got == std::set<std::string>({"a.md", "notes/b.txt", "notes/deep/c.tex"}));
    for (auto & [p, ms] : l) CHECK(ms > 1700000000000LL);   // unix ms, not the filesystem clock
    CHECK(cowork::list_documents("").empty() && cowork::list_documents((root / "nope").string()).empty());
}

static void test_write_mode_refusal() {
    fs::path root = g_base / "wm";
    fs::create_directories(root);
    put(root / "mine.txt", "my words\n");
    ToolRegistry reg;
    register_builtin_tools(reg);
    Config cfg;
    cfg.permissions.mode = "full-auto";
    cfg.permissions.allow_paths = {root.string()};
    ToolContext ctx;
    ctx.cwd = root.string();
    ctx.cfg = &cfg;
    ctx.mode = "write";
    auto e = reg.call("edit_file", {{"path", "mine.txt"}, {"old_string", "my"}, {"new_string", "your"}}, ctx);
    CHECK(!e.ok && has(e.output, "Write mode: suggest with doc_suggest; the text is the user's"));
    auto w = reg.call("write_file", {{"path", "mine.txt"}, {"content", "x"}}, ctx);
    CHECK(!w.ok && has(w.output, "Write mode: suggest with doc_suggest"));
    auto ap = reg.call("write_file", {{"path", "mine.txt"}, {"content", "x"}, {"append", true}}, ctx);
    CHECK(!ap.ok);
    CHECK(slurp(root / "mine.txt") == "my words\n");
    auto nw = reg.call("write_file", {{"path", "outline.md"}, {"content", "# outline\n"}}, ctx);   // a new file is allowed
    CHECK(nw.ok && fs::exists(root / "outline.md"));
    CHECK(!reg.call("write_file", {{"path", "outline.md"}, {"content", "again"}}, ctx).ok);   // but not over it
    // the shell sees the user's files read-only in write mode (needs bwrap; skipped where it is missing)
    // (the sandbox has a private /tmp, so this part runs in a folder under the build directory)
    fs::path sb = fs::absolute(fs::current_path() / ("cowork-sb-" + std::to_string(getpid())));
    if (fs::exists("/usr/bin/bwrap") && sb.string().rfind("/tmp", 0) != 0) {
        fs::create_directories(sb);
        put(sb / "mine.txt", "my words\n");
        ctx.cwd = sb.string();
        cfg.permissions.allow_paths = {sb.string()};
        cfg.permissions.sandbox = "bwrap";
        auto rd = reg.call("shell", {{"command", "cat mine.txt"}}, ctx);
        CHECK(rd.ok && has(rd.output, "my words"));
        reg.call("shell", {{"command", "echo gone > mine.txt; echo y > new.txt"}}, ctx);
        CHECK(slurp(sb / "mine.txt") == "my words\n" && !fs::exists(sb / "new.txt"));
        ctx.mode = "design";   // outside write mode the same shell can write
        reg.call("shell", {{"command", "echo y > new.txt"}}, ctx);
        CHECK(fs::exists(sb / "new.txt"));
        ctx.mode = "write";
        cfg.permissions.sandbox = "none";
        ctx.cwd = root.string();
        cfg.permissions.allow_paths = {root.string()};
        fs::remove_all(sb);
    }
    // other modes are unaffected
    ctx.mode = "design";
    CHECK(reg.call("edit_file", {{"path", "mine.txt"}, {"old_string", "my"}, {"new_string", "your"}}, ctx).ok);
}

static void test_accept() {
    fs::path root = g_base / "acc";
    fs::create_directories(root);
    Store st((g_base / "acc.db").string());
    Journal journal(g_base / "acc-snap");
    auto sess = st.create_session("t", root.string(), "m", "write");
    std::string orig = "A teh cat. Another cat. And nothing else.\n";
    put(root / "a.txt", orig);
    auto add = [&](const std::string & kind, const std::string & find, const std::string & rep) {
        SuggestionRow g;
        g.path = "a.txt"; g.kind = kind; g.find = find; g.replace = rep; g.why = "w";
        return st.add_suggestion(sess.id, g);
    };
    std::string g1 = add("spelling", "teh", "the");
    auto o = cowork::accept_suggestion(st, &journal, sess.id, root.string(), g1);
    CHECK(o.status == 200 && o.body["ok"] == true && o.body["suggestion"]["status"] == "accepted");
    CHECK(slurp(root / "a.txt") == "A the cat. Another cat. And nothing else.\n");
    // snapshot: the journal can put the original back
    std::string cp = o.body.value("checkpoint", "");
    REQUIRE(!cp.empty());
    CHECK(journal.get(cp).has_value() && journal.get(cp)->files.size() == 1);
    auto rv = journal.revert(cp);
    CHECK(rv.ok && slurp(root / "a.txt") == orig);
    // a second accept of the same suggestion is a conflict and changes nothing
    CHECK(cowork::accept_suggestion(st, &journal, sess.id, root.string(), g1).status == 409);
    CHECK(slurp(root / "a.txt") == orig);

    // find occurs twice at accept time -> stale, 409, file untouched
    std::string g2 = add("spelling", "cat", "dog");
    auto o2 = cowork::accept_suggestion(st, &journal, sess.id, root.string(), g2);
    CHECK(o2.status == 409 && o2.body["suggestion"]["status"] == "stale");
    CHECK(st.get_suggestion(sess.id, g2)->status == "stale" && slurp(root / "a.txt") == orig);

    // the user edited the anchor away since -> stale
    std::string g3 = add("spelling", "nothing", "something");
    put(root / "a.txt", "A teh cat. Different now.\n");
    CHECK(cowork::accept_suggestion(st, &journal, sess.id, root.string(), g3).status == 409);
    CHECK(st.get_suggestion(sess.id, g3)->status == "stale");
    // the file is gone -> stale
    std::string g4 = add("spelling", "teh", "the");
    fs::remove(root / "a.txt");
    CHECK(cowork::accept_suggestion(st, &journal, sess.id, root.string(), g4).status == 409);

    // note: accept just marks it done; reject works once; unknown ids are 404
    std::string g5 = add("note", "Different", "");
    auto o5 = cowork::accept_suggestion(st, &journal, sess.id, root.string(), g5);
    CHECK(o5.status == 200 && o5.body["suggestion"]["status"] == "accepted" && !o5.body.contains("checkpoint"));
    std::string g6 = add("spelling", "x", "y");
    auto rj = cowork::reject_suggestion(st, sess.id, g6);
    CHECK(rj.status == 200 && rj.body["suggestion"]["status"] == "rejected");
    CHECK(cowork::reject_suggestion(st, sess.id, g6).status == 409);
    CHECK(cowork::accept_suggestion(st, &journal, sess.id, root.string(), g6).status == 409);
    CHECK(cowork::accept_suggestion(st, &journal, sess.id, root.string(), "g99").status == 404);
    CHECK(cowork::reject_suggestion(st, sess.id, "nope").status == 404);
    CHECK(cowork::accept_suggestion(st, &journal, "s-other", root.string(), g1).status == 404);   // per session

    // an escape in the stored path never writes outside the folder
    put(g_base / "outside.txt", "secret teh\n");
    SuggestionRow bad;
    bad.path = "../outside.txt"; bad.kind = "spelling"; bad.find = "teh"; bad.replace = "the";
    std::string gb = st.add_suggestion(sess.id, bad);
    CHECK(cowork::accept_suggestion(st, &journal, sess.id, root.string(), gb).status == 409);
    CHECK(slurp(g_base / "outside.txt") == "secret teh\n");
}

static void test_files() {
    fs::path root = g_base / "files";
    fs::create_directories(root / "sub");
    put(root / "sub/doc.md", "# Title\nbody\n");
    std::string cwd = root.string();

    auto g = cowork::read_file(cwd, "sub/doc.md");
    CHECK(g.status == 200 && g.text == "# Title\nbody\n" && g.hash.size() == 64 && g.hash == sha256_hex(g.text));
    CHECK(cowork::read_file(cwd, "nope.md").status == 404);
    CHECK(cowork::read_file(cwd, "sub").status == 404);
    CHECK(cowork::read_file(cwd, "").status == 400);
    CHECK(cowork::read_file(cwd, "../outside.txt").status == 403);
    CHECK(cowork::read_file(cwd, "sub/../../files2/x").status == 403);
    CHECK(cowork::read_file(cwd, "/etc/passwd").status == 403);
    // symlinks out of the folder are refused, symlinks inside work
    put(g_base / "target.txt", "outside\n");
    fs::create_symlink(g_base / "target.txt", root / "link-out.txt");
    CHECK(cowork::read_file(cwd, "link-out.txt").status == 403);
    fs::create_symlink(root / "sub/doc.md", root / "link-in.md");
    CHECK(cowork::read_file(cwd, "link-in.md").status == 200);
    // size and binary
    put(root / "big.txt", std::string(cowork::kMaxFileBytes + 1, 'a'));
    CHECK(cowork::read_file(cwd, "big.txt").status == 413);
    put(root / "bin.dat", std::string("ab\0cd", 5));
    CHECK(cowork::read_file(cwd, "bin.dat").status == 415);

    // PUT: hash check
    auto w1 = cowork::write_file(cwd, "sub/doc.md", "# Title\nnew body\n", g.hash);
    CHECK(w1.status == 200 && w1.hash == sha256_hex("# Title\nnew body\n") && slurp(root / "sub/doc.md") == "# Title\nnew body\n");
    auto w2 = cowork::write_file(cwd, "sub/doc.md", "stale write", g.hash);   // the old hash no longer matches
    CHECK(w2.status == 409 && w2.hash == w1.hash && slurp(root / "sub/doc.md") == "# Title\nnew body\n");
    CHECK(cowork::write_file(cwd, "sub/doc.md", "x", "").status == 409);
    CHECK(cowork::write_file(cwd, "../evil.txt", "x", "").status == 403);
    CHECK(!fs::exists(g_base / "evil.txt"));
    CHECK(cowork::write_file(cwd, "link-out.txt", "x", sha256_hex("outside\n")).status == 403);
    CHECK(slurp(g_base / "target.txt") == "outside\n");
    CHECK(cowork::write_file(cwd, "x.txt", std::string("a\0b", 3), "").status == 415);
    CHECK(cowork::write_file(cwd, "x.txt", std::string(cowork::kMaxFileBytes + 1, 'a'), "").status == 413);
    // a new document: empty base hash creates it; a base hash for a missing file conflicts
    auto w3 = cowork::write_file(cwd, "new/draft.md", "first\n", "");
    CHECK(w3.status == 200 && slurp(root / "new/draft.md") == "first\n");
    CHECK(cowork::write_file(cwd, "gone.md", "x", sha256_hex("zzz")).status == 409);
    // no temp files left behind
    for (auto & e : fs::recursive_directory_iterator(root)) CHECK(e.path().string().find(".judas-tmp-") == std::string::npos);
}

int main() {
    g_base = fs::path(JUDAS_TEST_TMP) / ("cowork-" + std::to_string(getpid()));
    fs::remove_all(g_base);
    fs::create_directories(g_base);
    test_modes_and_prompts();
    test_gating();
    test_migration();
    test_tools();
    test_write_mode_refusal();
    test_list_documents();
    test_mode_change();
    test_accept();
    test_files();
    fs::remove_all(g_base);
    TEST_MAIN_END();
}
