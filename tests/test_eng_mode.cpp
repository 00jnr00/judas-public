// Engineering mode: the prompt, the CAD backend interface (stub and a fake kernel), the cad_* tools and their gating.
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "agent/agent.h"
#include "cad/cad_backend.h"
#include "cad/cad_tools.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

static bool has(const std::string & h, const std::string & n) { return h.find(n) != std::string::npos; }

// A fake kernel that records calls.
struct FakeCad : CadBackend {
    std::vector<std::string> calls;
    std::string name() const override { return "fake"; }
    bool available(std::string &) const override { return true; }
    CadResult state() override { calls.push_back("state"); return {true, "{\"parts\":[]}"}; }
    CadResult new_part(const std::string & n) override { calls.push_back("new_part " + n); return {true, "ok"}; }
    CadResult sketch(const json & s) override { calls.push_back("sketch " + s.dump()); return {true, "profile"}; }
    CadResult feature(const json & s) override { calls.push_back("feature " + s.dump()); return {true, "f1"}; }
    CadResult set_param(const std::string & n, const std::string & e) override { calls.push_back("param " + n + "=" + e); return {true, "ok"}; }
    CadResult measure(const json & r) override { calls.push_back("measure " + r.dump()); return {true, "volume 1000"}; }
    CadResult export_to(const std::string & f, const std::string & p) override {
        calls.push_back("export " + f);
        std::ofstream(p) << "solid x\nendsolid x\n";
        return {true, "wrote " + p};
    }
    CadResult undo() override { calls.push_back("undo"); return {true, "ok"}; }
};

int main() {
    fs::path root = fs::path(JUDAS_TEST_TMP) / ("eng-mode-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root / "project");

    // ---- prompt ----
    {
        std::string p = engineering_prompt();
        CHECK(has(p, "Never claim a test"));
        CHECK(has(p, "cad_state") && has(p, "artifacts/") && has(p, "artifact_render"));
        CHECK(has(p, "no CAD kernel"));
        CHECK(!has(p, "moldar") && !has(p, "MOLDAR") && !has(p, "render_part"));
        CHECK(!has(p, "\xe2\x80\x94"));   // no em dash
    }

    // ---- the stub backend: unavailable, no tools registered, every call a clear error ----
    {
        auto be = make_stub_cad_backend();
        std::string why;
        CHECK(!be->available(why) && has(why, "no CAD kernel compiled in"));
        auto r = be->feature({{"type", "extrude"}});
        CHECK(!r.ok && has(r.text, "CAD is unavailable") && has(r.text, "no model was built"));
        CHECK(!be->state().ok && !be->export_to("stl", "/x").ok && !be->undo().ok);
        ToolRegistry reg;
        std::string w;
        CHECK(!register_cad_tools(reg, be, &w) && has(w, "no CAD kernel"));
        CHECK(reg.list().empty());
        CHECK(!register_cad_tools(reg, nullptr, &w));
        CHECK(make_stub_cad_backend("custom reason")->available(w) == false && w == "custom reason");
    }

#ifdef JUDAS_WITH_CAD
    {   // JUDAS_CAD=ON: the binary's own backend is the OpenCascade kernel (tests/test_cad_occt.cpp covers it)
        std::string why;
        CHECK(make_cad_backend()->available(why));
    }
#else
    {
        std::string why;
        CHECK(!make_cad_backend()->available(why));
    }
#endif

    // ---- with a (fake) kernel the tools register and map onto the interface ----
    {
        auto fake = std::make_shared<FakeCad>();
        ToolRegistry reg;
        CHECK(register_cad_tools(reg, fake));
        for (const char * n : {"cad_state", "cad_new_part", "cad_sketch", "cad_feature", "cad_param", "cad_measure", "cad_export", "cad_undo"})
            CHECK(reg.find(n) != nullptr);
        Config cfg;
        cfg.permissions.mode = "full-auto";
        cfg.permissions.allow_paths = {(root / "project").string()};
        ToolContext ctx;
        ctx.cwd = (root / "project").string();
        ctx.cfg = &cfg;
        CHECK(reg.call("cad_state", json::object(), ctx).ok);
        CHECK(reg.call("cad_new_part", {{"name", "bracket"}}, ctx).ok);
        CHECK(reg.call("cad_sketch", {{"spec", {{"plane", "XY"}}}}, ctx).ok);
        CHECK(reg.call("cad_feature", {{"spec", {{"type", "extrude"}, {"distance", 8}}}}, ctx).ok);
        CHECK(reg.call("cad_param", {{"name", "wall"}, {"expr", "2.4"}}, ctx).ok);
        auto m = reg.call("cad_measure", json::object(), ctx);
        CHECK(m.ok && has(m.output, "volume 1000"));
        CHECK(reg.call("cad_undo", json::object(), ctx).ok);
        CHECK(fake->calls.size() == 7 && fake->calls[3].find("extrude") != std::string::npos && fake->calls[4] == "param wall=2.4");
        // export: new file inside the project works; overwrite, bad format and outside paths are refused before the backend
        auto e = reg.call("cad_export", {{"fmt", "stl"}, {"path", "artifacts/part.stl"}}, ctx);
        CHECK(e.ok && fs::exists(root / "project" / "artifacts" / "part.stl"));
        size_t n = fake->calls.size();
        CHECK(!reg.call("cad_export", {{"fmt", "stl"}, {"path", "artifacts/part.stl"}}, ctx).ok);
        CHECK(!reg.call("cad_export", {{"fmt", "obj"}, {"path", "artifacts/p.obj"}}, ctx).ok);
        CHECK(!reg.call("cad_export", {{"fmt", "stl"}, {"path", (root / "elsewhere.stl").string()}}, ctx).ok);
        CHECK(!fs::exists(root / "elsewhere.stl"));
        CHECK(fake->calls.size() == n);
        // missing required argument is an error the model can read, not a crash
        CHECK(!reg.call("cad_new_part", json::object(), ctx).ok);
    }

    {   // each session keeps its own live model
        auto a1 = cad_backend_for("s-a"), a2 = cad_backend_for("s-a"), b = cad_backend_for("s-b");
        CHECK(a1 == a2 && a1 != b && a1 != shared_cad_backend());
        CHECK(cad_backend_for("") == shared_cad_backend());
    }

    fs::remove_all(root);
    TEST_MAIN_END();
}
