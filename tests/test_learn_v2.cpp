// Learn v2: question cards, plan, notes and their storage and endpoints.
#include <unistd.h>

#include <filesystem>
#include <fstream>

#include "core/config.h"
#include "test.h"
#include "server/visual_proxy.h"
#include "tools/tools.h"
#include "tutor/learn_store.h"
#include "tutor/tutor.h"

using namespace judas;
namespace fs = std::filesystem;

int main() {
    fs::path root = JUDAS_TEST_TMP "/judas-learn2-test-" + std::to_string(getpid());
    fs::remove_all(root);
    fs::create_directories(root);
    Config cfg;
    cfg.data_dir = (root / "data").string();
    ToolRegistry reg;
    register_learn_tools(reg);
    ToolContext ctx;
    ctx.cfg = &cfg;
    ctx.cwd = root.string();
    ctx.session = "s1";

    // learn_ask validation
    json norm;
    CHECK(learn_ask_check({{"kind", "predict"}, {"prompt", "What happens?"}}, &norm).empty());
    CHECK(norm["options"].is_array() && norm["options"].empty() && norm["want_confidence"] == false);
    CHECK(learn_ask_check({{"kind", "check"}, {"prompt", "p"}, {"options", {"a", "b"}}, {"want_confidence", true}}, &norm).empty());
    CHECK(norm["options"].size() == 2 && norm["want_confidence"] == true);
    CHECK(!learn_ask_check({{"kind", "quiz"}, {"prompt", "p"}}).empty());
    CHECK(!learn_ask_check({{"kind", "check"}}).empty());
    CHECK(!learn_ask_check({{"kind", "check"}, {"prompt", "  \n"}}).empty());
    CHECK(!learn_ask_check({{"kind", "check"}, {"prompt", std::string(4001, 'x')}}).empty());
    CHECK(!learn_ask_check({{"kind", "check"}, {"prompt", "p"}, {"options", {"only"}}}).empty());
    CHECK(!learn_ask_check({{"kind", "check"}, {"prompt", "p"}, {"options", {"a", ""}}}).empty());
    CHECK(!learn_ask_check({{"kind", "check"}, {"prompt", "p"}, {"options", "a,b"}}).empty());
    CHECK(!learn_ask_check({{"kind", "check"}, {"prompt", "p"}, {"options", {"1", "2", "3", "4", "5", "6", "7", "8", "9"}}}).empty());
    CHECK(!learn_ask_check({{"kind", "check"}, {"prompt", "p"}, {"want_thinking", "yes"}}).empty());
    CHECK(reg.call("learn_ask", {{"kind", "probe"}, {"prompt", "Why?"}}, ctx).ok);
    CHECK(!reg.call("learn_ask", {{"kind", "probe"}}, ctx).ok);
    CHECK(learn_ends_turn("learn_ask") && !learn_ends_turn("learn_note") && !learn_ends_turn("done"));
    CHECK(learn_tool_allowed("learn_ask"));

    // slugs
    CHECK(learn_slug("Hash Tables 101!") == "hash-tables-101");
    CHECK(learn_slug("  --  ") == "" && learn_slug("a__b") == "a-b");
    CHECK(learn_slug(std::string(100, 'a')).size() == 48);

    // learn_plan: validation
    auto plan = [&](json a) { return reg.call("learn_plan", a, ctx); };
    json steps = {{{"id", "s1"}, {"title", "Hash function"}},
                  {{"id", "s2"}, {"title", "Collisions"}, {"depends_on", json::array({"s1"})}},
                  {{"id", "s3"}, {"title", "Load factor"}, {"depends_on", {"s1", "s2"}}, {"status", "now"}}};
    CHECK(!plan({{"steps", steps}}).ok);                                   // no topic
    CHECK(!plan({{"topic", "hashing"}}).ok);                               // nothing to do
    CHECK(!plan({{"topic", "hashing"}, {"phase", "study"}}).ok);
    CHECK(!plan({{"topic", "hashing"}, {"steps", json::array()}}).ok);
    CHECK(!plan({{"topic", "hashing"}, {"steps", {{{"id", "a"}, {"title", "x"}, {"depends_on", json::array({"zz"})}}}}}).ok);
    CHECK(!plan({{"topic", "hashing"}, {"steps", {{{"id", "a"}, {"title", "x"}}, {{"id", "a"}, {"title", "y"}}}}}).ok);
    CHECK(!plan({{"topic", "hashing"}, {"steps", {{{"id", "a"}, {"title", "x"}, {"depends_on", json::array({"a"})}}}}}).ok);
    CHECK(plan({{"topic", "hashing"}, {"steps", {{{"id", "a"}, {"title", "x"}, {"depends_on", json::array({"b"})}},
                                                  {{"id", "b"}, {"title", "y"}, {"depends_on", json::array({"a"})}}}}}).output.find("loop") != std::string::npos);
    CHECK(!plan({{"topic", "hashing"}, {"steps", {{{"id", "a b"}, {"title", "x"}}}}}).ok);
    CHECK(!plan({{"topic", "hashing"}, {"steps", {{{"id", "a"}, {"title", ""}}}}}).ok);
    CHECK(!plan({{"topic", "hashing"}, {"steps", {{{"id", "a"}, {"title", "x"}, {"status", "maybe"}}}}}).ok);
    CHECK(!fs::exists(learn_topics_path(cfg)));                            // nothing saved by failures

    // learn_plan: round trip through the file
    auto r1 = plan({{"topic", "Hash tables"}, {"phase", "plan"}, {"steps", steps}});
    CHECK(r1.ok && r1.output.find("hash-tables") != std::string::npos && r1.output.find("[now] s3") != std::string::npos);
    CHECK(fs::exists(learn_topics_path(cfg)));
    CHECK(learn_topics_path(cfg).rfind(cfg.data_dir, 0) == 0);
    json doc = learn_topics_load(cfg);
    const json & t = doc["topics"]["hash-tables"];
    CHECK(t["name"] == "Hash tables" && t["phase"] == "plan" && t["steps"].size() == 3 && t["session"] == "s1");
    CHECK(t["steps"][2]["depends_on"].size() == 2 && t["steps"][0]["status"] == "todo");
    CHECK(plan({{"topic", "hash tables"}, {"update", {{{"id", "s1"}, {"status", "done"}}, {{"id", "s3"}, {"status", "repair"}}}}}).ok);
    CHECK(!plan({{"topic", "hash tables"}, {"update", {{{"id", "nope"}, {"status", "done"}}}}}).ok);
    CHECK(!plan({{"topic", "hash tables"}, {"update", {{{"id", "s1"}, {"status", "finished"}}}}}).ok);
    CHECK(plan({{"topic", "hash tables"}, {"phase", "teach"}}).ok);
    doc = learn_topics_load(cfg);
    CHECK(doc["topics"]["hash-tables"]["phase"] == "teach");
    CHECK(doc["topics"]["hash-tables"]["steps"][0]["status"] == "done" && doc["topics"]["hash-tables"]["steps"][2]["status"] == "repair");
    CHECK(doc["topics"].size() == 1);
    // an update to a topic without steps is refused and does not create it
    CHECK(!plan({{"topic", "other"}, {"update", {{{"id", "s1"}, {"status", "done"}}}}}).ok);
    CHECK(learn_topics_load(cfg)["topics"].size() == 1);

    // learn_note
    auto note = [&](json a) { return reg.call("learn_note", a, ctx); };
    CHECK(!note({{"topic", "hash tables"}}).ok);
    CHECK(!note({{"topic", "hash tables"}, {"notes", json::array()}}).ok);
    CHECK(!note({{"topic", "hash tables"}, {"notes", {{{"text", "a"}}, {{"text", "b"}}, {{"text", "c"}}, {{"text", "d"}}}}}).ok);
    CHECK(!note({{"topic", "hash tables"}, {"notes", {{{"text", ""}}}}}).ok);
    CHECK(!note({{"topic", "hash tables"}, {"notes", {{{"text", std::string(301, 'x')}}}}}).ok);
    CHECK(!note({{"topic", "hash tables"}, {"notes", {{{"text", "x"}, {"premises", json::array({"1", "2", "3", "4", "5", "6"})}}}}}).ok);
    CHECK(!note({{"topic", "hash tables"}, {"notes", json::array({"x"})}}).ok);
    auto n1 = note({{"topic", "hash tables"}, {"notes", {{{"text", "A hash maps a key to a slot"}, {"premises", json::array({"keys are arbitrary"})}},
                                                          {{"text", "Two keys can share a slot"}, {"premises", json::array({"p1", "the slot count is finite"})}}}}});
    CHECK(n1.ok && n1.output.find("p1: A hash maps") != std::string::npos && n1.output.find("p2:") != std::string::npos);
    CHECK(note({{"topic", "hash tables"}, {"notes", {{{"text", "a hash MAPS a key to a slot"}}}}}).ok);   // same claim: merged
    doc = learn_topics_load(cfg);
    CHECK(doc["topics"]["hash-tables"]["props"].size() == 2);
    CHECK(doc["topics"]["hash-tables"]["props"][0]["premises"].empty());   // refreshed by the repeat
    CHECK(note({{"topic", "Fourier"}, {"notes", {{{"text", "Any signal is a sum of sines"}}}}}).ok);   // new topic from a note
    for (int i = 0; i < 70; i++) CHECK(note({{"topic", "big"}, {"notes", {{{"text", "claim number " + std::to_string(i)}}}}}).ok);
    doc = learn_topics_load(cfg);
    CHECK(doc["topics"]["big"]["props"].size() == 60 && doc["topics"]["big"]["props"][0]["text"] == "claim number 10");

    // listing, single topic, digest
    json list = learn_topics_list(doc);
    CHECK(list.size() == 3);
    bool found = false;
    for (auto & row : list)
        if (row["id"] == "hash-tables") {
            found = true;
            CHECK(row["steps_total"] == 3 && row["steps_done"] == 1 && row["props"] == 2 && row["phase"] == "teach");
        }
    CHECK(found);
    CHECK(learn_topic_get(doc, "hash-tables")["steps"].size() == 3 && learn_topic_get(doc, "hash-tables")["id"] == "hash-tables");
    CHECK(learn_topic_get(doc, "zzz").is_null());
    std::string dg = learn_knowledge_digest(doc, 3000);
    CHECK(dg.find("Hash tables") != std::string::npos && dg.find("Two keys can share a slot") != std::string::npos);
    CHECK(dg.find("because: A hash maps a key to a slot;") != std::string::npos);   // an id premise reads as its text
    CHECK(learn_knowledge_digest(doc, 400).size() <= 400);
    CHECK(learn_knowledge_digest(json::object(), 400).empty());
    CHECK(dg.find("\xe2\x80\x94") == std::string::npos);

    // premises: normalized matching and explicit ids
    {
        CHECK(learn_norm("  Chaining,  STORES colliding-keys! ") == "chaining stores colliding keys");
        CHECK(learn_norm("...") == "" && learn_norm("") == "");
        auto n = [&](json a) { return reg.call("learn_note", a, ctx); };
        auto prm = [&](const char * topic) { return learn_topics_load(cfg)["topics"][topic]["props"]; };
        CHECK(n({{"topic", "links"}, {"notes", {{{"text", "Chaining stores colliding keys in a list"}}}}}).ok);
        // words differing in case, punctuation and spacing resolve to p1; a short id works as "P1" and "#p1"
        CHECK(n({{"topic", "links"}, {"notes", {{{"text", "Lookup walks one list"},
              {"premises", json::array({"chaining  STORES colliding keys, in a list!", "P1", "#p1", "lists are short"})}}}}}).ok);
        json pr = prm("links");
        CHECK(pr.size() == 2 && pr[1]["premises"].size() == 2 && pr[1]["premises"][0] == "p1" && pr[1]["premises"][1] == "lists are short");
        CHECK(learn_premise_ref(pr, "LOOKUP walks one list.") == "p2" && learn_premise_ref(pr, "p2") == "p2");
        CHECK(learn_premise_ref(pr, "p9").empty() && learn_premise_ref(pr, "something else").empty());
        // an unknown id is refused and nothing from that call is kept
        auto bad = n({{"topic", "links"}, {"notes", {{{"text", "Fresh claim"}, {"premises", json::array({"p9"})}}}}});
        CHECK(!bad.ok && bad.output.find("p9") != std::string::npos && prm("links").size() == 2);
        // a premise may name a proposition added earlier in the same call
        CHECK(n({{"topic", "links"}, {"notes", {{{"text", "Load factor is keys over slots"}},
              {{"text", "Resize when it grows"}, {"premises", json::array({"p3"})}}}}}).ok);
        CHECK(prm("links")[3]["premises"][0] == "p3");
        // a claim cannot rest on itself; restating it (normalized) merges
        CHECK(n({{"topic", "links"}, {"notes", {{{"text", "load factor is keys OVER slots."}, {"premises", json::array({"p3", "p1"})}}}}}).ok);
        pr = prm("links");
        CHECK(pr.size() == 4 && pr[2]["premises"].size() == 1 && pr[2]["premises"][0] == "p1");
        // text with no ASCII letters or digits is never merged with other such text; a new claim cannot cite its own words
        CHECK(n({{"topic", "jp"}, {"notes", {{{"text", "\xe3\x81\x82"}}, {{"text", "\xe3\x81\x84"}}}}}).ok && prm("jp").size() == 2);
        CHECK(n({{"topic", "jp"}, {"notes", {{{"text", "New claim here"}, {"premises", json::array({"new claim HERE!", "other"})}}}}}).ok);
        CHECK(prm("jp")[2]["premises"].size() == 1 && prm("jp")[2]["premises"][0] == "other");
    }

    // learn_progress is retired and old data is folded into the new store once
    {
        CHECK(!learn_tool_allowed("learn_progress"));
        CHECK(!reg.call("learn_progress", {{"action", "show"}}, ctx).ok);
        json old = {{"topics", {{"Hashing", {{"updated", "2026-10-01"}, {"goal", "see why probing works"},
                      {"held", {"arrays"}}, {"gaps", {"load factor"}},
                      {"plan", {{{"step", "hash function"}, {"done", true}, {"note", "ok"}}, {{"step", "collisions"}, {"done", false}},
                                {{"step", "load factor"}, {"done", false}}}},
                      {"practice", {{{"date", "2026-10-02"}, {"problem", "insert 5 keys"}, {"result", "correct"}}}}}}}}};
        std::string ppath = learn_progress_path(cfg);
        fs::create_directories(fs::path(ppath).parent_path());
        { std::ofstream(ppath) << old.dump(); }
        fs::remove(learn_topics_path(cfg));
        json d = learn_topics_load_migrated(cfg);
        const json & t = d["topics"]["hashing"];
        CHECK(t["name"] == "Hashing" && t["steps"].size() == 3);
        CHECK(t["steps"][0]["status"] == "done" && t["steps"][1]["status"] == "now" && t["steps"][2]["status"] == "todo");
        CHECK(t["steps"][1]["depends_on"][0] == "s1");
        CHECK(t["props"].size() == 4 && t["props"][0]["text"] == "Goal: see why probing works");
        CHECK(t["props"][3]["text"].get<std::string>().find("insert 5 keys -> correct") != std::string::npos);
        CHECK(fs::exists(learn_topics_path(cfg)) && fs::exists(ppath));
        // it works through the tools, runs once, and never overwrites a topic that has been changed since
        CHECK(reg.call("learn_note", {{"topic", "hashing"}, {"notes", json::array({json{{"text", "Probing finds the next free slot"}}})}}, ctx).ok);
        { std::ofstream(ppath) << old.dump(); }
        d = learn_topics_load_migrated(cfg);
        CHECK(d["topics"]["hashing"]["props"].size() == 5 && d["progress_migrated"] == true);
        // existing topic with the same name is kept untouched by a fresh migration
        json fresh = {{"topics", {{"hashing", {{"name", "Mine"}, {"steps", json::array()}, {"props", json::array()}}}}}};
        CHECK(learn_progress_migrate(fresh, old) && fresh["topics"]["hashing"]["name"] == "Mine" && !learn_progress_migrate(fresh, old));
        // missing or damaged progress.json is fine
        fs::remove(learn_topics_path(cfg));
        { std::ofstream(ppath) << "{oops"; }
        CHECK(learn_topics_load_migrated(cfg)["topics"].empty());
        fs::remove(ppath);
        fs::remove(learn_topics_path(cfg));
        CHECK(learn_topics_load_migrated(cfg)["topics"].empty());
    }

    // damaged file reads as empty
    { std::ofstream(learn_topics_path(cfg)) << "{not json"; }
    CHECK(learn_topics_load(cfg)["topics"].empty());
    CHECK(learn_tool_allowed("learn_plan") && learn_tool_allowed("learn_note"));

    // the visual tutor stays off unless [learn] show_visual is set
    {
        Config v;
        v.learn.visual_url = "http://localhost:8024";
        CHECK(!v.learn.show_visual && visual_proxy_port(v.learn, 8079) == 0);
        v.learn.show_visual = true;
        CHECK(visual_proxy_port(v.learn, 8079) == 8124);
    }
    // prompt: names every tool, no dashes of the forbidden kind, no vendor names
    {
        std::string p = learn_prompt();
        for (const char * w : {"learn_ask", "learn_plan", "learn_note", "measure", "practice"}) CHECK(p.find(w) != std::string::npos);
        CHECK(p.find("\xe2\x80\x94") == std::string::npos && p.find("\xe2\x80\x93") == std::string::npos);
        CHECK(p.find("OpenAI") == std::string::npos);
    }

    fs::remove_all(root);
    TEST_MAIN_END();
}
