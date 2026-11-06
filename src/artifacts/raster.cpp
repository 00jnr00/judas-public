#include "artifacts/raster.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>

#include "core/json.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "third_party/stb/stb_image_write.h"
#define NANOSVG_IMPLEMENTATION
#include "third_party/nanosvg/nanosvg.h"
#define NANOSVGRAST_IMPLEMENTATION
#include "third_party/nanosvg/nanosvgrast.h"
#pragma GCC diagnostic pop

namespace judas {
namespace {

constexpr size_t MAX_TRIANGLES = 4000000;

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 norm(V3 a) {
    float l = std::sqrt(dot(a, a));
    return l > 1e-20f ? V3{a.x / l, a.y / l, a.z / l} : V3{0, 0, 1};
}

float rd_f32(const unsigned char * p) { float f; std::memcpy(&f, p, 4); return f; }
uint32_t rd_u32(const unsigned char * p) { uint32_t v; std::memcpy(&v, p, 4); return v; }

}  // namespace

// ---------- STL ----------

bool load_stl(const std::string & bytes, Mesh & out, std::string & err) {
    out.tri.clear();
    const unsigned char * p = (const unsigned char *) bytes.data();
    size_t n = bytes.size();
    if (n >= 84) {
        uint32_t cnt = rd_u32(p + 80);
        if (84 + (size_t) cnt * 50 == n && cnt > 0) {   // binary: the size matches the declared count exactly
            if (cnt > MAX_TRIANGLES) { err = "too many triangles"; return false; }
            out.tri.reserve((size_t) cnt * 9);
            for (uint32_t i = 0; i < cnt; i++) {
                const unsigned char * t = p + 84 + (size_t) i * 50 + 12;   // skip the normal
                for (int k = 0; k < 9; k++) {
                    float f = rd_f32(t + k * 4);
                    out.tri.push_back(std::isfinite(f) ? f : 0.f);
                }
            }
            return true;
        }
    }
    // ASCII: collect every "vertex x y z"
    const char * s = bytes.c_str();
    const char * end = s + n;
    std::vector<float> v;
    while (s < end) {
        const char * f = std::strstr(s, "vertex");
        if (!f) break;
        char * e = nullptr;
        float a = std::strtof(f + 6, &e);
        if (e == f + 6) { s = f + 6; continue; }
        float b = std::strtof(e, &e);
        float c = std::strtof(e, &e);
        v.push_back(a); v.push_back(b); v.push_back(c);
        s = e;
        if (v.size() / 9 > MAX_TRIANGLES) { err = "too many triangles"; return false; }
    }
    size_t whole = v.size() / 9 * 9;
    for (float f : v) if (!std::isfinite(f)) { err = "STL has non-finite coordinates"; return false; }
    if (whole == 0) { err = "not a readable STL (no triangles found)"; return false; }
    v.resize(whole);
    out.tri = std::move(v);
    return true;
}

// ---------- GLB ----------

namespace {

struct M4 {
    float m[16];   // column-major like glTF
};
M4 ident() { M4 r{}; r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1; return r; }
M4 mul(const M4 & a, const M4 & b) {
    M4 r{};
    for (int c = 0; c < 4; c++)
        for (int rr = 0; rr < 4; rr++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + rr] * b.m[c * 4 + k];
            r.m[c * 4 + rr] = s;
        }
    return r;
}
M4 from_trs(const float t[3], const float q[4], const float s[3]) {
    float x = q[0], y = q[1], z = q[2], w = q[3];
    M4 r = ident();
    r.m[0] = (1 - 2 * (y * y + z * z)) * s[0]; r.m[1] = (2 * (x * y + z * w)) * s[0]; r.m[2] = (2 * (x * z - y * w)) * s[0];
    r.m[4] = (2 * (x * y - z * w)) * s[1]; r.m[5] = (1 - 2 * (x * x + z * z)) * s[1]; r.m[6] = (2 * (y * z + x * w)) * s[1];
    r.m[8] = (2 * (x * z + y * w)) * s[2]; r.m[9] = (2 * (y * z - x * w)) * s[2]; r.m[10] = (1 - 2 * (x * x + y * y)) * s[2];
    r.m[12] = t[0]; r.m[13] = t[1]; r.m[14] = t[2];
    return r;
}
V3 xf(const M4 & m, V3 p) {
    return {m.m[0] * p.x + m.m[4] * p.y + m.m[8] * p.z + m.m[12], m.m[1] * p.x + m.m[5] * p.y + m.m[9] * p.z + m.m[13],
            m.m[2] * p.x + m.m[6] * p.y + m.m[10] * p.z + m.m[14]};
}

}  // namespace

bool load_glb(const std::string & bytes, Mesh & out, std::string & err) {
    out.tri.clear();
    const unsigned char * p = (const unsigned char *) bytes.data();
    size_t n = bytes.size();
    if (n < 20 || std::memcmp(p, "glTF", 4) != 0) { err = "not a GLB file"; return false; }
    if (rd_u32(p + 4) != 2) { err = "only glTF 2.0 is supported"; return false; }
    json doc;
    const unsigned char * bin = nullptr;
    size_t bin_n = 0;
    size_t off = 12;
    bool have_json = false;
    while (off + 8 <= n) {
        uint32_t len = rd_u32(p + off), type = rd_u32(p + off + 4);
        if (off + 8 + (size_t) len > n) { err = "truncated GLB"; return false; }
        if (type == 0x4E4F534A && !have_json) {
            doc = json::parse(std::string((const char *) p + off + 8, len), nullptr, false);
            have_json = !doc.is_discarded() && doc.is_object();
        } else if (type == 0x004E4942 && !bin) {
            bin = p + off + 8;
            bin_n = len;
        }
        off += 8 + (size_t) len;
    }
    if (!have_json) { err = "GLB has no readable JSON chunk"; return false; }
    auto arr = [&](const char * k) -> const json & {
        static const json empty = json::array();
        return doc.contains(k) && doc[k].is_array() ? doc[k] : empty;
    };
    const json & accessors = arr("accessors");
    const json & views = arr("bufferViews");
    const json & meshes = arr("meshes");
    const json & nodes = arr("nodes");

    // reads accessor `ai` as `comps` floats per element (positions) or as unsigned ints (indices)
    auto accessor_ptr = [&](int ai, size_t & count, size_t & stride, int & ctype, const unsigned char *& base) -> bool {
        if (ai < 0 || (size_t) ai >= accessors.size()) return false;
        const json & a = accessors[(size_t) ai];
        if (!a.contains("bufferView") || a.contains("sparse")) return false;
        int bv = a["bufferView"].get<int>();
        if (bv < 0 || (size_t) bv >= views.size() || !bin) return false;
        const json & v = views[(size_t) bv];
        if (v.value("buffer", 0) != 0) return false;
        size_t vo = v.value("byteOffset", 0), ao = a.value("byteOffset", 0);
        count = a.value("count", 0);
        ctype = a.value("componentType", 0);
        std::string ty = a.value("type", "SCALAR");
        size_t comps = ty == "VEC3" ? 3 : ty == "VEC2" ? 2 : ty == "VEC4" ? 4 : 1;
        size_t csz = (ctype == 5126 || ctype == 5125) ? 4 : (ctype == 5123) ? 2 : 1;
        stride = v.value("byteStride", 0);
        if (stride == 0) stride = comps * csz;
        if (count == 0) return false;
        size_t last = vo + ao + (count - 1) * stride + comps * csz;
        if (last > bin_n) return false;
        base = bin + vo + ao;
        return true;
    };

    std::function<void(int, const M4 &, int)> visit = [&](int ni, const M4 & parent, int depth) {
        if (ni < 0 || (size_t) ni >= nodes.size() || depth > 64 || out.tri.size() / 9 > MAX_TRIANGLES) return;
        const json & nd = nodes[(size_t) ni];
        M4 local = ident();
        if (nd.contains("matrix") && nd["matrix"].is_array() && nd["matrix"].size() == 16) {
            for (int i = 0; i < 16; i++) local.m[i] = nd["matrix"][(size_t) i].get<float>();
        } else {
            float t[3] = {0, 0, 0}, q[4] = {0, 0, 0, 1}, s[3] = {1, 1, 1};
            if (nd.contains("translation") && nd["translation"].size() == 3) for (int i = 0; i < 3; i++) t[i] = nd["translation"][(size_t) i].get<float>();
            if (nd.contains("rotation") && nd["rotation"].size() == 4) for (int i = 0; i < 4; i++) q[i] = nd["rotation"][(size_t) i].get<float>();
            if (nd.contains("scale") && nd["scale"].size() == 3) for (int i = 0; i < 3; i++) s[i] = nd["scale"][(size_t) i].get<float>();
            local = from_trs(t, q, s);
        }
        M4 world = mul(parent, local);
        if (nd.contains("mesh") && nd["mesh"].is_number_integer()) {
            int mi = nd["mesh"].get<int>();
            if (mi >= 0 && (size_t) mi < meshes.size() && meshes[(size_t) mi].contains("primitives"))
                for (auto & prim : meshes[(size_t) mi]["primitives"]) {
                    if (prim.value("mode", 4) != 4 || !prim.contains("attributes") || !prim["attributes"].contains("POSITION")) continue;
                    size_t pc, ps; int pt; const unsigned char * pb;
                    if (!accessor_ptr(prim["attributes"]["POSITION"].get<int>(), pc, ps, pt, pb) || pt != 5126) continue;
                    std::vector<V3> verts(pc);
                    for (size_t i = 0; i < pc; i++) {
                        V3 v{rd_f32(pb + i * ps), rd_f32(pb + i * ps + 4), rd_f32(pb + i * ps + 8)};
                        verts[i] = xf(world, v);
                    }
                    std::vector<uint32_t> idx;
                    if (prim.contains("indices")) {
                        size_t ic, is; int it; const unsigned char * ib;
                        if (!accessor_ptr(prim["indices"].get<int>(), ic, is, it, ib)) continue;
                        for (size_t i = 0; i < ic; i++) {
                            const unsigned char * q = ib + i * is;
                            idx.push_back(it == 5125 ? rd_u32(q) : it == 5123 ? (uint32_t) (q[0] | (q[1] << 8)) : (uint32_t) q[0]);
                        }
                    } else {
                        for (size_t i = 0; i < pc; i++) idx.push_back((uint32_t) i);
                    }
                    for (size_t i = 0; i + 2 < idx.size() && out.tri.size() / 9 < MAX_TRIANGLES; i += 3) {
                        if (idx[i] >= pc || idx[i + 1] >= pc || idx[i + 2] >= pc) continue;
                        for (int k = 0; k < 3; k++) {
                            V3 v = verts[idx[i + (size_t) k]];
                            out.tri.push_back(v.x); out.tri.push_back(-v.z); out.tri.push_back(v.y);   // Y-up to Z-up
                        }
                    }
                }
        }
        if (nd.contains("children") && nd["children"].is_array())
            for (auto & c : nd["children"]) if (c.is_number_integer()) visit(c.get<int>(), world, depth + 1);
    };

    std::vector<int> roots;
    const json & scenes = arr("scenes");
    int sc = doc.value("scene", 0);
    if (sc >= 0 && (size_t) sc < scenes.size() && scenes[(size_t) sc].contains("nodes"))
        for (auto & r : scenes[(size_t) sc]["nodes"]) if (r.is_number_integer()) roots.push_back(r.get<int>());
    if (roots.empty())
        for (size_t i = 0; i < nodes.size(); i++) roots.push_back((int) i);   // no scene: every node (children repeat; harmless)
    for (int r : roots) visit(r, ident(), 0);
    if (out.tri.empty()) { err = "GLB has no triangle meshes this renderer can read"; return false; }
    return true;
}

// ---------- rasterizer ----------

std::vector<std::string> default_views() { return {"iso", "front", "right", "top"}; }

namespace {

struct Cam {
    V3 right, up, fwd;   // fwd points from the eye into the scene
};

bool make_cam(const std::string & view, Cam & c) {
    V3 dir;   // from the scene toward the eye
    if (view == "iso") dir = norm({0.62f, -0.78f, 0.72f});
    else if (view == "front") dir = {0, -1, 0};
    else if (view == "back") dir = {0, 1, 0};
    else if (view == "right") dir = {1, 0, 0};
    else if (view == "left") dir = {-1, 0, 0};
    else if (view == "top") dir = {0, 0, 1};
    else if (view == "bottom") dir = {0, 0, -1};
    else return false;
    c.fwd = {-dir.x, -dir.y, -dir.z};
    V3 upref = (view == "top") ? V3{0, 1, 0} : (view == "bottom") ? V3{0, -1, 0} : V3{0, 0, 1};
    c.right = norm(cross(c.fwd, upref));
    c.up = cross(c.right, c.fwd);
    return true;
}

struct Rgb { float r, g, b; };

void draw_tile(const Mesh & m, const Cam & cam, V3 ctr, float radius, Image & big, int tx, int ty, int tw, int th) {
    // big is the supersampled canvas; the tile occupies [tx,tx+tw) x [ty,ty+th)
    std::vector<float> zbuf((size_t) tw * th, 1e30f);
    std::vector<int> ids((size_t) tw * th, -1);   // triangle id per pixel, for crease and silhouette lines
    std::vector<V3> nrm(m.triangles());
    // frame the projected extents of this view (not the bounding sphere), so flat views fill the tile
    float pminx = 1e30f, pmaxx = -1e30f, pminy = 1e30f, pmaxy = -1e30f;
    for (size_t i = 0; i + 2 < m.tri.size(); i += 3) {
        V3 d = V3{m.tri[i], m.tri[i + 1], m.tri[i + 2]} - ctr;
        float px = dot(d, cam.right), py = dot(d, cam.up);
        pminx = std::min(pminx, px); pmaxx = std::max(pmaxx, px);
        pminy = std::min(pminy, py); pmaxy = std::max(pmaxy, py);
    }
    if (m.tri.empty()) { pminx = pminy = -radius; pmaxx = pmaxy = radius; }
    float ext_w = std::max(pmaxx - pminx, radius * 0.02f), ext_h = std::max(pmaxy - pminy, radius * 0.02f);
    float scale = std::min((float) tw / (ext_w * 1.18f), (float) th / (ext_h * 1.18f));
    float cx = tw * 0.5f - (pminx + pmaxx) * 0.5f * scale, cy = th * 0.5f + (pminy + pmaxy) * 0.5f * scale;
    // lights in the camera frame (x right, y up, z toward the eye): key from upper left, fill from lower right
    V3 eye = {-cam.fwd.x, -cam.fwd.y, -cam.fwd.z};
    auto to_world = [&](V3 l) {
        return norm({cam.right.x * l.x + cam.up.x * l.y + eye.x * l.z, cam.right.y * l.x + cam.up.y * l.y + eye.y * l.z,
                     cam.right.z * l.x + cam.up.z * l.y + eye.z * l.z});
    };
    V3 keyw = to_world({-0.5f, 0.7f, 0.55f}), fillw = to_world({0.7f, -0.3f, 0.45f});
    const Rgb base = {0.62f, 0.69f, 0.78f};
    size_t nt = m.triangles();
    for (size_t t = 0; t < nt; t++) {
        const float * f = &m.tri[t * 9];
        V3 a{f[0], f[1], f[2]}, b{f[3], f[4], f[5]}, c{f[6], f[7], f[8]};
        V3 n = cross(b - a, c - a);
        float nl = std::sqrt(dot(n, n));
        if (nl < 1e-20f) continue;
        n = {n.x / nl, n.y / nl, n.z / nl};
        if (dot(n, eye) < 0) n = {-n.x, -n.y, -n.z};   // two sided: shade the face that looks at the camera
        nrm[t] = n;
        float kd = std::max(0.f, dot(n, keyw)), fd = std::max(0.f, dot(n, fillw));
        float hemi = 0.5f + 0.5f * dot(n, cam.up);
        float lum = 0.30f + 0.20f * hemi + 0.62f * kd + 0.22f * fd;
        lum = std::min(1.f, lum);
        float sx[3], sy[3], sz[3];
        V3 pts[3] = {a, b, c};
        for (int i = 0; i < 3; i++) {
            V3 d = pts[i] - ctr;
            sx[i] = cx + dot(d, cam.right) * scale;
            sy[i] = cy - dot(d, cam.up) * scale;
            sz[i] = dot(d, cam.fwd);
        }
        float minx = std::min({sx[0], sx[1], sx[2]}), maxx = std::max({sx[0], sx[1], sx[2]});
        float miny = std::min({sy[0], sy[1], sy[2]}), maxy = std::max({sy[0], sy[1], sy[2]});
        int x0 = std::max(0, (int) std::floor(minx)), x1 = std::min(tw - 1, (int) std::ceil(maxx));
        int y0 = std::max(0, (int) std::floor(miny)), y1 = std::min(th - 1, (int) std::ceil(maxy));
        float area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0]);
        if (std::fabs(area) < 1e-12f) continue;
        float inv = 1.f / area;
        unsigned char R = (unsigned char) std::lround(std::min(1.f, base.r * lum) * 255), G = (unsigned char) std::lround(std::min(1.f, base.g * lum) * 255),
                      B = (unsigned char) std::lround(std::min(1.f, base.b * lum) * 255);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((sx[1] - px) * (sy[2] - py) - (sx[2] - px) * (sy[1] - py)) * inv;
                float w1 = ((sx[2] - px) * (sy[0] - py) - (sx[0] - px) * (sy[2] - py)) * inv;
                float w2 = 1.f - w0 - w1;
                if (w0 < -1e-5f || w1 < -1e-5f || w2 < -1e-5f) continue;
                float z = w0 * sz[0] + w1 * sz[1] + w2 * sz[2];
                size_t zi = (size_t) y * tw + x;
                if (z >= zbuf[zi]) continue;
                zbuf[zi] = z;
                ids[zi] = (int) t;
                unsigned char * px4 = &big.rgba[((size_t) (ty + y) * big.w + (tx + x)) * 4];
                px4[0] = R; px4[1] = G; px4[2] = B; px4[3] = 255;
            }
    }
    // dark lines at silhouettes, depth steps and creases (adjacent faces turned more than about 37 degrees apart)
    float thresh = radius * 0.04f;
    for (int y = 0; y < th; y++)
        for (int x = 0; x < tw; x++) {
            size_t zi = (size_t) y * tw + x;
            if (ids[zi] < 0) continue;
            bool edge = false;
            const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            for (int k = 0; k < 4 && !edge; k++) {
                int nx = x + dx[k], ny = y + dy[k];
                if (nx < 0 || ny < 0 || nx >= tw || ny >= th) { edge = true; break; }
                size_t ni = (size_t) ny * tw + nx;
                if (ids[ni] < 0) edge = true;
                else if (ids[ni] != ids[zi])
                    edge = std::fabs(zbuf[ni] - zbuf[zi]) > thresh || dot(nrm[(size_t) ids[ni]], nrm[(size_t) ids[zi]]) < 0.8f;
            }
            if (!edge) continue;
            unsigned char * p = &big.rgba[((size_t) (ty + y) * big.w + (tx + x)) * 4];
            p[0] = (unsigned char) (p[0] * 0.3f); p[1] = (unsigned char) (p[1] * 0.3f); p[2] = (unsigned char) (p[2] * 0.33f);
        }
    // axis triad in the lower left: X red, Y green, Z blue, drawn as short lines
    float len = std::min(tw, th) * 0.09f;
    float ox = 0.07f * tw + len, oy = th - 0.07f * th - len;
    const V3 axes[3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    const unsigned char cols[3][3] = {{214, 64, 64}, {70, 160, 80}, {64, 100, 214}};
    for (int a = 0; a < 3; a++) {
        float ex = ox + dot(axes[a], cam.right) * len, ey = oy - dot(axes[a], cam.up) * len;
        int steps = (int) (len * 1.5f) + 1;
        for (int s = 0; s <= steps; s++) {
            float u = (float) s / steps;
            int x = (int) std::lround(ox + (ex - ox) * u), y = (int) std::lround(oy + (ey - oy) * u);
            for (int ddx = -1; ddx <= 0; ddx++)
                for (int ddy = -1; ddy <= 0; ddy++) {
                    int xx = x + ddx, yy = y + ddy;
                    if (xx < 0 || yy < 0 || xx >= tw || yy >= th) continue;
                    unsigned char * p = &big.rgba[((size_t) (ty + yy) * big.w + (tx + xx)) * 4];
                    p[0] = cols[a][0]; p[1] = cols[a][1]; p[2] = cols[a][2]; p[3] = 255;
                }
        }
    }
}

}  // namespace

Image render_mesh(const Mesh & m, int w, int h, const std::vector<std::string> & views_in) {
    w = std::clamp(w, 64, 2048);
    h = std::clamp(h, 64, 2048);
    std::vector<std::string> views;
    for (auto & v : views_in) { Cam c; if (make_cam(v, c)) views.push_back(v); }
    if (views.empty()) views = {"iso"};
    if (views.size() > 4) views.resize(4);
    const int SS = 2;   // supersampling factor
    Image big;
    big.w = w * SS;
    big.h = h * SS;
    big.rgba.assign((size_t) big.w * big.h * 4, 255);
    // background: a soft vertical gradient so a gray part has contrast on both edges
    for (int y = 0; y < big.h; y++) {
        float t = (float) y / big.h;
        unsigned char v = (unsigned char) std::lround(246 - 20 * t);
        for (int x = 0; x < big.w; x++) {
            unsigned char * p = &big.rgba[((size_t) y * big.w + x) * 4];
            p[0] = v; p[1] = v; p[2] = (unsigned char) std::min(255, v + 3); p[3] = 255;
        }
    }
    // bounding sphere
    V3 lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
    for (size_t i = 0; i + 2 < m.tri.size(); i += 3) {
        lo = {std::min(lo.x, m.tri[i]), std::min(lo.y, m.tri[i + 1]), std::min(lo.z, m.tri[i + 2])};
        hi = {std::max(hi.x, m.tri[i]), std::max(hi.y, m.tri[i + 1]), std::max(hi.z, m.tri[i + 2])};
    }
    if (m.tri.empty()) lo = hi = {0, 0, 0};
    V3 ctr{(lo.x + hi.x) / 2, (lo.y + hi.y) / 2, (lo.z + hi.z) / 2};
    float radius = 0;
    for (size_t i = 0; i + 2 < m.tri.size(); i += 3) {
        V3 d = V3{m.tri[i], m.tri[i + 1], m.tri[i + 2]} - ctr;
        radius = std::max(radius, std::sqrt(dot(d, d)));
    }
    if (radius < 1e-9f) radius = 1;
    int cols = views.size() == 1 ? 1 : 2, rows = (int) ((views.size() + (size_t) cols - 1) / (size_t) cols);
    int tw = big.w / cols, th = big.h / rows;
    for (size_t i = 0; i < views.size(); i++) {
        Cam cam;
        make_cam(views[i], cam);
        // a thin separator between tiles is drawn after; draw_tile paints only inside its own rectangle
        draw_tile(m, cam, ctr, radius, big, (int) (i % (size_t) cols) * tw, (int) (i / (size_t) cols) * th, tw, th);
    }
    // box downsample
    Image out;
    out.w = w;
    out.h = h;
    out.rgba.resize((size_t) w * h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            int acc[4] = {0, 0, 0, 0};
            for (int j = 0; j < SS; j++)
                for (int i = 0; i < SS; i++) {
                    const unsigned char * p = &big.rgba[((size_t) (y * SS + j) * big.w + (x * SS + i)) * 4];
                    for (int c = 0; c < 4; c++) acc[c] += p[c];
                }
            unsigned char * o = &out.rgba[((size_t) y * w + x) * 4];
            for (int c = 0; c < 4; c++) o[c] = (unsigned char) (acc[c] / (SS * SS));
        }
    if (views.size() > 1) {   // hairlines between tiles
        for (int x = 0; x < w; x++)
            if (rows > 1) { unsigned char * o = &out.rgba[((size_t) (h / rows) * w + x) * 4]; o[0] = o[1] = o[2] = 205; }
        for (int y = 0; y < h; y++) { unsigned char * o = &out.rgba[((size_t) y * w + (w / 2)) * 4]; o[0] = o[1] = o[2] = 205; }
    }
    return out;
}

// ---------- SVG ----------

bool render_svg(const std::string & svg, int w, int h, Image & out, std::string & err) {
    std::string copy = svg;   // nsvgParse edits its input
    NSVGimage * im = nsvgParse(copy.data(), "px", 96.f);
    if (!im) { err = "cannot parse the SVG"; return false; }
    if (im->width <= 0 || im->height <= 0) { nsvgDelete(im); err = "SVG has no size (add width and height or a viewBox)"; return false; }
    float sc;
    int ow, oh;
    if (w <= 0 && h <= 0) {
        sc = std::min(1.f, 2048.f / std::max(im->width, im->height));
        if (sc * std::max(im->width, im->height) < 600) sc = 600.f / std::max(im->width, im->height);
    } else {
        float sw = w > 0 ? w / im->width : 1e9f, sh = h > 0 ? h / im->height : 1e9f;
        sc = std::min(sw, sh);
    }
    ow = std::clamp((int) std::lround(im->width * sc), 1, 4096);
    oh = std::clamp((int) std::lround(im->height * sc), 1, 4096);
    NSVGrasterizer * r = nsvgCreateRasterizer();
    if (!r) { nsvgDelete(im); err = "out of memory"; return false; }
    std::vector<unsigned char> px((size_t) ow * oh * 4, 0);
    nsvgRasterize(r, im, 0, 0, sc, px.data(), ow, oh, ow * 4);
    nsvgDeleteRasterizer(r);
    nsvgDelete(im);
    out.w = ow;
    out.h = oh;
    out.rgba.resize(px.size());
    for (size_t i = 0; i < (size_t) ow * oh; i++) {   // premultiply-free composite on white
        unsigned a = px[i * 4 + 3];
        for (int c = 0; c < 3; c++) out.rgba[i * 4 + (size_t) c] = (unsigned char) ((px[i * 4 + (size_t) c] * a + 255 * (255 - a)) / 255);
        out.rgba[i * 4 + 3] = 255;
    }
    return true;
}

// ---------- PNG ----------

std::string encode_png(const Image & img) {
    std::string out;
    auto cb = [](void * ctx, void * data, int size) { static_cast<std::string *>(ctx)->append((const char *) data, (size_t) size); };
    if (!stbi_write_png_to_func(cb, &out, img.w, img.h, 4, img.rgba.data(), img.w * 4)) return "";
    return out;
}

bool png_size(const std::string & png, int & w, int & h) {
    if (png.size() < 24 || std::memcmp(png.data(), "\x89PNG\r\n\x1a\n", 8) != 0) return false;
    auto be = [&](size_t o) { return (int) (((unsigned char) png[o] << 24) | ((unsigned char) png[o + 1] << 16) | ((unsigned char) png[o + 2] << 8) | (unsigned char) png[o + 3]); };
    w = be(16);
    h = be(20);
    return true;
}

}  // namespace judas
