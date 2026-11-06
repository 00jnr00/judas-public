// The OpenCascade CAD backend: parts, sketches, features, parameters, undo, measure, export, mesh and error text.
#include <unistd.h>

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

#include <BRepGProp.hxx>
#include <GProp_GProps.hxx>
#include <STEPControl_Reader.hxx>
#include <TopoDS_Shape.hxx>

#include "cad/cad_backend.h"
#include "cad/cad_tools.h"
#include "test.h"

namespace fs = std::filesystem;
using namespace judas;

static bool has(const std::string & h, const std::string & n) { return h.find(n) != std::string::npos; }
static bool near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

static json J(const CadResult & r) { return json::parse(r.text, nullptr, false); }

// total volume of the part via cad_measure
static double total_volume(CadBackend & be) {
    auto r = be.measure(json::array());
    if (!r.ok) return -1;
    return J(r)[0]["total_volume_mm3"].get<double>();
}

static double step_volume(const std::string & path) {
    STEPControl_Reader rd;
    if (rd.ReadFile(path.c_str()) != IFSelect_RetDone) return -1;
    rd.TransferRoots();
    TopoDS_Shape s = rd.OneShape();
    GProp_GProps p;
    BRepGProp::VolumeProperties(s, p);
    return p.Mass();
}

int main() {
    fs::path root = fs::path(JUDAS_TEST_TMP) / ("cad-occt-" + std::to_string(getpid()));
    fs::remove_all(root);
    fs::create_directories(root);
    const double kPi = 3.14159265358979323846;

    // ---- availability ----
    {
        auto be = make_cad_backend();
        std::string why;
        CHECK(be->available(why));
        CHECK(be->name() == "opencascade");
        ToolRegistry reg;
        CHECK(register_cad_tools(reg, be));
        CHECK(reg.find("cad_feature") != nullptr);
    }

    // ---- box minus cylinder ----
    {
        auto be = make_cad_backend();
        CHECK(be->new_part("block").ok);
        CHECK(!be->new_part("block").ok);   // never overwrites
        auto r = be->feature({{"type", "box"}, {"size", {20, 20, 10}}});
        CHECK(r.ok);
        CHECK(near(total_volume(*be), 4000, 1e-6));
        r = be->feature({{"type", "cylinder"}, {"radius", 3}, {"height", 10}, {"origin", {10, 10, 0}}, {"mode", "cut"}});
        CHECK(r.ok);
        CHECK(near(total_volume(*be), 4000 - kPi * 9 * 10, 1e-3));
        // state lists faces with types and a cylinder face
        auto st = J(be->state());
        CHECK(st["parts"][0]["bodies"][0]["face_count"].get<int>() == 7);
        bool cyl = false;
        for (auto & f : st["parts"][0]["bodies"][0]["faces"]) cyl |= f["type"] == "cylinder" && std::fabs(f["radius"].get<double>() - 3) < 1e-6;
        CHECK(cyl);
    }

    // ---- sketch + extrude, nested hole, params, undo ----
    {
        auto be = make_cad_backend();
        CHECK(be->set_param("w", "40").ok);
        CHECK(be->set_param("t", "w/10").ok);
        auto sk = be->sketch({{"plane", "XY"},
                              {"entities", json::array({{{"type", "rect"}, {"corner", {0, 0}}, {"size", {"w", 20}}},
                                                        {{"type", "circle"}, {"center", {20, 10}}, {"diameter", 6}}})}});
        CHECK(sk.ok);
        auto sj = J(sk);
        CHECK(sj["profiles"].size() == 1 && sj["profiles"][0]["holes"].get<int>() == 1);
        CHECK(near(sj["profiles"][0]["area"].get<double>(), 40 * 20 - kPi * 9, 1e-3));
        auto ex = be->feature({{"type", "extrude"}, {"profile", "s1.p1"}, {"distance", "t"}});
        CHECK(ex.ok);
        double v0 = 40 * 20 * 4 - kPi * 9 * 4;
        CHECK(near(total_volume(*be), v0, 1e-3));
        // param change rebuilds
        CHECK(be->set_param("t", "8").ok);
        CHECK(near(total_volume(*be), 40 * 20 * 8 - kPi * 9 * 8, 1e-3));
        CHECK(be->set_param("w", "50").ok);
        CHECK(near(total_volume(*be), 50 * 20 * 8 - kPi * 9 * 8, 1e-3));
        // undo returns to w = 40
        CHECK(be->undo().ok);
        CHECK(near(total_volume(*be), 40 * 20 * 8 - kPi * 9 * 8, 1e-3));
        // one more undo takes t back to w/10 = 4; a second extrusion as a new body (symmetric) gives two bodies
        CHECK(be->undo().ok);
        CHECK(near(total_volume(*be), 40 * 20 * 4 - kPi * 9 * 4, 1e-3));
        CHECK(be->feature({{"type", "extrude"}, {"profile", "s1.p1"}, {"distance", 4}, {"direction", "symmetric"}, {"mode", "new"}}).ok);
        auto st = J(be->state());
        CHECK(st["parts"][0]["bodies"].size() == 2);
    }

    // ---- faces named by direction stay right after features (seen live: the second hole went through a side) ----
    {
        auto be = make_cad_backend();
        CHECK(be->feature({{"type", "box"}, {"size", {80, 30, 10}}}).ok);
        CHECK(be->feature({{"type", "hole"}, {"face", "b1.top"}, {"diameter", 6.5}, {"at", {-10, 8}}}).ok);
        CHECK(be->feature({{"type", "hole"}, {"face", "b1.top"}, {"diameter", 6.5}, {"at", {-10, -8}}}).ok);
        CHECK(near(total_volume(*be), 80.0 * 30 * 10 - 2 * kPi * 3.25 * 3.25 * 10, 1e-3));
        CHECK(be->feature({{"type", "hole"}, {"face", "-X"}, {"diameter", 4}, {"depth", 5}}).ok);   // no body prefix
        CHECK(!be->feature({{"type", "hole"}, {"face", "b1.sideways"}, {"diameter", 4}}).ok);
        double before = total_volume(*be);
        auto off = be->feature({{"type", "hole"}, {"face", "b1.top"}, {"diameter", 6.5}, {"at", {-50, 8}}});   // past the end
        CHECK(!off.ok && off.text.find("removed no material") != std::string::npos && total_volume(*be) == before);
        auto sk = be->sketch({{"plane", "b1.top"}, {"entities", json::array({json{{"type", "circle"}, {"center", {20, 0}}, {"radius", 2}}})}});
        CHECK(sk.ok);
    }

    // ---- fillet decreases volume; chamfer too; hole on a face; shell ----
    {
        auto be = make_cad_backend();
        CHECK(be->feature({{"type", "box"}, {"size", {30, 20, 10}}}).ok);
        double v0 = total_volume(*be);
        // find the top face (normal +z) and a vertical edge
        auto st = J(be->state());
        std::string top, vedge;
        for (auto & f : st["parts"][0]["bodies"][0]["faces"])
            if (f["type"] == "plane" && f["normal"][2].get<double>() > 0.9) top = f["id"];
        CHECK(!top.empty());
        for (auto & e : st["parts"][0]["bodies"][0]["edges"])
            if (e["type"] == "line" && std::fabs(e["length"].get<double>() - 10) < 1e-6) { vedge = e["id"]; break; }
        CHECK(!vedge.empty());
        auto fr = be->feature({{"type", "fillet"}, {"edges", json::array({vedge})}, {"radius", 2}});
        CHECK(fr.ok);
        double v1 = total_volume(*be);
        CHECK(v1 < v0 - 1);
        CHECK(near(v0 - v1, (4 - kPi) * 10, 1e-3));   // 1 - pi/4 of r^2 per mm times length
        auto ch = be->feature({{"type", "chamfer"}, {"edges", json::array({json{{"face", top}}})}, {"distance", 0.5}});
        CHECK(ch.ok);
        CHECK(total_volume(*be) < v1 - 1);
        // hole in the top face (face indices may have changed: re-read)
        st = J(be->state());
        top.clear();
        for (auto & f : st["parts"][0]["bodies"][0]["faces"])
            if (f["type"] == "plane" && f["normal"][2].get<double>() > 0.9 && f["area"].get<double>() > 100) top = f["id"];
        double vb = total_volume(*be);
        auto h = be->feature({{"type", "hole"}, {"face", top}, {"diameter", 4}, {"depth", 5}});
        CHECK(h.ok);
        CHECK(near(vb - total_volume(*be), kPi * 4 * 5, 1e-2));
        auto sh = be->feature({{"type", "shell"}, {"remove", json::array({top})}, {"thickness", 1}});
        if (sh.ok) CHECK(total_volume(*be) < vb * 0.6);   // may be rejected on this filleted, holed body
    }

    // ---- shell: open-top box, 2 mm walls ----
    {
        auto be = make_cad_backend();
        CHECK(be->feature({{"type", "box"}, {"size", {30, 20, 10}}}).ok);
        auto st = J(be->state());
        std::string top;
        for (auto & f : st["parts"][0]["bodies"][0]["faces"])
            if (f["normal"][2].get<double>() > 0.9) top = f["id"];
        CHECK(be->feature({{"type", "shell"}, {"remove", json::array({top})}, {"thickness", 2}}).ok);
        CHECK(near(total_volume(*be), 6000 - 26.0 * 16 * 8, 1e-3));
    }

    // ---- revolve ----
    {
        auto be = make_cad_backend();
        // rectangle x 5..10, y 0..20 revolved about the sketch y axis: a pipe, V = pi (10^2 - 5^2) 20
        CHECK(be->sketch({{"plane", "XY"}, {"entities", json::array({{{"type", "rect"}, {"corner", {5, 0}}, {"size", {5, 20}}}})}}).ok);
        CHECK(be->feature({{"type", "revolve"}, {"sketch", "s1"}, {"axis", "sketch_y"}}).ok);
        CHECK(near(total_volume(*be), kPi * (100 - 25) * 20, 1e-2));
    }

    // ---- measure: distance, section, density, face/edge ----
    {
        auto be = make_cad_backend();
        CHECK(be->feature({{"type", "box"}, {"size", {10, 10, 10}}}).ok);
        auto m = be->measure(json::array({{{"between", json::array({json::array({20, 5, 5}), "b1"})}}}));
        CHECK(m.ok && near(J(m)[0]["min_distance_mm"].get<double>(), 10, 1e-6));
        m = be->measure(json::array({{{"section", {{"plane", "XY"}, {"offset", 5}}}}}));
        CHECK(m.ok && near(J(m)[0]["area_mm2"].get<double>(), 100, 1e-6));
        m = be->measure(json::array({{{"body", "b1"}, {"density", 7.85}}}));
        CHECK(m.ok && near(J(m)[0]["mass_g"].get<double>(), 7.85, 1e-4));
        m = be->measure(json::array({{{"face", "b1.f1"}}}));
        CHECK(m.ok && near(J(m)[0]["area"].get<double>(), 100, 1e-6));
    }

    // ---- export STL + STEP, re-import STEP ----
    {
        auto be = make_cad_backend();
        CHECK(be->feature({{"type", "box"}, {"size", {10, 20, 30}}}).ok);
        CHECK(be->feature({{"type", "cylinder"}, {"radius", 2}, {"height", 30}, {"origin", {5, 10, 0}}, {"mode", "cut"}}).ok);
        double v = total_volume(*be);
        std::string stl = (root / "p.stl").string(), step = (root / "p.step").string();
        auto e1 = be->export_opts("stl", stl, {{"tolerance", 0.01}});
        CHECK(e1.ok);
        auto e2 = be->export_to("step", step);
        CHECK(e2.ok);
        // binary STL: 80-byte header, u32 triangle count, 50 bytes per triangle
        std::ifstream in(stl, std::ios::binary);
        char hdr[84] = {};
        in.read(hdr, 84);
        uint32_t n = 0;
        std::memcpy(&n, hdr + 80, 4);
        CHECK(n > 12);
        CHECK(fs::file_size(stl) == 84 + 50ull * n);
        CHECK(near(step_volume(step), v, 1e-3));
        CHECK(!be->export_to("obj", (root / "x.obj").string()).ok);
    }

    // ---- mesh accessor ----
    {
        auto be = make_cad_backend();
        CadMesh m;
        std::string err;
        CHECK(!be->mesh(m, err) && !err.empty());
        CHECK(be->feature({{"type", "box"}, {"size", {10, 10, 10}}}).ok);
        CHECK(be->mesh(m, err, 0.1));
        CHECK(m.indices.size() == 36 && m.positions.size() == m.normals.size() && m.tri_face.size() == 12);
        // outward-facing: signed volume from the triangles is positive and equals 1000
        double vol = 0;
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
            const float * a = &m.positions[3 * m.indices[i]];
            const float * b = &m.positions[3 * m.indices[i + 1]];
            const float * c = &m.positions[3 * m.indices[i + 2]];
            vol += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
        }
        CHECK(near(vol, 1000, 1e-3));
    }

    // ---- bad input gives clear errors and leaves the model alone ----
    {
        auto be = make_cad_backend();
        CHECK(be->feature({{"type", "box"}, {"size", {10, 10, 10}}}).ok);
        double v = total_volume(*be);
        auto r = be->feature({{"type", "fillet"}, {"edges", json::array({"b1.e1"})}, {"radius", 50}});
        CHECK(!r.ok && has(r.text, "fillet") && has(r.text, "radius"));
        r = be->feature({{"type", "fillet"}, {"edges", json::array({"b1.e99"})}, {"radius", 1}});
        CHECK(!r.ok && has(r.text, "out of range"));
        r = be->feature({{"type", "fillet"}, {"edges", json::array({"zzz"})}, {"radius", 1}});
        CHECK(!r.ok && has(r.text, "bad reference"));
        r = be->feature({{"type", "wibble"}});
        CHECK(!r.ok && has(r.text, "unknown feature type"));
        r = be->feature({{"type", "extrude"}, {"distance", 5}});
        CHECK(!r.ok && has(r.text, "sketch"));
        r = be->sketch({{"plane", "ABC"}, {"entities", json::array({{{"type", "circle"}, {"center", {0, 0}}, {"radius", 1}}})}});
        CHECK(!r.ok && has(r.text, "unknown plane"));
        r = be->sketch({{"plane", "XY"}, {"entities", json::array({{{"type", "line"}, {"from", {0, 0}}, {"to", {5, 0}}}})}});
        CHECK(!r.ok && has(r.text, "no closed profile"));
        r = be->sketch({{"plane", "XY"}, {"entities", json::array({json{{"type", "blob"}}})}});
        CHECK(!r.ok && has(r.text, "unknown sketch entity"));
        CHECK(!be->set_param("a", "b + 1").ok);                 // unknown parameter
        CHECK(!be->set_param("1x", "3").ok);                    // bad name
        CHECK(be->set_param("a", "2").ok);
        auto cyc = be->set_param("a", "a + 1");                  // self reference
        CHECK(!cyc.ok && has(cyc.text, "circular"));
        CHECK(!be->set_param("d", "1/0").ok);
        CHECK(near(total_volume(*be), v, 1e-9));
        // a param change that breaks a later feature is rejected and leaves the old value
        CHECK(be->set_param("k", "3").ok);
        CHECK(be->feature({{"type", "cylinder"}, {"radius", "k"}, {"height", 10}, {"origin", {5, 5, 0}}, {"mode", "cut"}}).ok);
        auto bad = be->set_param("k", "0");
        CHECK(!bad.ok && has(bad.text, "f2"));
        auto st = J(be->state());
        CHECK(st["parts"][0]["parameters"][1]["expr"] == "3" || st["parts"][0]["parameters"][2]["expr"] == "3");
        // undo past the start
        for (int i = 0; i < 20; i++) be->undo();
        CHECK(!be->undo().ok);
    }

    fs::remove_all(root);
    TEST_MAIN_END();
}
