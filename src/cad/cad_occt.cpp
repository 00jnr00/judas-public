// OpenCascade CAD backend for Engineering mode (see cad_backend.h and docs/ENGINEERING-MODE.md).
// Compiled into the judas binary with static OCCT libraries; no subprocess, no files besides what the user exports.
//
// Model: a part is a list of timeline items (sketches and features) plus named parameters. Every change is applied
// to a copy, the whole timeline is rebuilt from scratch, and only a successful rebuild replaces the part (so a
// failed fillet never corrupts the model). Undo restores the previous copy. Topology references are 1-based
// indices from cad_state listings ("b1.f3" = face 3 of body b1); they are stable for a given rebuild and can shift
// when a parameter change alters the topology, so re-read cad_state after changing parameters.
#include "cad/cad_backend.h"

#include <Bnd_Box.hxx>
#include <BRep_Builder.hxx>
#include <BRep_Tool.hxx>
#include <BRepAdaptor_Curve.hxx>
#include <BRepAdaptor_Surface.hxx>
#include <BRepAlgoAPI_Common.hxx>
#include <BRepAlgoAPI_Cut.hxx>
#include <BRepAlgoAPI_Fuse.hxx>
#include <BRepAlgoAPI_Section.hxx>
#include <BRepBndLib.hxx>
#include <BRepBuilderAPI_MakeEdge.hxx>
#include <BRepBuilderAPI_MakeFace.hxx>
#include <BRepBuilderAPI_MakeVertex.hxx>
#include <BRepBuilderAPI_MakeWire.hxx>
#include <BRepBuilderAPI_Transform.hxx>
#include <BRepCheck_Analyzer.hxx>
#include <BRepClass_FaceClassifier.hxx>
#include <BRepExtrema_DistShapeShape.hxx>
#include <BRepFilletAPI_MakeChamfer.hxx>
#include <BRepFilletAPI_MakeFillet.hxx>
#include <BRepGProp.hxx>
#include <BRepMesh_IncrementalMesh.hxx>
#include <BRepOffsetAPI_MakeThickSolid.hxx>
#include <BRepPrimAPI_MakeBox.hxx>
#include <BRepPrimAPI_MakeCylinder.hxx>
#include <BRepPrimAPI_MakePrism.hxx>
#include <BRepPrimAPI_MakeRevol.hxx>
#include <BRepPrimAPI_MakeSphere.hxx>
#include <BRepTools.hxx>
#include <GC_MakeArcOfCircle.hxx>
#include <GProp_GProps.hxx>
#include <Geom_TrimmedCurve.hxx>
#include <IFSelect_ReturnStatus.hxx>
#include <Message.hxx>
#include <Message_Messenger.hxx>
#include <Message_Printer.hxx>
#include <Poly_Triangulation.hxx>
#include <STEPControl_Reader.hxx>
#include <STEPControl_Writer.hxx>
#include <ShapeAnalysis_FreeBounds.hxx>
#include <ShapeFix_Face.hxx>
#include <ShapeUpgrade_UnifySameDomain.hxx>
#include <StlAPI_Writer.hxx>
#include <Standard_Failure.hxx>
#include <TColgp_Array1OfPnt.hxx>
#include <TopExp.hxx>
#include <TopExp_Explorer.hxx>
#include <TopLoc_Location.hxx>
#include <TopTools_HSequenceOfShape.hxx>
#include <TopTools_IndexedMapOfShape.hxx>
#include <TopTools_ListOfShape.hxx>
#include <TopoDS.hxx>
#include <TopoDS_Compound.hxx>
#include <gp_Ax1.hxx>
#include <gp_Ax2.hxx>
#include <gp_Ax3.hxx>
#include <gp_Circ.hxx>
#include <gp_Dir.hxx>
#include <gp_Pln.hxx>
#include <gp_Pnt.hxx>
#include <gp_Trsf.hxx>
#include <gp_Vec.hxx>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <mutex>
#include <regex>
#include <stdexcept>

namespace judas {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

struct CadError : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void fail(const std::string & m) { throw CadError(m); }

double rnd(double v) {
    double r = std::round(v * 1e4) / 1e4;
    return r == 0 ? 0.0 : r;
}
json vec3(const gp_XYZ & p) { return json::array({rnd(p.X()), rnd(p.Y()), rnd(p.Z())}); }
json vec3(const gp_Pnt & p) { return vec3(p.XYZ()); }
json vec3(const gp_Dir & d) { return vec3(d.XYZ()); }

// ---- expressions: numbers, parameter names, + - * / ^ ( ), functions; trig works in degrees ----
using Params = std::vector<std::pair<std::string, std::string>>;

class Eval {
public:
    explicit Eval(const Params & p) : params_(p) {}
    double eval(const std::string & e, int depth = 0) {
        if (depth > 24) fail("circular or too deep parameter reference in '" + e + "'");
        depth_ = depth;
        s_ = e;
        i_ = 0;
        double v = sum();
        ws();
        if (i_ != s_.size()) fail("cannot parse expression '" + e + "' near '" + s_.substr(i_) + "'");
        if (!std::isfinite(v)) fail("expression '" + e + "' is not finite");
        return v;
    }

private:
    const Params & params_;
    std::string s_;
    size_t i_ = 0;
    int depth_ = 0;
    void ws() { while (i_ < s_.size() && std::isspace((unsigned char)s_[i_])) i_++; }
    bool eat(char c) { ws(); if (i_ < s_.size() && s_[i_] == c) { i_++; return true; } return false; }
    double sum() {
        double v = prod();
        for (;;) {
            if (eat('+')) v += prod();
            else if (eat('-')) v -= prod();
            else return v;
        }
    }
    double prod() {
        double v = unary();
        for (;;) {
            if (eat('*')) v *= unary();
            else if (eat('/')) { double d = unary(); if (d == 0) fail("division by zero in '" + s_ + "'"); v /= d; }
            else return v;
        }
    }
    double unary() {
        if (eat('-')) return -unary();
        if (eat('+')) return unary();
        double b = atom();
        if (eat('^')) return std::pow(b, unary());
        return b;
    }
    double atom() {
        ws();
        if (eat('(')) { double v = sum(); if (!eat(')')) fail("missing ')' in '" + s_ + "'"); return v; }
        if (i_ < s_.size() && (std::isdigit((unsigned char)s_[i_]) || s_[i_] == '.')) {
            size_t st = i_;
            while (i_ < s_.size() && (std::isdigit((unsigned char)s_[i_]) || s_[i_] == '.')) i_++;
            if (i_ < s_.size() && (s_[i_] == 'e' || s_[i_] == 'E') && i_ + 1 < s_.size() &&
                (std::isdigit((unsigned char)s_[i_ + 1]) || s_[i_ + 1] == '-' || s_[i_ + 1] == '+')) {
                i_ += 2;
                while (i_ < s_.size() && std::isdigit((unsigned char)s_[i_])) i_++;
            }
            try { return std::stod(s_.substr(st, i_ - st)); } catch (...) { fail("bad number in '" + s_ + "'"); }
        }
        if (i_ < s_.size() && (std::isalpha((unsigned char)s_[i_]) || s_[i_] == '_')) {
            size_t st = i_;
            while (i_ < s_.size() && (std::isalnum((unsigned char)s_[i_]) || s_[i_] == '_')) i_++;
            std::string id = s_.substr(st, i_ - st);
            if (eat('(')) {
                std::vector<double> a;
                if (!eat(')')) {
                    do a.push_back(sum()); while (eat(','));
                    if (!eat(')')) fail("missing ')' in '" + s_ + "'");
                }
                return call(id, a);
            }
            for (auto & p : params_)
                if (p.first == id) {
                    Eval sub(params_);
                    return sub.eval(p.second, depth_ + 1);
                }
            if (id == "pi") return kPi;
            fail("unknown parameter '" + id + "' (define it with cad_param)");
        }
        fail("cannot parse expression '" + s_ + "'");
    }
    static double call(const std::string & f, const std::vector<double> & a) {
        auto need = [&](size_t n) { if (a.size() != n) fail("function " + f + " takes " + std::to_string(n) + " argument(s)"); };
        if (f == "sin") { need(1); return std::sin(a[0] * kDeg); }
        if (f == "cos") { need(1); return std::cos(a[0] * kDeg); }
        if (f == "tan") { need(1); return std::tan(a[0] * kDeg); }
        if (f == "asin") { need(1); return std::asin(a[0]) / kDeg; }
        if (f == "acos") { need(1); return std::acos(a[0]) / kDeg; }
        if (f == "atan") { need(1); return std::atan(a[0]) / kDeg; }
        if (f == "sqrt") { need(1); if (a[0] < 0) fail("sqrt of a negative number"); return std::sqrt(a[0]); }
        if (f == "abs") { need(1); return std::fabs(a[0]); }
        if (f == "min") { need(2); return std::min(a[0], a[1]); }
        if (f == "max") { need(2); return std::max(a[0], a[1]); }
        if (f == "pow") { need(2); return std::pow(a[0], a[1]); }
        fail("unknown function '" + f + "'");
    }
};

// ---- shape helpers ----
double volume_of(const TopoDS_Shape & s) { GProp_GProps p; BRepGProp::VolumeProperties(s, p); return p.Mass(); }
double area_of(const TopoDS_Shape & s) { GProp_GProps p; BRepGProp::SurfaceProperties(s, p); return p.Mass(); }
double length_of(const TopoDS_Shape & s) { GProp_GProps p; BRepGProp::LinearProperties(s, p); return p.Mass(); }
gp_Pnt vol_center(const TopoDS_Shape & s) { GProp_GProps p; BRepGProp::VolumeProperties(s, p); return p.CentreOfMass(); }
gp_Pnt surf_center(const TopoDS_Shape & s) { GProp_GProps p; BRepGProp::SurfaceProperties(s, p); return p.CentreOfMass(); }
gp_Pnt lin_center(const TopoDS_Shape & s) { GProp_GProps p; BRepGProp::LinearProperties(s, p); return p.CentreOfMass(); }

json bbox_json(const TopoDS_Shape & s) {
    Bnd_Box b;
    BRepBndLib::AddOptimal(s, b, false, false);
    if (b.IsVoid()) return nullptr;
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    return {{"min", json::array({rnd(x0), rnd(y0), rnd(z0)})}, {"max", json::array({rnd(x1), rnd(y1), rnd(z1)})},
            {"size", json::array({rnd(x1 - x0), rnd(y1 - y0), rnd(z1 - z0)})}};
}

double bbox_diag(const TopoDS_Shape & s) {
    Bnd_Box b;
    BRepBndLib::Add(s, b);
    if (b.IsVoid()) return 100;
    double x0, y0, z0, x1, y1, z1;
    b.Get(x0, y0, z0, x1, y1, z1);
    return std::sqrt((x1 - x0) * (x1 - x0) + (y1 - y0) * (y1 - y0) + (z1 - z0) * (z1 - z0));
}

TopoDS_Shape sub_shape(const TopoDS_Shape & body, TopAbs_ShapeEnum kind, int idx) {
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(body, kind, m);
    if (idx < 1 || idx > m.Extent())
        fail(std::string(kind == TopAbs_FACE ? "face" : "edge") + " index " + std::to_string(idx) + " is out of range (the body has " +
             std::to_string(m.Extent()) + "); read cad_state again");
    return m(idx);
}

// A face named by direction: "top" or "+Z" is the planar face whose outward normal points that way and that sits
// farthest along it (ties: the larger face). Stable across features, unlike f-numbers. 0 when none faces that way.
gp_Dir plane_normal(const TopoDS_Face & f);
int face_by_direction(const TopoDS_Shape & body, const gp_Dir & d) {
    TopTools_IndexedMapOfShape m;
    TopExp::MapShapes(body, TopAbs_FACE, m);
    int best = 0;
    double best_pos = -1e300, best_area = -1;
    for (int i = 1; i <= m.Extent(); i++) {
        TopoDS_Face f = TopoDS::Face(m(i));
        BRepAdaptor_Surface a(f);
        if (a.GetType() != GeomAbs_Plane) continue;
        if (plane_normal(f).Dot(d) < 0.999) continue;
        gp_Pnt c = surf_center(f);
        double pos = c.X() * d.X() + c.Y() * d.Y() + c.Z() * d.Z(), area = area_of(f);
        if (pos > best_pos + 1e-6 || (std::fabs(pos - best_pos) <= 1e-6 && area > best_area)) { best = i; best_pos = pos; best_area = area; }
    }
    return best;
}

bool direction_name(std::string s, gp_Dir & d) {
    for (auto & c : s) c = (char)std::tolower((unsigned char)c);
    if (s == "top" || s == "+z" || s == "z") { d = gp_Dir(0, 0, 1); return true; }
    if (s == "bottom" || s == "-z") { d = gp_Dir(0, 0, -1); return true; }
    if (s == "right" || s == "+x" || s == "x") { d = gp_Dir(1, 0, 0); return true; }
    if (s == "left" || s == "-x") { d = gp_Dir(-1, 0, 0); return true; }
    if (s == "back" || s == "+y" || s == "y") { d = gp_Dir(0, 1, 0); return true; }
    if (s == "front" || s == "-y") { d = gp_Dir(0, -1, 0); return true; }
    return false;
}

TopoDS_Shape unify(const TopoDS_Shape & s) {
    ShapeUpgrade_UnifySameDomain u(s, true, true, true);
    u.Build();
    return u.Shape();
}

std::string face_type(const BRepAdaptor_Surface & a) {
    switch (a.GetType()) {
        case GeomAbs_Plane: return "plane";
        case GeomAbs_Cylinder: return "cylinder";
        case GeomAbs_Cone: return "cone";
        case GeomAbs_Sphere: return "sphere";
        case GeomAbs_Torus: return "torus";
        case GeomAbs_BezierSurface: return "bezier";
        case GeomAbs_BSplineSurface: return "bspline";
        default: return "other";
    }
}

// outward normal of a planar face
gp_Dir plane_normal(const TopoDS_Face & f) {
    BRepAdaptor_Surface a(f);
    gp_Dir n = a.Plane().Axis().Direction();
    if (f.Orientation() == TopAbs_REVERSED) n.Reverse();
    return n;
}

// frame on a planar face: origin at the face centre, z = outward normal
gp_Ax3 face_frame(const TopoDS_Face & f) {
    BRepAdaptor_Surface a(f);
    if (a.GetType() != GeomAbs_Plane) fail("that face is not planar (" + face_type(a) + "); pick a plane face");
    gp_Pln pl = a.Plane();
    gp_Dir n = plane_normal(f);
    gp_Pnt o = surf_center(f);
    gp_Dir xd = pl.Position().XDirection();
    return gp_Ax3(o, n, xd);
}

// ---- sketch machinery ----
struct SketchBuilt {
    gp_Ax3 ax;
    std::vector<TopoDS_Face> profiles;
    json info;
};

gp_Pnt to3(const gp_Ax3 & ax, double u, double v) {
    gp_Pnt o = ax.Location();
    gp_Dir x = ax.XDirection(), y = ax.YDirection();
    return gp_Pnt(o.X() + u * x.X() + v * y.X(), o.Y() + u * x.Y() + v * y.Y(), o.Z() + u * x.Z() + v * y.Z());
}
std::pair<double, double> to2(const gp_Ax3 & ax, const gp_Pnt & p) {
    gp_Vec d(ax.Location(), p);
    return {d.Dot(gp_Vec(ax.XDirection())), d.Dot(gp_Vec(ax.YDirection()))};
}

// Connect edges into wires, keep closed ones, nest them (even depth = outline, odd depth = hole) and make planar faces.
std::vector<TopoDS_Face> faces_from_edges(const std::vector<TopoDS_Edge> & edges, const gp_Ax3 & ax, int * open_wires) {
    Handle(TopTools_HSequenceOfShape) in = new TopTools_HSequenceOfShape();
    for (auto & e : edges) in->Append(e);
    Handle(TopTools_HSequenceOfShape) wires;
    ShapeAnalysis_FreeBounds::ConnectEdgesToWires(in, 1e-4, false, wires);
    std::vector<TopoDS_Wire> closed;
    int open = 0;
    for (int i = 1; i <= wires->Length(); i++) {
        TopoDS_Wire w = TopoDS::Wire(wires->Value(i));
        if (BRep_Tool::IsClosed(w)) closed.push_back(w); else open++;
    }
    if (open_wires) *open_wires = open;
    gp_Pln pln(ax);
    std::vector<TopoDS_Face> single;
    std::vector<double> areas;
    for (auto & w : closed) {
        BRepBuilderAPI_MakeFace mf(pln, w, true);
        if (!mf.IsDone()) continue;
        single.push_back(mf.Face());
        areas.push_back(area_of(mf.Face()));
    }
    size_t n = single.size();
    std::vector<size_t> order(n);
    for (size_t i = 0; i < n; i++) order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return areas[a] > areas[b]; });
    std::vector<int> parent(n, -1), depth(n, 0);
    for (size_t oi = 0; oi < n; oi++) {
        size_t j = order[oi];
        BRepAdaptor_Curve c;
        TopExp_Explorer ex(closed[j], TopAbs_EDGE);
        c.Initialize(TopoDS::Edge(ex.Current()));
        gp_Pnt mid = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
        for (size_t oi2 = oi; oi2-- > 0;) {   // smallest larger face first
            size_t i = order[oi2];
            BRepClass_FaceClassifier cl;
            cl.Perform(single[i], mid, 1e-6);
            if (cl.State() == TopAbs_IN) { parent[j] = (int)i; break; }
        }
    }
    for (size_t oi = 0; oi < n; oi++) { size_t j = order[oi]; depth[j] = parent[j] < 0 ? 0 : depth[parent[j]] + 1; }
    std::vector<TopoDS_Face> out;
    for (size_t oi = 0; oi < n; oi++) {
        size_t j = order[oi];
        if (depth[j] % 2) continue;
        BRepBuilderAPI_MakeFace mf(pln, closed[j], true);
        for (size_t oi2 = oi + 1; oi2 < n; oi2++) {
            size_t k = order[oi2];
            if (parent[k] == (int)j && depth[k] % 2) {
                TopoDS_Wire h = closed[k];
                h.Reverse();
                mf.Add(h);
            }
        }
        ShapeFix_Face fx(mf.Face());
        fx.Perform();
        TopoDS_Face f = TopoDS::Face(fx.Face());
        // make the face normal agree with the sketch normal
        BRepAdaptor_Surface sa(f);
        if (sa.GetType() == GeomAbs_Plane && plane_normal(f).Dot(ax.Direction()) < 0) f.Reverse();
        out.push_back(f);
    }
    return out;
}

// ---- the part ----
struct Body { std::string id; TopoDS_Shape shape; };
struct Built {
    std::vector<Body> bodies;
    std::map<std::string, SketchBuilt> sketches;
    std::map<std::string, json> info;   // timeline id -> result summary
    std::string last_id;
    int body_counter = 0;
};
struct PartData {
    std::string name;
    Params params;
    json items = json::array();
    int n_sketch = 0, n_feat = 0;
};

class Rebuilder {
public:
    Rebuilder(const PartData & d) : d_(d), ev_(d.params) {}
    Built run() {
        for (auto & it : d_.items) {
            std::string id = it.value("id", "?");
            try {
                if (it.value("kind", "") == "sketch") sketch(it);
                else feature(it);
            } catch (const CadError & e) {
                fail("timeline item " + id + " (" + it.value("type", it.value("kind", "?")) + "): " + e.what());
            } catch (const Standard_Failure & e) {
                fail("timeline item " + id + " (" + it.value("type", it.value("kind", "?")) + "): kernel error: " + std::string(e.GetMessageString() ? e.GetMessageString() : e.DynamicType()->Name()));
            }
            b_.last_id = id;
        }
        return std::move(b_);
    }

private:
    const PartData & d_;
    Eval ev_;
    Built b_;

    double N(const json & v, const char * what = "value") {
        if (v.is_number()) return v.get<double>();
        if (v.is_string()) return ev_.eval(v.get<std::string>());
        fail(std::string(what) + " must be a number or an expression string");
    }
    double NK(const json & o, const char * key, bool required = true, double def = 0) {
        if (!o.contains(key) || o[key].is_null()) {
            if (required) fail(std::string("missing '") + key + "'");
            return def;
        }
        return N(o[key], key);
    }
    std::vector<double> NA(const json & v, size_t n, const char * what) {
        if (!v.is_array() || v.size() != n) fail(std::string(what) + " must be an array of " + std::to_string(n) + " numbers");
        std::vector<double> r;
        for (auto & x : v) r.push_back(N(x, what));
        return r;
    }

    // ---- references ----
    struct Ref { std::string body; char kind = 0; int idx = 0; bool by_dir = false; gp_Dir dir; std::string dir_name; };
    Ref parse_ref(const json & v, char default_kind = 0) {
        Ref r;
        if (v.is_object()) {
            r.body = v.value("body", "");
            std::string k = v.value("kind", "");
            r.kind = k == "face" ? 'f' : k == "edge" ? 'e' : default_kind;
            if (v.contains("index")) r.idx = (int)N(v["index"], "index");
            if (!r.kind || r.idx < 1) fail("reference needs kind (face|edge) and index >= 1, or a string like \"b1.f3\"");
            return r;
        }
        if (!v.is_string()) fail("a reference is a string like \"b1.f3\" or {body, kind, index}");
        std::string s = v.get<std::string>();
        {   // "b1.top", "top", "b1.+Z": a face by the way it faces
            static const std::regex rd(R"(^(?:(b\d+)\.)?([A-Za-z+\-]+)$)");
            std::smatch md;
            gp_Dir d;
            if (std::regex_match(s, md, rd) && direction_name(md[2].str(), d)) {
                r.body = md[1].str(); r.kind = 'f'; r.by_dir = true; r.dir = d; r.dir_name = md[2].str();
                return r;
            }
        }
        static const std::regex re(R"(^(?:(b\d+)\.)?([fe])(\d+)$)");
        std::smatch m;
        if (!std::regex_match(s, m, re)) fail("bad reference '" + s + "'; use \"b1.f3\" for face 3 of body b1, \"b1.e5\" for an edge, or \"b1.top\" (top bottom left right front back, +X -X +Y -Y +Z -Z) for the outermost face pointing that way");
        r.body = m[1].str();
        r.kind = m[2].str()[0];
        r.idx = std::stoi(m[3].str());
        return r;
    }
    Body & body(const std::string & id) {
        if (b_.bodies.empty()) fail("there is no body yet; create one with extrude, revolve, box or cylinder first");
        if (id.empty()) {
            if (b_.bodies.size() > 1) fail("there are several bodies (" + body_ids() + "); name one with \"body\"");
            return b_.bodies[0];
        }
        for (auto & b : b_.bodies) if (b.id == id) return b;
        fail("no body '" + id + "' (have " + body_ids() + ")");
    }
    std::string body_ids() {
        std::string s;
        for (auto & b : b_.bodies) s += (s.empty() ? "" : ", ") + b.id;
        return s;
    }
    TopoDS_Shape resolve(const Ref & r, std::string * body_id = nullptr) {
        Body & b = body(r.body);
        if (body_id) *body_id = b.id;
        if (r.by_dir) {
            int i = face_by_direction(b.shape, r.dir);
            if (!i) fail("body " + b.id + " has no flat face pointing " + r.dir_name);
            return sub_shape(b.shape, TopAbs_FACE, i);
        }
        return sub_shape(b.shape, r.kind == 'f' ? TopAbs_FACE : TopAbs_EDGE, r.idx);
    }
    std::string pick_body(const json & spec) {
        if (spec.contains("body") && spec["body"].is_string()) return body(spec["body"].get<std::string>()).id;
        return body("").id;
    }

    // ---- planes and sketches ----
    gp_Ax3 plane_of(const json & p, double offset) {
        if (p.is_string()) {
            std::string s = p.get<std::string>();
            for (auto & c : s) c = (char)std::toupper((unsigned char)c);
            if (s == "XY") return gp_Ax3(gp_Pnt(0, 0, offset), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0));
            if (s == "YZ") return gp_Ax3(gp_Pnt(offset, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0));
            if (s == "XZ") return gp_Ax3(gp_Pnt(0, -offset, 0), gp_Dir(0, -1, 0), gp_Dir(1, 0, 0));
            static const std::regex re(R"(^(?:B\d+\.)?F\d+$)");
            static const std::regex rdir(R"(^(?:B\d+\.)?(TOP|BOTTOM|LEFT|RIGHT|FRONT|BACK|[+\-][XYZ])$)");
            if (std::regex_match(s, re) || std::regex_match(s, rdir)) {
                for (auto & c : s) c = (char)std::tolower((unsigned char)c);
                return face_plane(json(s), offset);
            }
            fail("unknown plane '" + p.get<std::string>() + "'; use XY, YZ, XZ or a planar face like \"b1.f3\"");
        }
        if (p.is_object() && p.contains("face")) return face_plane(p["face"], p.contains("offset") ? N(p["offset"]) : offset);
        fail("plane must be \"XY\", \"YZ\", \"XZ\", a face reference string, or {face: ref, offset: mm}");
    }
    gp_Ax3 face_plane(const json & ref, double offset) {
        Ref r = parse_ref(ref, 'f');
        if (r.kind != 'f') fail("a sketch plane must be a face");
        TopoDS_Face f = TopoDS::Face(resolve(r));
        gp_Ax3 ax = face_frame(f);
        if (offset != 0) ax.Translate(gp_Vec(ax.Direction()) * offset);
        return ax;
    }

    void sketch(const json & it) {
        double off = it.contains("offset") ? N(it["offset"], "offset") : 0;
        if (!it.contains("plane")) fail("sketch needs a plane (XY, YZ, XZ or a face reference)");
        gp_Ax3 ax = plane_of(it["plane"], off);
        std::map<std::string, std::pair<double, double>> named;
        auto P = [&](const json & v) -> std::pair<double, double> {
            if (v.is_string()) {
                auto f = named.find(v.get<std::string>());
                if (f != named.end()) return f->second;
                fail("unknown sketch point '" + v.get<std::string>() + "'");
            }
            auto a = NA(v, 2, "point [x, y]");
            return {a[0], a[1]};
        };
        if (it.contains("points")) {
            if (!it["points"].is_object()) fail("points must be an object {name: [x, y]}");
            for (auto & [k, v] : it["points"].items()) { auto a = NA(v, 2, "point [x, y]"); named[k] = {a[0], a[1]}; }
        }
        std::vector<TopoDS_Edge> edges;
        auto line = [&](std::pair<double, double> a, std::pair<double, double> b) {
            gp_Pnt p1 = to3(ax, a.first, a.second), p2 = to3(ax, b.first, b.second);
            if (p1.Distance(p2) < 1e-6) fail("zero-length line");
            edges.push_back(BRepBuilderAPI_MakeEdge(p1, p2).Edge());
        };
        if (!it.contains("entities") || !it["entities"].is_array() || it["entities"].empty()) fail("sketch needs a non-empty 'entities' array");
        for (auto & e : it["entities"]) {
            std::string t = e.value("type", "");
            if (t == "line") {
                line(P(e.at("from")), P(e.at("to")));
            } else if (t == "rect") {
                auto sz = NA(e.at("size"), 2, "size [w, h]");
                std::pair<double, double> c0;
                if (e.contains("center")) { auto c = P(e["center"]); c0 = {c.first - sz[0] / 2, c.second - sz[1] / 2}; }
                else if (e.contains("corner")) c0 = P(e["corner"]);
                else fail("rect needs 'corner' or 'center' plus 'size'");
                if (sz[0] <= 0 || sz[1] <= 0) fail("rect size must be positive");
                std::pair<double, double> a = c0, b{c0.first + sz[0], c0.second}, c{c0.first + sz[0], c0.second + sz[1]}, d{c0.first, c0.second + sz[1]};
                line(a, b); line(b, c); line(c, d); line(d, a);
            } else if (t == "circle") {
                auto c = P(e.at("center"));
                double r = e.contains("diameter") ? NK(e, "diameter") / 2 : NK(e, "radius");
                if (r <= 0) fail("circle radius must be positive");
                gp_Circ ci(gp_Ax2(to3(ax, c.first, c.second), ax.Direction(), ax.XDirection()), r);
                edges.push_back(BRepBuilderAPI_MakeEdge(ci).Edge());
            } else if (t == "arc") {
                auto c = P(e.at("center"));
                double r = NK(e, "radius"), a0 = NK(e, "start_angle"), a1 = NK(e, "end_angle");
                if (r <= 0) fail("arc radius must be positive");
                while (a1 <= a0) a1 += 360;
                gp_Circ ci(gp_Ax2(to3(ax, c.first, c.second), ax.Direction(), ax.XDirection()), r);
                edges.push_back(BRepBuilderAPI_MakeEdge(ci, a0 * kDeg, a1 * kDeg).Edge());
            } else if (t == "arc3") {
                auto a = P(e.at("from")), m = P(e.at("via")), b = P(e.at("to"));
                GC_MakeArcOfCircle mk(to3(ax, a.first, a.second), to3(ax, m.first, m.second), to3(ax, b.first, b.second));
                if (!mk.IsDone()) fail("arc3 points are collinear");
                edges.push_back(BRepBuilderAPI_MakeEdge(mk.Value()).Edge());
            } else if (t == "polyline") {
                if (!e.contains("points") || !e["points"].is_array() || e["points"].size() < 2) fail("polyline needs 'points' with at least 2 entries");
                std::vector<std::pair<double, double>> pts;
                for (auto & p : e["points"]) pts.push_back(P(p));
                for (size_t i = 0; i + 1 < pts.size(); i++) line(pts[i], pts[i + 1]);
                if (e.value("closed", false)) line(pts.back(), pts.front());
            } else {
                fail("unknown sketch entity type '" + t + "'; use line, rect, circle, arc, arc3 or polyline");
            }
        }
        int open = 0;
        auto faces = faces_from_edges(edges, ax, &open);
        if (faces.empty())
            fail(open ? "the sketch has no closed profile (" + std::to_string(open) + " open chain(s)); end points must coincide within 0.0001 mm"
                      : "the sketch has no closed profile");
        // stable order: by lower-left corner of the profile in sketch coordinates, then larger area first
        struct PI { TopoDS_Face f; double u0, v0, a; };
        std::vector<PI> pis;
        for (auto & f : faces) {
            Bnd_Box bb;
            BRepBndLib::Add(f, bb);
            double umin = 1e300, vmin = 1e300;
            double x0, y0, z0, x1, y1, z1;
            bb.Get(x0, y0, z0, x1, y1, z1);
            for (int i = 0; i < 8; i++) {
                auto q = to2(ax, gp_Pnt(i & 1 ? x1 : x0, i & 2 ? y1 : y0, i & 4 ? z1 : z0));
                umin = std::min(umin, q.first);
                vmin = std::min(vmin, q.second);
            }
            pis.push_back({f, std::round(umin * 1e3) / 1e3, std::round(vmin * 1e3) / 1e3, area_of(f)});
        }
        std::sort(pis.begin(), pis.end(), [](const PI & a, const PI & b) {
            if (a.u0 != b.u0) return a.u0 < b.u0;
            if (a.v0 != b.v0) return a.v0 < b.v0;
            return a.a > b.a;
        });
        SketchBuilt sb;
        sb.ax = ax;
        std::string sid = it.at("id");
        json profs = json::array();
        int k = 1;
        for (auto & p : pis) {
            sb.profiles.push_back(p.f);
            int holes = 0;
            for (TopExp_Explorer ex(p.f, TopAbs_WIRE); ex.More(); ex.Next()) holes++;
            auto c = to2(ax, surf_center(p.f));
            profs.push_back({{"id", sid + ".p" + std::to_string(k++)}, {"area", rnd(p.a)}, {"holes", holes - 1},
                             {"centroid_uv", json::array({rnd(c.first), rnd(c.second)})}});
        }
        sb.info = {{"sketch", sid},
                   {"frame", {{"origin", vec3(ax.Location())}, {"x_dir", vec3(ax.XDirection())}, {"y_dir", vec3(ax.YDirection())}, {"normal", vec3(ax.Direction())}}},
                   {"profiles", profs}};
        if (open) sb.info["warning"] = std::to_string(open) + " open chain(s) ignored";
        if (it.contains("constraints") && !it["constraints"].empty())
            sb.info["warning"] = "constraints are not supported by this kernel and were ignored; use dimensions and parameters";
        b_.info[sid] = sb.info;
        b_.sketches[sid] = std::move(sb);
    }

    // ---- features ----
    std::vector<TopoDS_Face> profiles_for(const json & f, gp_Ax3 * ax) {
        std::vector<std::string> ids;
        auto add = [&](const json & v) { if (!v.is_string()) fail("profile ids are strings like \"s1.p1\""); ids.push_back(v.get<std::string>()); };
        if (f.contains("profiles") && f["profiles"].is_array()) for (auto & v : f["profiles"]) add(v);
        else if (f.contains("profile")) add(f["profile"]);
        else if (f.contains("sketch")) add(f["sketch"]);
        else {
            std::string last;
            for (auto & it : d_.items) if (it.value("kind", "") == "sketch") last = it["id"];
            if (last.empty()) fail("no sketch to use; create one with cad_sketch first");
            ids.push_back(last);
        }
        std::vector<TopoDS_Face> out;
        for (auto & id : ids) {
            size_t dot = id.find('.');
            std::string sid = id.substr(0, dot);
            auto s = b_.sketches.find(sid);
            if (s == b_.sketches.end()) fail("no sketch '" + sid + "'");
            *ax = s->second.ax;
            if (dot == std::string::npos) { for (auto & p : s->second.profiles) out.push_back(p); continue; }
            static const std::regex re(R"(^p(\d+)$)");
            std::smatch m;
            std::string ps = id.substr(dot + 1);
            if (!std::regex_match(ps, m, re) || std::stoi(m[1].str()) < 1 || std::stoi(m[1].str()) > (int)s->second.profiles.size())
                fail("no profile '" + id + "' (sketch " + sid + " has " + std::to_string(s->second.profiles.size()) + ")");
            out.push_back(s->second.profiles[std::stoi(m[1].str()) - 1]);
        }
        return out;
    }

    TopoDS_Shape check(TopoDS_Shape s, const char * what) {
        if (s.IsNull()) fail(std::string(what) + " produced no shape");
        BRepCheck_Analyzer an(s);
        if (!an.IsValid()) fail(std::string(what) + " produced an invalid solid (try different dimensions)");
        return s;
    }

    // combine a new tool solid into the model according to mode
    void combine(const TopoDS_Shape & tool, const json & f, const std::string & id) {
        std::string mode = f.value("mode", b_.bodies.empty() ? "new" : "join");
        if (mode == "new" || (b_.bodies.empty() && mode == "join")) {
            b_.bodies.push_back({"b" + std::to_string(++b_.body_counter), check(tool, "feature")});
            return;
        }
        if (b_.bodies.empty()) fail("mode '" + mode + "' needs an existing body");
        Body & b = body(f.contains("body") && f["body"].is_string() ? f["body"].get<std::string>() : "");
        double v0 = volume_of(b.shape);
        TopoDS_Shape r;
        if (mode == "join") { BRepAlgoAPI_Fuse op(b.shape, tool); if (!op.IsDone() || op.HasErrors()) fail("join failed"); r = op.Shape(); }
        else if (mode == "cut") { BRepAlgoAPI_Cut op(b.shape, tool); if (!op.IsDone() || op.HasErrors()) fail("cut failed"); r = op.Shape(); }
        else if (mode == "intersect") { BRepAlgoAPI_Common op(b.shape, tool); if (!op.IsDone() || op.HasErrors()) fail("intersect failed"); r = op.Shape(); }
        else fail("mode must be new, join, cut or intersect");
        r = unify(r);
        if (volume_of(r) < 1e-9) fail("the result is empty (the tool " + std::string(mode == "intersect" ? "does not overlap" : "removes") + " the whole body)");
        b.shape = check(r, "feature");
        double v1 = volume_of(b.shape);
        if (std::fabs(v1 - v0) < 1e-9 * std::max(1.0, v0) && mode != "intersect")
            b_.info[id]["warning"] = "the volume did not change; the tool may not touch the body";
    }

    gp_Ax1 axis_of(const json & a, const gp_Ax3 & ax) {
        if (a.is_string()) {
            std::string s = a.get<std::string>();
            if (s == "sketch_x") return gp_Ax1(ax.Location(), ax.XDirection());
            if (s == "sketch_y") return gp_Ax1(ax.Location(), ax.YDirection());
            if (s == "X") return gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(1, 0, 0));
            if (s == "Y") return gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 1, 0));
            if (s == "Z") return gp_Ax1(gp_Pnt(0, 0, 0), gp_Dir(0, 0, 1));
        } else if (a.is_object() && a.contains("from") && a.contains("to")) {
            auto p = NA(a["from"], 2, "from"), q = NA(a["to"], 2, "to");
            gp_Pnt p1 = to3(ax, p[0], p[1]), p2 = to3(ax, q[0], q[1]);
            if (p1.Distance(p2) < 1e-6) fail("axis has zero length");
            return gp_Ax1(p1, gp_Dir(gp_Vec(p1, p2)));
        }
        fail("axis must be \"sketch_x\", \"sketch_y\", \"X\", \"Y\", \"Z\" or {from: [u,v], to: [u,v]} in sketch coordinates");
    }

    std::vector<TopoDS_Edge> edges_for(const json & f, std::string * body_id) {
        if (!f.contains("edges")) fail("missing 'edges' (list of references like \"b1.e5\", \"all\", or {face: \"b1.f3\"})");
        std::vector<TopoDS_Edge> out;
        auto push = [&](const TopoDS_Shape & e, const std::string & bid) {
            if (!body_id->empty() && *body_id != bid) fail("edges must all belong to one body");
            *body_id = bid;
            for (auto & x : out) if (x.IsSame(e)) return;
            out.push_back(TopoDS::Edge(e));
        };
        json list = f["edges"].is_array() ? f["edges"] : json::array({f["edges"]});
        for (auto & v : list) {
            if (v.is_string() && v.get<std::string>() == "all") {
                Body & b = body(f.value("body", ""));
                TopTools_IndexedMapOfShape m;
                TopExp::MapShapes(b.shape, TopAbs_EDGE, m);
                for (int i = 1; i <= m.Extent(); i++) push(m(i), b.id);
            } else if (v.is_object() && v.contains("face")) {
                Ref r = parse_ref(v["face"], 'f');
                std::string bid;
                TopoDS_Shape face = resolve(r, &bid);
                for (TopExp_Explorer ex(face, TopAbs_EDGE); ex.More(); ex.Next()) push(ex.Current(), bid);
            } else {
                Ref r = parse_ref(v, 'e');
                if (r.kind != 'e') fail("expected an edge reference, got a face");
                std::string bid;
                TopoDS_Shape e = resolve(r, &bid);
                push(e, bid);
            }
        }
        if (out.empty()) fail("no edges selected");
        return out;
    }

    void feature(const json & f) {
        std::string t = f.value("type", "");
        std::string id = f.at("id");
        b_.info[id] = {{"feature", id}, {"type", t}};
        if (t == "extrude") {
            gp_Ax3 ax;
            auto faces = profiles_for(f, &ax);
            double d = NK(f, "distance");
            std::string dir = f.value("direction", "normal");
            if (d == 0) fail("distance must not be 0");
            if (d < 0) { d = -d; dir = dir == "reverse" ? "normal" : "reverse"; }
            gp_Vec n(ax.Direction());
            TopoDS_Shape acc;
            for (auto & face : faces) {
                TopoDS_Shape fc = face;
                gp_Vec v = n * d;
                if (dir == "reverse") v = n * -d;
                else if (dir == "symmetric") { gp_Trsf tr; tr.SetTranslation(n * (-d / 2)); fc = BRepBuilderAPI_Transform(face, tr, true).Shape(); }
                else if (dir != "normal") fail("direction must be normal, reverse or symmetric");
                BRepPrimAPI_MakePrism pr(fc, v);
                if (!pr.IsDone()) fail("extrusion failed");
                if (acc.IsNull()) acc = pr.Shape();
                else { BRepAlgoAPI_Fuse fu(acc, pr.Shape()); if (!fu.IsDone()) fail("joining profiles failed"); acc = fu.Shape(); }
            }
            combine(acc, f, id);
        } else if (t == "revolve") {
            gp_Ax3 ax;
            auto faces = profiles_for(f, &ax);
            gp_Ax1 axis = axis_of(f.contains("axis") ? f["axis"] : json("sketch_y"), ax);
            double ang = NK(f, "angle", false, 360);
            if (ang <= 0 || ang > 360) fail("angle must be in (0, 360]");
            TopoDS_Shape acc;
            for (auto & face : faces) {
                BRepPrimAPI_MakeRevol rv(face, axis, ang * kDeg);
                if (!rv.IsDone()) fail("revolve failed (the profile may cross the axis)");
                if (acc.IsNull()) acc = rv.Shape();
                else { BRepAlgoAPI_Fuse fu(acc, rv.Shape()); if (!fu.IsDone()) fail("joining profiles failed"); acc = fu.Shape(); }
            }
            combine(acc, f, id);
        } else if (t == "box") {
            auto sz = NA(f.at("size"), 3, "size [x, y, z]");
            if (sz[0] <= 0 || sz[1] <= 0 || sz[2] <= 0) fail("box size must be positive");
            std::vector<double> o = {0, 0, 0};
            if (f.contains("center")) { auto c = NA(f["center"], 3, "center"); o = {c[0] - sz[0] / 2, c[1] - sz[1] / 2, c[2] - sz[2] / 2}; }
            else if (f.contains("origin")) o = NA(f["origin"], 3, "origin");
            combine(BRepPrimAPI_MakeBox(gp_Pnt(o[0], o[1], o[2]), sz[0], sz[1], sz[2]).Shape(), f, id);
        } else if (t == "cylinder") {
            double r = f.contains("diameter") ? NK(f, "diameter") / 2 : NK(f, "radius"), h = NK(f, "height");
            if (r <= 0 || h <= 0) fail("cylinder radius and height must be positive");
            std::vector<double> o = f.contains("origin") ? NA(f["origin"], 3, "origin") : std::vector<double>{0, 0, 0};
            gp_Dir dir(0, 0, 1);
            if (f.contains("axis")) { auto a = NA(f["axis"], 3, "axis"); if (std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]) < 1e-9) fail("axis is zero"); dir = gp_Dir(a[0], a[1], a[2]); }
            combine(BRepPrimAPI_MakeCylinder(gp_Ax2(gp_Pnt(o[0], o[1], o[2]), dir), r, h).Shape(), f, id);
        } else if (t == "sphere") {
            double r = f.contains("diameter") ? NK(f, "diameter") / 2 : NK(f, "radius");
            if (r <= 0) fail("sphere radius must be positive");
            std::vector<double> c = f.contains("center") ? NA(f["center"], 3, "center") : std::vector<double>{0, 0, 0};
            combine(BRepPrimAPI_MakeSphere(gp_Pnt(c[0], c[1], c[2]), r).Shape(), f, id);
        } else if (t == "boolean") {
            std::string op = f.value("op", "");
            Body & a = body(f.value("target", ""));
            std::string tid = f.value("tool", "");
            if (tid.empty()) fail("boolean needs 'target' and 'tool' body ids");
            Body & bb = body(tid);
            if (&a == &bb) fail("target and tool are the same body");
            TopoDS_Shape r;
            if (op == "join") { BRepAlgoAPI_Fuse x(a.shape, bb.shape); if (!x.IsDone() || x.HasErrors()) fail("join failed"); r = x.Shape(); }
            else if (op == "cut") { BRepAlgoAPI_Cut x(a.shape, bb.shape); if (!x.IsDone() || x.HasErrors()) fail("cut failed"); r = x.Shape(); }
            else if (op == "intersect") { BRepAlgoAPI_Common x(a.shape, bb.shape); if (!x.IsDone() || x.HasErrors()) fail("intersect failed"); r = x.Shape(); }
            else fail("op must be join, cut or intersect");
            r = unify(r);
            if (volume_of(r) < 1e-9) fail("the result is empty");
            std::string keep = a.id, drop = bb.id;
            a.shape = check(r, "boolean");
            b_.bodies.erase(std::remove_if(b_.bodies.begin(), b_.bodies.end(), [&](const Body & x) { return x.id == drop; }), b_.bodies.end());
        } else if (t == "fillet" || t == "chamfer") {
            std::string bid;
            auto edges = edges_for(f, &bid);
            Body & b = body(bid);
            double v0 = volume_of(b.shape);
            TopoDS_Shape r;
            if (t == "fillet") {
                double rad = NK(f, "radius");
                if (rad <= 0) fail("radius must be positive");
                BRepFilletAPI_MakeFillet mk(b.shape);
                for (auto & e : edges) mk.Add(rad, e);
                mk.Build();
                if (!mk.IsDone()) fail("fillet failed: the radius " + std::to_string(rad) + " mm is probably too large for these edges, or the edges meet at a corner the kernel cannot blend; try a smaller radius or fewer edges");
                r = mk.Shape();
            } else {
                double dist = NK(f, "distance");
                if (dist <= 0) fail("distance must be positive");
                BRepFilletAPI_MakeChamfer mk(b.shape);
                for (auto & e : edges) mk.Add(dist, e);
                mk.Build();
                if (!mk.IsDone()) fail("chamfer failed: the distance is probably too large for these edges; try a smaller one");
                r = mk.Shape();
            }
            b.shape = check(r, t.c_str());
            b_.info[id]["volume_change"] = rnd(volume_of(b.shape) - v0);
        } else if (t == "hole") {
            Ref fr = parse_ref(f.contains("face") ? f["face"] : json(), 'f');
            if (fr.kind != 'f') fail("hole needs a 'face' reference");
            std::string bid;
            TopoDS_Face face = TopoDS::Face(resolve(fr, &bid));
            gp_Ax3 fa = face_frame(face);
            double dia = NK(f, "diameter");
            if (dia <= 0) fail("diameter must be positive");
            gp_Pnt at = fa.Location();
            if (f.contains("at")) {
                if (!f["at"].is_array()) fail("'at' is [u, v] on the face or [x, y, z] global");
                if (f["at"].size() == 2) { auto a = NA(f["at"], 2, "at"); at = to3(fa, a[0], a[1]); }
                else {
                    auto a = NA(f["at"], 3, "at");
                    auto uv = to2(fa, gp_Pnt(a[0], a[1], a[2]));
                    at = to3(fa, uv.first, uv.second);
                }
            }
            gp_Dir down = fa.Direction().Reversed();
            double depth = f.contains("depth") && !f["depth"].is_null() ? N(f["depth"]) : 0;
            Body & b = body(bid);
            double len = depth > 0 ? depth : bbox_diag(b.shape) * 2;
            const double lead = 1.0;
            gp_Pnt start(at.X() - down.X() * lead, at.Y() - down.Y() * lead, at.Z() - down.Z() * lead);
            TopoDS_Shape tool = BRepPrimAPI_MakeCylinder(gp_Ax2(start, down), dia / 2, len + lead).Shape();
            if (f.contains("cbore_diameter")) {
                double cd = NK(f, "cbore_diameter"), cdepth = NK(f, "cbore_depth");
                if (cd <= dia) fail("cbore_diameter must be larger than diameter");
                if (cdepth <= 0) fail("cbore_depth must be positive");
                TopoDS_Shape cb = BRepPrimAPI_MakeCylinder(gp_Ax2(start, down), cd / 2, cdepth + lead).Shape();
                BRepAlgoAPI_Fuse fu(tool, cb);
                if (!fu.IsDone()) fail("counterbore failed");
                tool = fu.Shape();
            }
            BRepAlgoAPI_Cut cut(b.shape, tool);
            if (!cut.IsDone() || cut.HasErrors()) fail("hole cut failed");
            TopoDS_Shape r = unify(cut.Shape());
            double v0 = volume_of(b.shape);
            TopoDS_Shape cut_shape = check(r, "hole");
            if (std::fabs(volume_of(cut_shape) - v0) < 1e-9)
                fail("the hole removed no material: its position is off the face (\"at\" is measured from the face centre)");
            b.shape = cut_shape;
        } else if (t == "shell") {
            Body & b = body(f.value("body", ""));
            double th = NK(f, "thickness");
            if (th <= 0) fail("thickness must be positive");
            TopTools_ListOfShape rm;
            if (f.contains("remove")) {
                json list = f["remove"].is_array() ? f["remove"] : json::array({f["remove"]});
                for (auto & v : list) {
                    Ref r = parse_ref(v, 'f');
                    if (r.kind != 'f') fail("'remove' takes face references");
                    rm.Append(resolve(r));
                }
            }
            BRepOffsetAPI_MakeThickSolid mk;
            mk.MakeThickSolidByJoin(b.shape, rm, -th, 1e-3);
            if (!mk.IsDone()) fail("shell failed: the wall thickness may be too large for the geometry");
            b.shape = check(mk.Shape(), "shell");
        } else {
            fail("unknown feature type '" + t + "'; use extrude, revolve, box, cylinder, sphere, boolean, fillet, chamfer, hole or shell");
        }
        for (auto & b : b_.bodies) (void)b;
        json bodies = json::array();
        for (auto & b : b_.bodies) bodies.push_back({{"id", b.id}, {"volume", rnd(volume_of(b.shape))}, {"bbox", bbox_json(b.shape)}});
        b_.info[id]["bodies"] = bodies;
    }
};

// ---- the backend ----
struct Part {
    PartData d;
    Built built;
    std::vector<PartData> undo;
};

class OcctCad : public CadBackend {
public:
    OcctCad() {
        // OCCT prints progress banners to stdout; the harness owns stdout.
        Message::DefaultMessenger()->ChangePrinters().Clear();
    }
    std::string name() const override { return "opencascade"; }
    bool available(std::string &) const override { return true; }

    CadResult state() override {
        return guard([&]() -> CadResult {
            std::lock_guard<std::mutex> lk(mu_);
            json parts = json::array();
            for (auto & n : order_) parts.push_back(part_json(parts_[n], n == cur_));
            return {true, json{{"units", "mm, degrees"}, {"current", cur_}, {"parts", parts},
                               {"notes", "Prefer direction names for faces: \"b1.top\" (also bottom, left, right, front, back, +X, -X, +Y, -Y, +Z, -Z) is the outermost flat face pointing that way and stays the same after holes and fillets. Numbered references (\"b1.f3\", \"b1.e5\") are 1-based indices from this listing and shift after every feature. Plane XY normal +Z, YZ +X, XZ -Y (sketch x = X, sketch y = Z). Trig in expressions uses degrees."}}.dump(2)};
        });
    }
    CadResult new_part(const std::string & name) override {
        return guard([&]() -> CadResult {
            std::lock_guard<std::mutex> lk(mu_);
            if (name.empty()) fail("name must not be empty");
            if (parts_.count(name)) fail("a part named '" + name + "' already exists; pick a new name (parts are never overwritten)");
            create(name);
            return {true, "started part '" + name + "' (now current). Units are mm and degrees."};
        });
    }
    CadResult sketch(const json & spec) override {
        return guard([&]() -> CadResult {
            std::lock_guard<std::mutex> lk(mu_);
            if (!spec.is_object()) fail("sketch spec must be an object {plane, entities, ...}");
            Part & p = current();
            PartData cand = p.d;
            json it = spec;
            it["kind"] = "sketch";
            it["id"] = "s" + std::to_string(++cand.n_sketch);
            cand.items.push_back(it);
            return commit(p, cand, it["id"]);
        });
    }
    CadResult feature(const json & spec) override {
        return guard([&]() -> CadResult {
            std::lock_guard<std::mutex> lk(mu_);
            if (!spec.is_object() || !spec.contains("type")) fail("feature spec needs a 'type' (extrude, revolve, box, cylinder, sphere, boolean, fillet, chamfer, hole, shell)");
            Part & p = current();
            PartData cand = p.d;
            json it = spec;
            it["kind"] = "feature";
            it["id"] = "f" + std::to_string(++cand.n_feat);
            cand.items.push_back(it);
            return commit(p, cand, it["id"]);
        });
    }
    CadResult set_param(const std::string & name, const std::string & expr) override {
        return guard([&]() -> CadResult {
            std::lock_guard<std::mutex> lk(mu_);
            static const std::regex re(R"(^[A-Za-z_][A-Za-z0-9_]*$)");
            if (!std::regex_match(name, re)) fail("parameter names are letters, digits and underscores, not starting with a digit");
            if (name == "pi") fail("'pi' is reserved");
            Part & p = current();
            PartData cand = p.d;
            bool found = false;
            for (auto & kv : cand.params) if (kv.first == name) { kv.second = expr; found = true; }
            if (!found) cand.params.push_back({name, expr});
            Eval ev(cand.params);
            double v = ev.eval(expr);   // fails on unknown names, cycles, bad syntax
            commit(p, cand, "");
            return {true, "parameter " + name + " = " + expr + " (" + std::to_string(rnd(v)) + ")" + (cand.items.empty() ? "" : "; model rebuilt. Face and edge indices may have changed, read cad_state.")};
        });
    }
    CadResult undo() override {
        return guard([&]() -> CadResult {
            std::lock_guard<std::mutex> lk(mu_);
            Part & p = current();
            if (p.undo.empty()) fail("nothing to undo");
            PartData prev = p.undo.back();
            p.undo.pop_back();
            p.built = Rebuilder(prev).run();
            p.d = prev;
            return {true, "undone; timeline now has " + std::to_string(p.d.items.size()) + " item(s)"};
        });
    }
    CadResult measure(const json & refs) override {
        return guard([&]() -> CadResult {
            std::lock_guard<std::mutex> lk(mu_);
            Part & p = current();
            Eval ev(p.d.params);
            json out = json::array();
            if (!refs.is_array() || refs.empty()) {
                out.push_back(part_summary(p, ev, json::object()));
            } else {
                for (auto & r : refs) {
                    if (!r.is_object()) fail("each measure entry is an object, e.g. {\"body\":\"b1\"}, {\"face\":\"b1.f2\"}, {\"between\":[\"b1.f2\",\"b1.f5\"]}");
                    out.push_back(measure_one(p, ev, r));
                }
            }
            return {true, out.dump(2)};
        });
    }
    CadResult export_to(const std::string & fmt, const std::string & path) override { return export_opts(fmt, path, json::object()); }
    CadResult export_opts(const std::string & fmt, const std::string & path, const json & opts) override {
        return guard([&]() -> CadResult {
            std::lock_guard<std::mutex> lk(mu_);
            Part & p = current();
            TopoDS_Shape s = whole(p);
            if (s.IsNull()) fail("the part has no geometry to export");
            if (fmt == "stl") {
                double tol = opts.is_object() && opts.contains("tolerance") && opts["tolerance"].is_number() ? opts["tolerance"].get<double>() : 0.05;
                if (tol <= 0) fail("tolerance must be positive");
                BRepTools::Clean(s);
                BRepMesh_IncrementalMesh mesher(s, tol, false, 0.3, true);
                StlAPI_Writer w;
                w.ASCIIMode() = false;
                if (!w.Write(s, path.c_str())) fail("the kernel could not write " + path);
                return {true, "wrote binary STL " + path + " (linear tolerance " + std::to_string(tol) + " mm)"};
            }
            if (fmt == "step") {
                STEPControl_Writer w;
                if (w.Transfer(s, STEPControl_AsIs) != IFSelect_RetDone) fail("STEP transfer failed");
                if (w.Write(path.c_str()) != IFSelect_RetDone) fail("the kernel could not write " + path);
                return {true, "wrote STEP " + path};
            }
            fail("fmt must be stl or step");
        });
    }
    bool mesh(CadMesh & out, std::string & err, double tol) override {
        try {
            std::lock_guard<std::mutex> lk(mu_);
            if (cur_.empty()) { err = "no part yet"; return false; }
            Part & p = parts_[cur_];
            if (p.built.bodies.empty()) { err = "the part has no bodies"; return false; }
            out = CadMesh{};
            int bi = 0;
            for (auto & b : p.built.bodies) {
                bi++;
                TopoDS_Shape s = b.shape;
                BRepTools::Clean(s);
                BRepMesh_IncrementalMesh mesher(s, tol > 0 ? tol : 0.1, false, 0.4, true);
                TopTools_IndexedMapOfShape faces;
                TopExp::MapShapes(s, TopAbs_FACE, faces);
                for (int fi = 1; fi <= faces.Extent(); fi++) {
                    TopoDS_Face f = TopoDS::Face(faces(fi));
                    TopLoc_Location loc;
                    Handle(Poly_Triangulation) tr = BRep_Tool::Triangulation(f, loc);
                    if (tr.IsNull()) continue;
                    bool rev = f.Orientation() == TopAbs_REVERSED;
                    uint32_t base = (uint32_t)(out.positions.size() / 3);
                    std::vector<gp_Vec> nrm(tr->NbNodes() + 1, gp_Vec(0, 0, 0));
                    for (int i = 1; i <= tr->NbNodes(); i++) {
                        gp_Pnt q = tr->Node(i).Transformed(loc.Transformation());
                        out.positions.push_back((float)q.X());
                        out.positions.push_back((float)q.Y());
                        out.positions.push_back((float)q.Z());
                    }
                    for (int i = 1; i <= tr->NbTriangles(); i++) {
                        int a, b2, c;
                        tr->Triangle(i).Get(a, b2, c);
                        if (rev) std::swap(b2, c);
                        gp_Pnt pa = tr->Node(a).Transformed(loc.Transformation()), pb = tr->Node(b2).Transformed(loc.Transformation()),
                               pc = tr->Node(c).Transformed(loc.Transformation());
                        gp_Vec n = gp_Vec(pa, pb).Crossed(gp_Vec(pa, pc));   // area weighted
                        nrm[a] += n; nrm[b2] += n; nrm[c] += n;
                        out.indices.push_back(base + a - 1);
                        out.indices.push_back(base + b2 - 1);
                        out.indices.push_back(base + c - 1);
                        out.tri_face.push_back(fi);
                        out.tri_body.push_back(bi);
                    }
                    for (int i = 1; i <= tr->NbNodes(); i++) {
                        gp_Vec n = nrm[i];
                        double m = n.Magnitude();
                        if (m > 1e-12) n /= m;
                        out.normals.push_back((float)n.X());
                        out.normals.push_back((float)n.Y());
                        out.normals.push_back((float)n.Z());
                    }
                }
            }
            if (out.indices.empty()) { err = "meshing produced no triangles"; return false; }
            return true;
        } catch (const CadError & e) {
            err = e.what();
        } catch (const Standard_Failure & e) {
            err = std::string("kernel error: ") + (e.GetMessageString() ? e.GetMessageString() : "unknown");
        }
        return false;
    }

private:
    std::mutex mu_;
    std::map<std::string, Part> parts_;
    std::vector<std::string> order_;
    std::string cur_;

    static CadResult guard(const std::function<CadResult()> & fn) {
        try {
            return fn();
        } catch (const CadError & e) {
            return {false, std::string("error: ") + e.what()};
        } catch (const Standard_Failure & e) {
            return {false, std::string("error: kernel failure: ") + (e.GetMessageString() ? e.GetMessageString() : e.DynamicType()->Name())};
        } catch (const std::exception & e) {
            return {false, std::string("error: ") + e.what()};
        }
    }
    Part & create(const std::string & name) {
        Part & p = parts_[name];
        p.d.name = name;
        order_.push_back(name);
        cur_ = name;
        return p;
    }
    Part & current() {
        if (cur_.empty()) create("part1");
        return parts_[cur_];
    }
    // rebuild a candidate; on success it becomes the part (old one onto the undo stack)
    CadResult commit(Part & p, const PartData & cand, const std::string & new_id) {
        Built nb = Rebuilder(cand).run();
        p.undo.push_back(p.d);
        if (p.undo.size() > 200) p.undo.erase(p.undo.begin());
        p.d = cand;
        p.built = std::move(nb);
        if (new_id.empty()) return {true, ""};
        json info = p.built.info.count(new_id) ? p.built.info[new_id] : json::object();
        return {true, info.dump(2)};
    }
    TopoDS_Shape whole(Part & p) {
        if (p.built.bodies.empty()) return TopoDS_Shape();
        if (p.built.bodies.size() == 1) return p.built.bodies[0].shape;
        TopoDS_Compound c;
        BRep_Builder bb;
        bb.MakeCompound(c);
        for (auto & b : p.built.bodies) bb.Add(c, b.shape);
        return c;
    }

    json face_json(const std::string & bid, int i, const TopoDS_Face & f) {
        BRepAdaptor_Surface a(f);
        json j = {{"id", bid + ".f" + std::to_string(i)}, {"type", face_type(a)}, {"area", rnd(area_of(f))}, {"center", vec3(surf_center(f))}};
        if (a.GetType() == GeomAbs_Plane) j["normal"] = vec3(plane_normal(f));
        else if (a.GetType() == GeomAbs_Cylinder) { j["radius"] = rnd(a.Cylinder().Radius()); j["axis"] = vec3(a.Cylinder().Axis().Direction()); }
        else if (a.GetType() == GeomAbs_Sphere) j["radius"] = rnd(a.Sphere().Radius());
        else if (a.GetType() == GeomAbs_Cone) j["half_angle_deg"] = rnd(a.Cone().SemiAngle() / kDeg);
        return j;
    }
    json edge_json(const std::string & bid, int i, const TopoDS_Edge & e) {
        BRepAdaptor_Curve c(e);
        std::string t = c.GetType() == GeomAbs_Line ? "line" : c.GetType() == GeomAbs_Circle ? "circle" : c.GetType() == GeomAbs_Ellipse ? "ellipse" : "curve";
        gp_Pnt mid = c.Value(0.5 * (c.FirstParameter() + c.LastParameter()));
        json j = {{"id", bid + ".e" + std::to_string(i)}, {"type", t}, {"length", rnd(length_of(e))}, {"mid", vec3(mid)}};
        if (c.GetType() == GeomAbs_Circle) { j["radius"] = rnd(c.Circle().Radius()); j["center"] = vec3(c.Circle().Location()); }
        return j;
    }
    json part_json(Part & p, bool current) {
        json params = json::array();
        Eval ev(p.d.params);
        for (auto & kv : p.d.params) {
            json pj = {{"name", kv.first}, {"expr", kv.second}};
            try { pj["value"] = rnd(ev.eval(kv.second)); } catch (...) {}
            params.push_back(pj);
        }
        json tl = json::array();
        for (auto & it : p.d.items) {
            json row = {{"id", it["id"]}, {"kind", it["kind"]}};
            if (it.contains("type")) row["type"] = it["type"];
            row["spec"] = it;
            tl.push_back(row);
        }
        json bodies = json::array();
        const size_t cap = 250;
        for (auto & b : p.built.bodies) {
            TopTools_IndexedMapOfShape fm, em;
            TopExp::MapShapes(b.shape, TopAbs_FACE, fm);
            TopExp::MapShapes(b.shape, TopAbs_EDGE, em);
            json faces = json::array(), edges = json::array();
            for (int i = 1; i <= fm.Extent() && (size_t)i <= cap; i++) faces.push_back(face_json(b.id, i, TopoDS::Face(fm(i))));
            for (int i = 1; i <= em.Extent() && (size_t)i <= cap; i++) edges.push_back(edge_json(b.id, i, TopoDS::Edge(em(i))));
            json bj = {{"id", b.id}, {"volume", rnd(volume_of(b.shape))}, {"area", rnd(area_of(b.shape))}, {"bbox", bbox_json(b.shape)},
                       {"face_count", fm.Extent()}, {"edge_count", em.Extent()}, {"faces", faces}, {"edges", edges}};
            if (fm.Extent() > (int)cap || em.Extent() > (int)cap) bj["truncated"] = "listing capped at 250 faces and 250 edges";
            bodies.push_back(bj);
        }
        json sketches = json::array();
        for (auto & [id, sb] : p.built.sketches) sketches.push_back(sb.info);
        return {{"name", p.d.name}, {"current", current}, {"parameters", params}, {"timeline", tl}, {"sketches", sketches}, {"bodies", bodies}};
    }

    json body_summary(Part & p, Eval & ev, const Body & b, const json & opts) {
        GProp_GProps vp;
        BRepGProp::VolumeProperties(b.shape, vp);
        json j = {{"body", b.id}, {"volume_mm3", rnd(vp.Mass())}, {"surface_area_mm2", rnd(area_of(b.shape))}, {"center_of_mass", vec3(vp.CentreOfMass())}, {"bbox", bbox_json(b.shape)}};
        if (opts.contains("density")) {
            double rho = opts["density"].is_number() ? opts["density"].get<double>() : ev.eval(opts["density"].get<std::string>());
            j["density_g_cm3"] = rho;
            j["mass_g"] = rnd(vp.Mass() / 1000.0 * rho);
        }
        return j;
    }
    json part_summary(Part & p, Eval & ev, const json & opts) {
        if (p.built.bodies.empty()) fail("the part has no bodies yet");
        json bodies = json::array();
        double v = 0, m = 0;
        for (auto & b : p.built.bodies) {
            json bj = body_summary(p, ev, b, opts);
            v += bj["volume_mm3"].get<double>();
            if (bj.contains("mass_g")) m += bj["mass_g"].get<double>();
            bodies.push_back(bj);
        }
        json j = {{"part", p.d.name}, {"bodies", bodies}, {"total_volume_mm3", rnd(v)}};
        if (opts.contains("density")) j["total_mass_g"] = rnd(m);
        return j;
    }
    // resolve a measure operand: body id, face/edge reference or [x,y,z]
    TopoDS_Shape operand(Part & p, const json & v) {
        if (v.is_array()) {
            if (v.size() != 3) fail("a point is [x, y, z]");
            Eval ev(p.d.params);
            auto g = [&](const json & x) { return x.is_number() ? x.get<double>() : ev.eval(x.get<std::string>()); };
            return BRepBuilderAPI_MakeVertex(gp_Pnt(g(v[0]), g(v[1]), g(v[2]))).Shape();
        }
        if (!v.is_string()) fail("operands are strings like \"b1\", \"b1.f2\", \"b1.e3\" or a point [x,y,z]");
        std::string s = v.get<std::string>();
        static const std::regex rb(R"(^b\d+$)"), rf(R"(^(?:(b\d+)\.)?([fe])(\d+)$)");
        std::smatch m;
        auto find_body = [&](const std::string & id) -> TopoDS_Shape {
            if (p.built.bodies.empty()) fail("the part has no bodies yet");
            if (id.empty()) { if (p.built.bodies.size() > 1) fail("several bodies; use \"b1.f2\" style references"); return p.built.bodies[0].shape; }
            for (auto & b : p.built.bodies) if (b.id == id) return b.shape;
            fail("no body '" + id + "'");
        };
        if (std::regex_match(s, rb)) return find_body(s);
        if (std::regex_match(s, m, rf)) return sub_shape(find_body(m[1].str()), m[2].str() == "f" ? TopAbs_FACE : TopAbs_EDGE, std::stoi(m[3].str()));
        fail("bad reference '" + s + "'");
    }
    json measure_one(Part & p, Eval & ev, const json & r) {
        if (r.contains("between")) {
            if (!r["between"].is_array() || r["between"].size() != 2) fail("'between' takes two operands");
            TopoDS_Shape a = operand(p, r["between"][0]), b = operand(p, r["between"][1]);
            BRepExtrema_DistShapeShape d(a, b);
            if (!d.IsDone() || d.NbSolution() < 1) fail("could not compute the distance");
            return {{"between", r["between"]}, {"min_distance_mm", rnd(d.Value())}, {"point_a", vec3(d.PointOnShape1(1))}, {"point_b", vec3(d.PointOnShape2(1))}};
        }
        if (r.contains("section")) {
            const json & sc = r["section"];
            Eval & e2 = ev;
            double off = sc.contains("offset") ? (sc["offset"].is_number() ? sc["offset"].get<double>() : e2.eval(sc["offset"].get<std::string>())) : 0;
            gp_Ax3 ax;
            std::string pl = sc.value("plane", "XY");
            for (auto & c : pl) c = (char)std::toupper((unsigned char)c);
            if (pl == "XY") ax = gp_Ax3(gp_Pnt(0, 0, off), gp_Dir(0, 0, 1), gp_Dir(1, 0, 0));
            else if (pl == "YZ") ax = gp_Ax3(gp_Pnt(off, 0, 0), gp_Dir(1, 0, 0), gp_Dir(0, 1, 0));
            else if (pl == "XZ") ax = gp_Ax3(gp_Pnt(0, -off, 0), gp_Dir(0, -1, 0), gp_Dir(1, 0, 0));
            else fail("section plane must be XY, YZ or XZ");
            TopoDS_Shape target = r.contains("body") ? operand(p, r["body"]) : operand(p, json("b" + std::string("1")));
            if (!r.contains("body") && p.built.bodies.size() > 1) fail("several bodies; name one with \"body\"");
            BRepAlgoAPI_Section sec(target, gp_Pln(ax), false);
            sec.Build();
            if (!sec.IsDone()) fail("section failed");
            std::vector<TopoDS_Edge> edges;
            for (TopExp_Explorer ex(sec.Shape(), TopAbs_EDGE); ex.More(); ex.Next()) edges.push_back(TopoDS::Edge(ex.Current()));
            if (edges.empty()) return {{"section", sc}, {"area_mm2", 0}, {"note", "the plane does not cut the body"}};
            int open = 0;
            auto faces = faces_from_edges(edges, ax, &open);
            double a = 0;
            for (auto & f : faces) a += area_of(f);
            double per = 0;
            for (auto & e : edges) per += length_of(e);
            return {{"section", sc}, {"area_mm2", rnd(a)}, {"perimeter_mm", rnd(per)}, {"regions", (int)faces.size()}};
        }
        if (r.contains("face")) {
            TopoDS_Shape s = operand(p, r["face"]);
            if (s.ShapeType() != TopAbs_FACE) fail("'face' must be a face reference");
            json j = face_json("", 0, TopoDS::Face(s));
            j["id"] = r["face"];
            return j;
        }
        if (r.contains("edge")) {
            TopoDS_Shape s = operand(p, r["edge"]);
            if (s.ShapeType() != TopAbs_EDGE) fail("'edge' must be an edge reference");
            json j = edge_json("", 0, TopoDS::Edge(s));
            j["id"] = r["edge"];
            return j;
        }
        if (r.contains("body")) {
            for (auto & b : p.built.bodies) if (b.id == r["body"].get<std::string>()) return body_summary(p, ev, b, r);
            fail("no body " + r["body"].dump());
        }
        return part_summary(p, ev, r);
    }
};

}  // namespace

std::shared_ptr<CadBackend> make_cad_backend() { return std::make_shared<OcctCad>(); }

}  // namespace judas
