#include <chrono>
#include <filesystem>
#include <regex>
#include <set>
#include <thread>

#include <sqlite3.h>

#include "store/store.h"
#include "test.h"

using namespace judas;
namespace fs = std::filesystem;

static std::string tmp_db(const char * name) {
    fs::path d = fs::path(JUDAS_TEST_TMP) / "judas-test-store";
    fs::create_directories(d);
    fs::path p = d / name;
    fs::remove(p);
    fs::remove(fs::path(p.string() + "-wal"));
    fs::remove(fs::path(p.string() + "-shm"));
    return p.string();
}

static void test_ids() {
    std::string id = new_id("s");
    CHECK(std::regex_match(id, std::regex("s-[0-9]{8}-[0-9]{6}-[0-9a-f]{4}")));
    std::set<std::string> seen;
    for (int i = 0; i < 20; i++) seen.insert(new_id("a"));
    CHECK(seen.size() > 15);
    long long t = now_ms();
    CHECK(t > 1700000000000LL);
}

static void test_crud() {
    Store st(tmp_db("crud.db"));
    auto s = st.create_session("first", "/tmp/x", "big");
    CHECK(s.id.rfind("s-", 0) == 0);
    auto got = st.get_session(s.id);
    REQUIRE(got.has_value());
    CHECK(got->title == "first" && got->cwd == "/tmp/x" && got->model == "big");
    CHECK(!st.get_session("nope").has_value());

    auto s2 = st.create_session("second", "/", "small");
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
    st.add_message(s.id, "", "user", json{{"role", "user"}, {"content", "bump"}});  // s now newest
    auto list = st.list_sessions();
    REQUIRE(list.size() == 2);
    CHECK(list[0].id == s.id);
    CHECK(st.list_sessions(1).size() == 1);

    st.rename_session(s2.id, "renamed 'quoted'; DROP TABLE sessions;--");
    CHECK(st.get_session(s2.id)->title == "renamed 'quoted'; DROP TABLE sessions;--");

    auto a = st.create_agent(s.id, "", "lead", "do the thing");
    CHECK(a.status == "queued");
    auto b = st.create_agent(s.id, a.id, "worker", "sub task");
    a.status = "done"; a.result = "ok"; a.prompt_tokens = 10; a.completion_tokens = 5;
    st.update_agent(a);
    auto ga = st.get_agent(a.id);
    REQUIRE(ga.has_value());
    CHECK(ga->status == "done" && ga->result == "ok" && ga->prompt_tokens == 10 && ga->completion_tokens == 5);
    CHECK(ga->task == "do the thing");
    auto ags = st.list_agents(s.id);
    REQUIRE(ags.size() == 2);
    CHECK(ags[0].id == a.id && ags[1].parent == a.id);
    CHECK(!st.get_agent("zz").has_value());
}

static void test_messages() {
    Store st(tmp_db("msg.db"));
    auto s = st.create_session("t", "/", "big");
    auto a = st.create_agent(s.id, "", "lead", "t");
    json m1 = {{"role", "user"}, {"content", "h\xc3\xa9llo \"world\" \xe2\x9c\x93"}};
    json m2 = {{"role", "assistant"}, {"content", nullptr},
               {"tool_calls", json::array({json{{"id", "c1"}, {"type", "function"},
                                                {"function", {{"name", "read_file"}, {"arguments", "{\"path\":\"/etc/hosts\"}"}}}}})}};
    long long i1 = st.add_message(s.id, a.id, "user", m1);
    long long i2 = st.add_message(s.id, a.id, "assistant", m2);
    CHECK(i2 > i1);
    auto ms = st.messages(s.id, a.id);
    REQUIRE(ms.size() == 2);
    CHECK(ms[0].body == m1);
    CHECK(ms[1].body == m2);
    CHECK(ms[0].role == "user" && ms[1].role == "assistant");
    CHECK(st.messages(s.id, "other").empty());

    bool threw = false;
    try { st.add_message("no-such-session", "", "user", m1); } catch (const std::exception &) { threw = true; }
    CHECK(threw);
}

static void test_search_and_cascade() {
    Store st(tmp_db("search.db"));
    auto s1 = st.create_session("one", "/", "big");
    auto s2 = st.create_session("two", "/", "big");
    st.add_message(s1.id, "", "user", json{{"content", "the quick brown fox"}});
    st.add_message(s1.id, "", "assistant", json{{"content", "jumps over the lazy dog"}});
    st.add_message(s2.id, "", "user", json{{"content", "quick thinking: C++ (std::vector) & \"quotes\""}});
    st.add_message(s2.id, "", "assistant",
                   json{{"content", ""}, {"tool_calls", json::array({json{{"function", {{"name", "grep"}, {"arguments", "{\"pattern\":\"needle\"}"}}}}})}});

    auto r = st.search("quick");
    CHECK(r.size() == 2);
    CHECK(r.size() == 2 && r[0].session == s2.id);  // newest first
    CHECK(st.search("quick fox").size() == 1);
    CHECK(st.search("needle").size() == 1);
    CHECK(st.search("zzzzqq").empty());
    CHECK(st.search("").empty());
    CHECK(st.search("   ").empty());
    // punctuation must never throw
    for (const char * q : {"\"", "\"unbalanced", "AND", "OR NOT", "*", "-", "a:b", "(", ")", "NEAR(", "C++", "std::vector", "'; DROP TABLE messages;--", "^x", "{}"}) {
        bool threw = false;
        try { st.search(q); } catch (const std::exception & e) { threw = true; std::fprintf(stderr, "query [%s]: %s\n", q, e.what()); }
        CHECK(!threw);
    }
    CHECK(st.search("quotes").size() == 1);
    CHECK(st.search("vector").size() == 1);
    CHECK(st.search("quick", 1).size() == 1);

    st.delete_session(s2.id);
    CHECK(!st.get_session(s2.id).has_value());
    CHECK(st.search("needle").empty());
    CHECK(st.search("quick").size() == 1);
    CHECK(st.messages(s2.id, "").empty());
    CHECK(st.search("fox").size() == 1);
}

static void test_cascade_agents() {
    Store st(tmp_db("cascade.db"));
    auto s = st.create_session("t", "/", "m");
    auto a = st.create_agent(s.id, "", "lead", "x");
    st.add_message(s.id, a.id, "user", json{{"content", "hi"}});
    st.delete_session(s.id);
    CHECK(!st.get_agent(a.id).has_value());
    CHECK(st.list_agents(s.id).empty());
}

static void test_persistence_and_threads() {
    std::string path = tmp_db("persist.db");
    std::string sid;
    {
        Store st(path);
        sid = st.create_session("keep", "/", "m").id;
        st.add_message(sid, "", "user", json{{"content", "persisted words"}});
    }
    Store st(path);  // reopen: schema exists, data remains
    CHECK(st.get_session(sid).has_value());
    CHECK(st.search("persisted").size() == 1);

    std::vector<std::thread> ts;
    for (int t = 0; t < 4; t++)
        ts.emplace_back([&, t] {
            for (int i = 0; i < 50; i++) {
                st.add_message(sid, "ag" + std::to_string(t), "user", json{{"content", "thread msg " + std::to_string(i)}});
                st.search("thread");
            }
        });
    for (auto & t : ts) t.join();
    size_t n = 0;
    for (int t = 0; t < 4; t++) n += st.messages(sid, "ag" + std::to_string(t)).size();
    CHECK(n == 200);
}

// A database from before the mode column existed opens, gains the column, and old rows read as "chat".
static void test_mode_migration() {
    std::string path = tmp_db("old.db");
    {
        sqlite3 * db = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        const char * old_schema =
            "CREATE TABLE sessions (id TEXT PRIMARY KEY, title TEXT NOT NULL, cwd TEXT NOT NULL, "
            "model TEXT NOT NULL, created_ms INTEGER NOT NULL, updated_ms INTEGER NOT NULL);"
            "INSERT INTO sessions VALUES ('s-old', 'old chat', '', 'big', 1000, 2000);";
        REQUIRE(sqlite3_exec(db, old_schema, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    for (int round = 0; round < 2; round++) {   // the second open must find the column already there
        Store st(path);
        auto old = st.get_session("s-old");
        REQUIRE(old.has_value());
        CHECK(old->mode == "chat");
        CHECK(old->title == "old chat");
        auto n = st.create_session("new", "/tmp", "big", "code");
        CHECK(st.get_session(n.id)->mode == "code");
        CHECK(st.list_sessions().size() == (size_t) (2 + round));
    }
}

static void test_mode() {
    Store st(tmp_db("mode.db"));
    auto a = st.create_session("a", "/", "m");
    CHECK(a.mode == "chat");
    auto b = st.create_session("b", "/", "m", "cowork");
    CHECK(st.get_session(b.id)->mode == "cowork");
    CHECK(st.set_session_mode(b.id, "code"));
    CHECK(st.get_session(b.id)->mode == "code");
    CHECK(!st.set_session_mode("nope", "code"));
    bool found = false;
    for (auto & s : st.list_sessions()) found = found || (s.id == b.id && s.mode == "code");
    CHECK(found);
}

static void test_pagination() {
    Store st(tmp_db("page.db"));
    auto s = st.create_session("t", "/", "m");
    auto a = st.create_agent(s.id, "", "lead", "t");
    auto w = st.create_agent(s.id, a.id, "worker", "t");
    std::vector<long long> ids;
    for (int i = 0; i < 25; i++)
        ids.push_back(st.add_message(s.id, i % 2 ? w.id : a.id, "user", json{{"content", "m" + std::to_string(i)}}));
    bool more = false;
    long long total = 0;
    auto all = st.messages_page(s.id, 0, 0, &more, &total);   // no limit: everything, across agents
    CHECK(all.size() == 25 && !more && total == 25);
    auto p1 = st.messages_page(s.id, 0, 10, &more, &total);   // newest 10, oldest first
    REQUIRE(p1.size() == 10);
    CHECK(more && total == 25);
    CHECK(p1.front().id == ids[15] && p1.back().id == ids[24]);
    auto p2 = st.messages_page(s.id, p1.front().id, 10, &more);
    REQUIRE(p2.size() == 10);
    CHECK(more && p2.front().id == ids[5] && p2.back().id == ids[14]);
    auto p3 = st.messages_page(s.id, p2.front().id, 10, &more);
    CHECK(p3.size() == 5 && !more && p3.front().id == ids[0]);
    CHECK(st.last_message_id(s.id) == ids[24]);
    CHECK(st.last_message_id("none") == 0);

    // session list cursor
    for (int i = 0; i < 5; i++) {
        st.create_session("x" + std::to_string(i), "/", "m");
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    auto first = st.list_sessions(3);
    REQUIRE(first.size() == 3);
    auto next = st.list_sessions(10, first.back().updated_ms);
    CHECK(next.size() == 3);   // 6 sessions, 3 already seen
    for (auto & x : next) CHECK(x.updated_ms < first.back().updated_ms);
}

int main() {
    test_ids();
    test_mode_migration();
    test_mode();
    test_pagination();
    test_crud();
    test_messages();
    test_search_and_cascade();
    test_cascade_agents();
    test_persistence_and_threads();
    std::printf("test_store: done\n");
    TEST_MAIN_END();
}
