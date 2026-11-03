// TUI snapshots: parsing, diff stats, id resolution, colored diff rows, revert preview and result, paging.
#include <cstdlib>
#include <ctime>

#include "test.h"
#include "tui/snapshots.h"

using namespace judas;
using namespace judas::tui;

static const char * kDiff =
    "--- /tmp/a.txt\n+++ /tmp/a.txt\n@@ -1,3 +1,3 @@\n one\n-two\n+TWO\n+three\n three\n"
    "--- /tmp/b.txt\n+++ /tmp/b.txt\n@@ -1,1 +1,1 @@\n-x\n+y\n";

int main() {
    setenv("TZ", "UTC", 1);
    tzset();

    judas::json arr = judas::json::parse(R"([
      {"id":"20261102-100000-0001","session":"s1","agent":"lead","label":"edit_file /w/a.txt","created_ms":1000,"committed":true,"reverted":false,
       "files":[{"path":"/w/a.txt"}],"irreversible":[]},
      {"id":"20261102-110000-0002","session":"s1","agent":"w1","label":"shell","created_ms":3000,"committed":true,"reverted":true,
       "files":[{"path":"/w/src/b.txt"},{"path":"/other/c.txt"}],"irreversible":["systemctl restart x"]},
      {"id":"20261102-120000-0010","session":"s1","agent":"lead","label":"write","created_ms":2000,"files":[]},
      {"nope":1}])");
    auto v = parse_snapshots(arr);
    REQUIRE(v.size() == 3);
    CHECK(v[0].id == "20261102-110000-0002" && v[1].id == "20261102-120000-0010");   // newest first
    CHECK(v[0].reverted && v[0].files.size() == 2 && v[0].irreversible.size() == 1);
    CHECK(parse_snapshots(judas::json::object()).empty());

    auto f = in_folder(v, "/w");
    REQUIRE(f.size() == 2);                                                           // a.txt and src/b.txt
    CHECK(in_folder(v, "/w/src/").size() == 1);
    CHECK(in_folder(v, "/wx").empty());                                               // not a path prefix match
    CHECK(in_folder(v, "/").size() == 2);

    CHECK(resolve_snapshot_id(v, "20261102-110000-0002") == "20261102-110000-0002");
    CHECK(resolve_snapshot_id(v, "20261102-11") == "20261102-110000-0002");
    CHECK(resolve_snapshot_id(v, "20261102") == "");                                  // ambiguous prefix
    CHECK(resolve_snapshot_id(v, "0010") == "20261102-120000-0010");                  // suffix
    CHECK(resolve_snapshot_id(v, "zzz") == "");
    CHECK(resolve_snapshot_id(v, "") == "");

    DiffStat st = diff_stat(kDiff);
    CHECK(st.added == 3 && st.removed == 2 && st.files == 2);
    CHECK(diff_stat("").added == 0);
    CHECK(diff_stat("Binary file differs: /x\n").files == 0);

    // clock: 1970-01-01 00:00:03 UTC
    CHECK(format_clock(3000, 4000) == "00:00");
    CHECK(format_clock(3000, 90000000LL).find("Jan 01") == 0);

    // list rows
    Palette plain = Palette::plain();
    v[0].added = 4; v[0].removed = 1;
    auto rows = render_snapshot_list(v, 5000, 100, plain, "session s1");
    REQUIRE(rows.size() >= 5);
    CHECK(rows[0].find("Snapshots") != std::string::npos && rows[0].find("(3)") != std::string::npos);
    CHECK(rows[1].find("20261102-110000-0002") != std::string::npos && rows[1].find("+4") != std::string::npos &&
          rows[1].find("-1") != std::string::npos && rows[1].find("2 files") != std::string::npos &&
          rows[1].find("(reverted)") != std::string::npos && rows[1].find("w1") != std::string::npos);
    for (auto & r : rows) CHECK(display_width(r) <= 100);
    auto none = render_snapshot_list({}, 0, 80, plain, "folder /x");
    CHECK(none.size() == 2 && none[1].find("none yet") != std::string::npos);
    auto narrow = render_snapshot_list(v, 5000, 70, plain, "s");
    for (size_t i = 1; i + 1 < narrow.size(); i++) CHECK(display_width(narrow[i]) <= 70);

    // colored diff
    Skin sk;
    ThemeLook look = look_for(sk, "");
    auto d = render_diff(kDiff, 60, look);
    std::fprintf(stderr, "diff rows %zu\n", d.size()); REQUIRE(d.size() == 13);
    CHECK(d[0].find(look.diff.head) == 0 && d[2].find(look.diff.hunk) == 0);
    CHECK(d[4].find(look.diff.del) == 0 && d[4].find("-two") != std::string::npos);
    CHECK(d[5].find(look.diff.add) == 0 && d[5].find("+TWO") != std::string::npos);
    CHECK(d[3].find(look.pal.text) == 0);                                              // context line
    CHECK(look.diff.add != look.diff.del);
    for (auto & r : d) CHECK(r.find("48;") == std::string::npos);
    // long lines wrap, tabs and control bytes are made safe
    std::string longline = "+" + std::string(150, 'x') + "\n+\ttab\x01\n";
    auto w = render_diff(longline, 40, look);
    CHECK(w.size() >= 4);
    for (auto & r : w) CHECK(display_width(r) <= 40);
    CHECK(strip_ansi(w.back()).find('\x01') == std::string::npos);
    CHECK(render_diff("", 40, look).empty());

    // revert preview
    auto pv = render_revert_preview(v[0], st, false, 80, plain);
    std::string all;
    for (auto & r : pv) all += r + "\n";
    CHECK(all.find("Revert") != std::string::npos && all.find("/w/src/b.txt") != std::string::npos &&
          all.find("+3") != std::string::npos && all.find("cannot be undone: systemctl restart x") != std::string::npos &&
          all.find("already reverted") != std::string::npos);
    auto pt = render_revert_preview(v[1], st, true, 80, plain);
    all.clear();
    for (auto & r : pt) all += r + "\n";
    CHECK(all.find("everything back") != std::string::npos && all.find("every later one") != std::string::npos);

    // revert result
    auto rr = render_revert_result(judas::json::parse(R"({"ok":true,"restored":["/w/a.txt"],"removed":["/w/n.txt"],"conflicts":["/w/c.txt"],"irreversible":[],"error":""})"), 80, plain);
    all.clear();
    for (auto & r : rr) all += r + "\n";
    CHECK(all.find("Revert finished") != std::string::npos && all.find("restored: /w/a.txt") != std::string::npos &&
          all.find("removed: /w/n.txt") != std::string::npos && all.find("changed since") != std::string::npos);
    auto bad = render_revert_result(judas::json::parse(R"({"ok":false,"error":"locked"})"), 80, plain);
    CHECK(bad[0].find("Revert failed") != std::string::npos && bad[1].find("locked") != std::string::npos);

    // paging
    std::vector<std::string> pg = {"1", "2", "3", "4", "5"};
    size_t next = 0;
    auto p1 = page_slice(pg, 0, 2, &next);
    CHECK(p1.size() == 2 && next == 2);
    auto p3 = page_slice(pg, 4, 2, &next);
    CHECK(p3.size() == 1 && next == 5);
    CHECK(page_slice(pg, 9, 2, &next).empty() && next == 5);

    TEST_MAIN_END();
}
