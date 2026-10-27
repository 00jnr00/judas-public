// Session search: FTS5 backfill and triggers, cross-session hits, and the session_search tool.
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <string>
#include <thread>

#include <sqlite3.h>

#include "store/store.h"
#include "test.h"
#include "tools/session_tools.h"
#include "tools/tools.h"

using namespace judas;
namespace fs = std::filesystem;

static fs::path g_root;

static std::string tmp_db(const char * name) {
    fs::path p = g_root / name;
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

static void put_old_db(const std::string & path) {
    sqlite3 * db = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    const char * old_schema =
        "CREATE TABLE sessions (id TEXT PRIMARY KEY, title TEXT NOT NULL, cwd TEXT NOT NULL, "
        "model TEXT NOT NULL, created_ms INTEGER NOT NULL, updated_ms INTEGER NOT NULL);"
        "CREATE TABLE messages (id INTEGER PRIMARY KEY AUTOINCREMENT, session TEXT NOT NULL, "
        "agent TEXT NOT NULL, role TEXT NOT NULL, body TEXT NOT NULL, content TEXT NOT NULL DEFAULT '', "
        "created_ms INTEGER NOT NULL);"
        "INSERT INTO sessions VALUES ('s-old', 'legacy notes', '', 'big', 1000, 2000);"
        "INSERT INTO messages(session, agent, role, body, content, created_ms) VALUES "
        "('s-old', '', 'user', '{\"content\":\"legacy needle in old history\"}', "
        "'legacy needle in old history', 3000);";
    REQUIRE(sqlite3_exec(db, old_schema, nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(db);
}

// A message table from before the FTS table existed is backfilled on open, so old turns are searchable.
static void test_backfill() {
    std::string path = tmp_db("old.db");
    put_old_db(path);

    Store st(path);
    auto hits = st.search_messages("legacy");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].session == "s-old");
    CHECK(hits[0].title == "legacy notes");
    CHECK(hits[0].message_id == 1);
    CHECK(hits[0].role == "user");
    CHECK(hits[0].created_ms == 3000);
    CHECK(hits[0].snippet.find("[legacy]") != std::string::npos);

    auto s = st.create_session("fresh", "/", "big");
    st.add_message(s.id, "", "user", json{{"content", "a brand new needle"}});
    CHECK(st.search_messages("needle").size() == 2);

    Store reopened(path);   // a second open must not lose or duplicate index rows
    CHECK(reopened.search_messages("legacy").size() == 1);
    CHECK(reopened.search_messages("needle").size() == 2);
}

static void test_search_messages() {
    Store st(tmp_db("search.db"));
    auto s1 = st.create_session("alpha work", "/a", "big");
    auto s2 = st.create_session("beta notes", "/b", "big");
    long long a1 = st.add_message(s1.id, "", "user", json{{"content", "the quick brown fox jumps over the lazy dog"}});
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    long long b1 = st.add_message(
        s2.id, "", "user", json{{"content", "quick thinking: C++ (std::vector) & \"quotes\""}});
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    long long b2 = st.add_message(s2.id, "", "assistant", json{{"content", "another quick note"}});

    auto all = st.search_messages("quick");
    REQUIRE(all.size() == 3);
    CHECK(all[0].message_id == b2 && all[0].session == s2.id);   // newest first
    CHECK(all[2].message_id == a1 && all[2].session == s1.id);
    CHECK(all[0].title == "beta notes" && all[2].title == "alpha work");
    CHECK(all[0].role == "assistant" && all[2].role == "user");
    CHECK(all[0].created_ms > 0);
    for (const auto & h : all) CHECK(h.snippet.find("[quick]") != std::string::npos);

    // implicit AND across terms, quoted punctuation and FTS syntax must never throw
    CHECK(st.search_messages("quick fox").size() == 1);
    for (const char * q : {"\"", "\"unbalanced", "AND", "OR NOT", "*", "-", "a:b", "(", ")", "NEAR(", "C++",
                           "std::vector", "'; DROP TABLE messages;--", "^x", "{}"}) {
        bool threw = false;
        try { st.search_messages(q); } catch (const std::exception & e) {
            threw = true;
            std::fprintf(stderr, "query [%s]: %s\n", q, e.what());
        }
        CHECK(!threw);
    }
    CHECK(st.search_messages("quotes").size() == 1);
    CHECK(st.search_messages("vector").size() == 1);

    // session filter, limit and empty input
    auto one = st.search_messages("quick", 10, s1.id);
    REQUIRE(one.size() == 1);
    CHECK(one[0].message_id == a1 && one[0].title == "alpha work");
    CHECK(st.search_messages("quick", 2).size() == 2);
    CHECK(st.search_messages("quick", 2)[0].message_id == b2);
    CHECK(st.search_messages("quick", 10, "no-such-session").empty());
    CHECK(st.search_messages("").empty());
    CHECK(st.search_messages("   ").empty());
    CHECK(st.search_messages("quick", 0).empty());

    // a long message yields a bounded snippet, not the whole text
    std::string long_text;
    for (int i = 0; i < 40; i++) long_text += "filler" + std::to_string(i) + " ";
    long_text += "needle ";
    for (int i = 0; i < 40; i++) long_text += "tail" + std::to_string(i) + " ";
    st.add_message(s1.id, "", "user", json{{"content", long_text}});
    auto snip = st.search_messages("needle");
    REQUIRE(snip.size() == 1);
    CHECK(snip[0].snippet.find("[needle]") != std::string::npos);
    CHECK(snip[0].snippet.size() < long_text.size());
    CHECK(snip[0].snippet.find("filler0") == std::string::npos ||
          snip[0].snippet.find("tail39") == std::string::npos);
}

// INSERT and DELETE triggers keep the index aligned; UPDATE is tested through a second connection.
static void test_triggers() {
    std::string path = tmp_db("triggers.db");
    Store st(path);
    auto s = st.create_session("trigger test", "/", "big");
    long long id = st.add_message(s.id, "", "user", json{{"content", "original haystack"}});
    CHECK(st.search_messages("original").size() == 1);

    sqlite3 * raw = nullptr;
    REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
    std::string upd = "UPDATE messages SET content='replacement haystack' WHERE id=" + std::to_string(id);
    REQUIRE(sqlite3_exec(raw, upd.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK);
    sqlite3_close(raw);

    CHECK(st.search_messages("original").empty());
    auto hits = st.search_messages("replacement");
    REQUIRE(hits.size() == 1);
    CHECK(hits[0].message_id == id);

    st.delete_session(s.id);   // cascades messages, and the delete trigger drops the index row
    CHECK(st.search_messages("replacement").empty());
    CHECK(st.search_messages("haystack").empty());
}

static void test_tool() {
    Store st(tmp_db("tool.db"));
    auto s1 = st.create_session("first thread", "/a", "big");
    auto s2 = st.create_session("second thread", "/b", "big");
    st.add_message(s1.id, "", "user", json{{"content", "alpha uniqueword one"}});
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    st.add_message(s2.id, "", "user", json{{"content", "beta uniqueword two"}});
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    st.add_message(s2.id, "", "assistant", json{{"content", "gamma uniqueword three"}});

    ToolRegistry reg;
    register_session_tools(reg, st);
    REQUIRE(reg.find("session_search") != nullptr);
    ToolContext ctx;

    auto r = reg.call("session_search", {{"query", "uniqueword"}}, ctx);
    REQUIRE(r.ok);
    CHECK(r.output.find("[uniqueword]") != std::string::npos);
    CHECK(r.output.find("first thread") != std::string::npos);
    int lines = 1;
    for (char c : r.output) lines += c == '\n';
    CHECK(lines == 3);

    // newest first and capped
    auto capped = reg.call("session_search", {{"query", "uniqueword"}, {"limit", 1}}, ctx);
    REQUIRE(capped.ok);
    CHECK(capped.output.find("gamma uniqueword three") != std::string::npos ||
          capped.output.find("three") != std::string::npos);
    CHECK(capped.output.find("alpha uniqueword one") == std::string::npos);
    for (char c : capped.output) CHECK(c != '\n');

    // session filter
    auto filtered = reg.call("session_search", {{"query", "uniqueword"}, {"session", s1.id}}, ctx);
    REQUIRE(filtered.ok);
    CHECK(filtered.output.find("first thread") != std::string::npos);
    CHECK(filtered.output.find("second thread") == std::string::npos);

    auto none = reg.call("session_search", {{"query", "zzzznotfound"}}, ctx);
    CHECK(none.ok && none.output.find("no matching") != std::string::npos);
    auto empty = reg.call("session_search", {{"query", "   "}}, ctx);
    CHECK(!empty.ok);
    auto missing = reg.call("session_search", json::object(), ctx);
    CHECK(!missing.ok && missing.output.find("query") != std::string::npos);
    auto bad_limit = reg.call("session_search", {{"query", "uniqueword"}, {"limit", 0}}, ctx);
    CHECK(!bad_limit.ok);
}

int main() {
    g_root = fs::path("/tmp/opencode") / ("judas-session-search-" + std::to_string(getpid()));
    fs::remove_all(g_root);
    fs::create_directories(g_root);

    test_backfill();
    test_search_messages();
    test_triggers();
    test_tool();

    fs::remove_all(g_root);
    std::printf("test_session_search: done\n");
    TEST_MAIN_END();
}
