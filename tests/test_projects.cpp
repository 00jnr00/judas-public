// Projects: the store (and its migration), the folder rules, the HTTP API, session attach and the system prompt.
#include <sqlite3.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#include "agent/agent.h"
#include "cpp-httplib/httplib.h"
#include "projects/projects.h"
#include "server/server.h"
#include "test.h"
#include "tools/project_tools.h"

namespace fs = std::filesystem;
using namespace judas;

// Records the system prompt of every request so a test can see what the model would be given.
class CaptureEngine : public Engine {
public:
    ChatResult chat(const ChatRequest & req, const std::function<bool(const ChatDelta &)> & on_delta,
                    const std::atomic<bool> *) override {
        {
            std::lock_guard lk(mu);
            for (auto & m : req.messages)
                if (m.role == "system") last_system = m.content;
            calls++;
        }
        ChatResult r;
        r.message.role = "assistant";
        r.message.content = "ok";
        r.finish_reason = "stop";
        r.prompt_tokens = 5;
        r.completion_tokens = 1;
        if (on_delta) on_delta({"ok", ""});
        return r;
    }
    EngineState state() const override { EngineState s; s.model = "fake"; s.loaded = true; s.max_agents = 4; return s; }
    std::string model_name() const override { return "fake"; }
    int count_tokens(const std::string & t) const override { return (int) t.size() / 4; }
    std::string system() { std::lock_guard lk(mu); return last_system; }
    int count() { std::lock_guard lk(mu); return calls; }
private:
    std::mutex mu;
    std::string last_system;
    int calls = 0;
};

static fs::path g_base;

static void test_slug() {
    CHECK(projects::slugify("My Cool Project!") == "my-cool-project");
    CHECK(projects::slugify("  --Hello__World--  ") == "hello-world");
    CHECK(projects::slugify("???") == "project");
    CHECK(projects::slugify("") == "project");
    CHECK(projects::slugify("Caf\xc3\xa9 10") == "caf-10");
    CHECK(projects::slugify(std::string(200, 'a')).size() == 48);
    fs::path root = g_base / "slugroot";
    fs::create_directories(root / "demo");
    CHECK(projects::unique_folder(root.string(), "Demo") == (root / "demo-2").string());
    fs::create_directories(root / "demo-2");
    CHECK(projects::unique_folder(root.string(), "Demo") == (root / "demo-3").string());
    CHECK(projects::unique_folder(root.string(), "Fresh One") == (root / "fresh-one").string());
}

static void test_create_kinds_and_delete() {
    Config cfg;
    cfg.projects.root = (g_base / "made").string();
    Store st((g_base / "p1.db").string());

    // "made": the folder appears under the root; a second project with the same name gets a suffix
    auto a = projects::create(st, cfg, "Alpha Lab", "make", "", "be brief");
    REQUIRE(a.project.has_value());
    CHECK(a.project->kind == "made" && a.project->folder == (g_base / "made/alpha-lab").string());
    CHECK(fs::is_directory(a.project->folder));
    auto b = projects::create(st, cfg, "Alpha Lab", "make", "", "");
    REQUIRE(b.project.has_value());
    CHECK(b.project->folder == (g_base / "made/alpha-lab-2").string() && fs::is_directory(b.project->folder));

    // "existing": nothing is created, a missing folder is refused, a relative path is refused
    fs::path ext = g_base / "ext-folder";
    fs::create_directories(ext);
    auto c = projects::create(st, cfg, "Ext", "existing", ext.string(), "");
    REQUIRE(c.project.has_value());
    CHECK(c.project->kind == "existing" && c.project->folder == fs::weakly_canonical(ext).string());
    fs::path missing = g_base / "never-made";
    auto d = projects::create(st, cfg, "Ghost", "existing", missing.string(), "");
    CHECK(!d.project && !fs::exists(missing));
    CHECK(!projects::create(st, cfg, "Rel", "existing", "relative/dir", "").project);
    CHECK(!projects::create(st, cfg, "   ", "make", "", "").project);
    CHECK(!projects::create(st, cfg, "X", "bogus", "", "").project);
    CHECK(st.list_projects().size() == 3);   // failures left no rows
    CHECK(!fs::exists(g_base / "made/x"));

    // a made folder that vanished is made again on demand; a vanished existing folder is an error
    fs::remove_all(a.project->folder);
    CHECK(projects::ensure_folder(*a.project).empty() && fs::is_directory(a.project->folder));
    fs::path gone = g_base / "gone-later";
    fs::create_directories(gone);
    auto e = projects::create(st, cfg, "Gone", "existing", gone.string(), "");
    fs::remove_all(gone);
    CHECK(!projects::ensure_folder(*e.project).empty() && !fs::exists(gone));

    // deleting the record never touches the folder (made or existing)
    std::ofstream(a.project->folder + "/keep.txt") << "x";
    auto sess = st.create_session("t", a.project->folder, "m");
    st.set_session_project(sess.id, a.project->id);
    CHECK(st.get_session(sess.id)->project_id == a.project->id);
    auto r = projects::remove(st, a.project->id, false);
    CHECK(r.ok && !r.folder_removed);
    CHECK(fs::exists(a.project->folder + "/keep.txt"));
    CHECK(!st.get_project(a.project->id));
    CHECK(st.get_session(sess.id).has_value() && st.get_session(sess.id)->project_id.empty());   // session kept, detached
    r = projects::remove(st, c.project->id, false);
    CHECK(r.ok && fs::is_directory(ext));

    // folder deletion is explicit, and refuses the home folder and shallow system paths
    CHECK(!projects::folder_delete_refusal(b.project->folder).empty() == false);
    CHECK(!projects::folder_delete_refusal("/").empty());
    CHECK(!projects::folder_delete_refusal("/usr").empty());
    CHECK(!projects::folder_delete_refusal("/home").empty());
    CHECK(!projects::folder_delete_refusal(expand_home("~")).empty());
    CHECK(!projects::folder_delete_refusal("relative").empty());
    r = projects::remove(st, b.project->id, true);
    CHECK(r.ok && r.folder_removed && !fs::exists(b.project->folder));
    CHECK(!projects::remove(st, "p-nope", false).ok);
}

static void test_migration() {
    // a database from before projects existed: sessions without project_id, no projects table
    std::string path = (g_base / "old.db").string();
    fs::remove(path);
    {
        sqlite3 * db = nullptr;
        REQUIRE(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
        const char * sql =
            "CREATE TABLE sessions (id TEXT PRIMARY KEY, title TEXT NOT NULL, cwd TEXT NOT NULL, model TEXT NOT NULL,"
            " created_ms INTEGER NOT NULL, updated_ms INTEGER NOT NULL, mode TEXT NOT NULL DEFAULT 'chat');"
            "INSERT INTO sessions VALUES ('s-old', 'old chat', '/tmp/old', 'm', 1000, 2000, 'code');";
        REQUIRE(sqlite3_exec(db, sql, nullptr, nullptr, nullptr) == SQLITE_OK);
        sqlite3_close(db);
    }
    Store st(path);
    auto s = st.get_session("s-old");
    REQUIRE(s.has_value());
    CHECK(s->cwd == "/tmp/old" && s->mode == "code" && s->project_id.empty() && !s->location_used);
    CHECK(st.list_projects().empty());
    Config cfg;
    cfg.projects.root = (g_base / "made2").string();
    auto p = projects::create(st, cfg, "After", "make", "", "");
    REQUIRE(p.project.has_value());
    CHECK(st.set_session_project("s-old", p.project->id));
    CHECK(st.list_sessions(10, 0, p.project->id).size() == 1);
    CHECK(st.list_sessions(10, 0, "p-other").empty());
    CHECK(st.list_sessions(10, 0).size() == 1);
    // reopening a migrated database is a no-op
    Store again(path);
    CHECK(again.get_session("s-old")->project_id == p.project->id);
}

static void test_tools() {
    Config cfg;
    cfg.projects.root = (g_base / "made3").string();
    Store st((g_base / "p3.db").string());
    ToolRegistry reg;
    register_project_tools(reg, st);
    ToolContext ctx;
    ctx.cfg = &cfg;
    auto s = st.create_session("t", "/tmp", "m");
    ctx.session = s.id;
    auto r = reg.call("project_info", json::object(), ctx);
    CHECK(r.ok && r.output.find("not in a project") != std::string::npos);
    r = reg.call("project_list", json::object(), ctx);
    CHECK(r.ok && r.output.find("No projects") != std::string::npos);
    auto p = projects::create(st, cfg, "Tool Proj", "make", "", "line one\nline two");
    REQUIRE(p.project.has_value());
    st.set_session_project(s.id, p.project->id);
    r = reg.call("project_info", json::object(), ctx);
    CHECK(r.ok && r.output.find("Tool Proj") != std::string::npos && r.output.find(p.project->folder) != std::string::npos &&
          r.output.find("line two") != std::string::npos);
    r = reg.call("project_list", json::object(), ctx);
    CHECK(r.ok && r.output.find("Tool Proj") != std::string::npos && r.output.find("line one") != std::string::npos);
}

static bool wait_idle(CaptureEngine & eng, AgentManager & agents, const std::string & sid, int calls) {
    for (int i = 0; i < 200; i++) {
        if (eng.count() >= calls && agents.turn_info(sid).status != "running") return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
}

static void test_http_and_prompt() {
    Config cfg;
    cfg.data_dir = (g_base / "data").string();
    cfg.agents.role_dirs = {};
    cfg.projects.root = (g_base / "made4").string();
    cfg.server.port = 18400 + getpid() % 1000;
    CaptureEngine engine;
    Store store((g_base / "p4.db").string());
    Journal journal(g_base / "snap");
    AgentManager agents(cfg, engine, store, journal);
    Server server(cfg, engine, agents, store, journal);
    std::thread th([&] { server.run(); });
    httplib::Client cli("127.0.0.1", cfg.server.port);
    cli.set_read_timeout(20);
    bool up = false;
    for (int i = 0; i < 100 && !up; i++) {
        auto r = cli.Get("/health");
        up = r && r->status == 200;
        if (!up) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(up);
    auto post = [&](const std::string & path, const json & b) { return cli.Post(path, b.dump(), "application/json"); };

    // create: the Judas-made kind
    auto r = post("/api/projects", {{"name", "French Tutor"}, {"mode", "make"}, {"instructions", "Always answer in French."}});
    REQUIRE(r && r->status == 200);
    json pj = json::parse(r->body);
    std::string pid = pj["id"], folder = pj["folder"];
    CHECK(pj["kind"] == "made" && folder == (g_base / "made4/french-tutor").string() && fs::is_directory(folder));
    // the same name again does not collide
    r = post("/api/projects", {{"name", "French Tutor"}});
    REQUIRE(r && r->status == 200);
    CHECK(json::parse(r->body)["folder"] == (g_base / "made4/french-tutor-2").string());
    // existing folder; a folder that is not there is refused and not created
    fs::path ext = g_base / "my-repo";
    fs::create_directories(ext);
    r = post("/api/projects", {{"name", "Repo"}, {"folder", ext.string()}});
    REQUIRE(r && r->status == 200);
    json ej = json::parse(r->body);
    CHECK(ej["kind"] == "existing");
    r = post("/api/projects", {{"name", "Nope"}, {"mode", "existing"}, {"folder", (g_base / "nope").string()}});
    CHECK(r && r->status == 400 && !fs::exists(g_base / "nope"));
    r = cli.Get("/api/projects");
    REQUIRE(r && r->status == 200);
    CHECK(json::parse(r->body)["projects"].size() == 3);

    // a message with project: the session cwd is the project folder, the instructions reach the system prompt
    r = post("/api/sessions/new/messages", {{"content", "bonjour"}, {"project", pid}, {"cwd", "/tmp/ignored"}, {"mode", "code"}});
    REQUIRE(r && r->status == 200);
    std::string sid = json::parse(r->body)["session"];
    REQUIRE(wait_idle(engine, agents, sid, 1));
    auto sess = store.get_session(sid);
    REQUIRE(sess.has_value());
    CHECK(sess->cwd == folder && sess->project_id == pid);
    CHECK(engine.system().find("Always answer in French.") != std::string::npos);
    CHECK(engine.system().find("Working directory: " + folder) != std::string::npos);
    CHECK(engine.system().find("Project: French Tutor") != std::string::npos);

    // session list filter, project name, project detail with its sessions
    r = cli.Get("/api/sessions?project=" + pid);
    REQUIRE(r && r->status == 200);
    json sl = json::parse(r->body);
    CHECK(sl["sessions"].size() == 1 && sl["sessions"][0]["project_name"] == "French Tutor");
    r = cli.Get("/api/sessions?project=p-none");
    CHECK(r && json::parse(r->body)["sessions"].empty());
    r = cli.Get("/api/projects/" + pid);
    CHECK(r && json::parse(r->body)["sessions"].size() == 1);

    // editing instructions applies from the next turn
    r = cli.Patch("/api/projects/" + pid, json({{"instructions", "Answer in Spanish now."}}).dump(), "application/json");
    CHECK(r && r->status == 200);
    r = post("/api/sessions/" + sid + "/messages", {{"content", "hola"}});
    REQUIRE(r && r->status == 200);
    REQUIRE(wait_idle(engine, agents, sid, 2));
    CHECK(engine.system().find("Answer in Spanish now.") != std::string::npos &&
          engine.system().find("Always answer in French.") == std::string::npos);

    // a plain session, then attach it by PATCH: cwd follows, the instructions apply; detach keeps the cwd
    fs::create_directories(g_base / "plain");
    r = post("/api/sessions", {{"cwd", (g_base / "plain").string()}});
    REQUIRE(r && r->status == 200);
    std::string sid2 = json::parse(r->body)["id"];
    r = post("/api/sessions/" + sid2 + "/messages", {{"content", "first"}});
    REQUIRE(wait_idle(engine, agents, sid2, 3));
    CHECK(engine.system().find("Answer in Spanish now.") == std::string::npos);
    r = cli.Patch("/api/sessions/" + sid2, json({{"project", pid}}).dump(), "application/json");
    CHECK(r && r->status == 200 && json::parse(r->body)["project_id"] == pid);
    CHECK(store.get_session(sid2)->cwd == folder);
    r = post("/api/sessions/" + sid2 + "/messages", {{"content", "second"}});
    REQUIRE(wait_idle(engine, agents, sid2, 4));
    CHECK(engine.system().find("Answer in Spanish now.") != std::string::npos);
    r = cli.Patch("/api/sessions/" + sid2, json({{"project", ""}}).dump(), "application/json");
    CHECK(r && r->status == 200 && store.get_session(sid2)->project_id.empty() && store.get_session(sid2)->cwd == folder);
    r = cli.Patch("/api/sessions/" + sid2, json({{"project", "p-missing"}}).dump(), "application/json");
    CHECK(r && r->status == 404);

    // delete: the record goes, the folder stays; the folder needs both flags
    std::ofstream(folder + "/notes.txt") << "mine";
    r = cli.Delete("/api/projects/" + pid + "?delete_folder=1");
    CHECK(r && r->status == 400 && store.get_project(pid).has_value() && fs::exists(folder + "/notes.txt"));
    r = cli.Delete("/api/projects/" + pid);
    CHECK(r && r->status == 200 && json::parse(r->body)["folder_removed"] == false);
    CHECK(!store.get_project(pid) && fs::exists(folder + "/notes.txt"));
    CHECK(store.get_session(sid)->project_id.empty());   // the chat survives
    r = cli.Delete("/api/projects/" + pid);
    CHECK(r && r->status == 404);
    // the explicit, doubly confirmed folder delete
    std::string eid = ej["id"];
    std::ofstream(ext / "f.txt") << "x";
    r = cli.Delete("/api/projects/" + eid + "?delete_folder=1&confirm_folder=1");
    CHECK(r && r->status == 200 && json::parse(r->body)["folder_removed"] == true && !fs::exists(ext));

    server.stop();
    th.join();
}

int main() {
    g_base = fs::path(JUDAS_TEST_TMP) / ("judas-projects-test-" + std::to_string(getpid()));
    fs::remove_all(g_base);
    fs::create_directories(g_base);
    test_slug();
    test_create_kinds_and_delete();
    test_migration();
    test_tools();
    test_http_and_prompt();
    fs::remove_all(g_base);
    TEST_MAIN_END();
}
