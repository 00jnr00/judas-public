// CAD backend interface for Engineering mode. The tools in cad_tools.cpp call only this interface, so a kernel can
// be compiled into the judas binary behind it (nothing outside the binary is ever used). With JUDAS_CAD=ON the
// OpenCascade backend (cad_occt.cpp) is used; with OFF, make_cad_backend() returns the stub, available() is false,
// and no cad_* tool is registered.
//
// Units are millimeters and degrees. Every call returns text the model reads; JSON arguments follow the
// shapes documented in each cad_* tool description.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/json.h"

namespace judas {

struct CadResult {
    bool ok = true;
    std::string text;   // what the model sees (a JSON dump or a plain message)
};

// Triangles of the current part, for a renderer. positions/normals are xyz triples (mm); indices are triangle
// corner triples into them; tri_face[i] is the 1-based face index (as in cad_state) of triangle i, 0 if unknown.
struct CadMesh {
    std::vector<float> positions;
    std::vector<float> normals;
    std::vector<uint32_t> indices;
    std::vector<int> tri_face;
    std::vector<int> tri_body;   // 1-based body index per triangle
};

class CadBackend {
public:
    virtual ~CadBackend() = default;
    virtual std::string name() const = 0;
    // false + why ("no CAD kernel compiled in") when the backend cannot work in this binary.
    virtual bool available(std::string & why) const = 0;

    virtual CadResult state() = 0;                                        // parts, features, parameters, bodies
    virtual CadResult new_part(const std::string & name) = 0;             // start an empty part (never overwrites a saved one)
    virtual CadResult sketch(const json & spec) = 0;                      // plane, points, entities, constraints; returns profiles
    virtual CadResult feature(const json & spec) = 0;                     // extrude, revolve, boolean, fillet, chamfer, hole, ...
    virtual CadResult set_param(const std::string & name, const std::string & expr) = 0;
    virtual CadResult measure(const json & refs) = 0;                     // distances, areas, bounding box, volume
    virtual CadResult export_to(const std::string & fmt, const std::string & path) = 0;   // fmt: stl | step
    virtual CadResult undo() = 0;
    // export_to with options: {"tolerance": linear deflection mm (default 0.05), "angular": radians (default 0.3)}.
    virtual CadResult export_opts(const std::string & fmt, const std::string & path, const json &) { return export_to(fmt, path); }
    // Triangulate the current part (all bodies). false + err when there is nothing to mesh or no kernel.
    virtual bool mesh(CadMesh &, std::string & err, double tolerance = 0.1) { err = "no CAD kernel compiled in"; return false; }
};

// The backend compiled into this binary: OpenCascade when built with JUDAS_CAD=ON (default), else the stub.
std::shared_ptr<CadBackend> make_cad_backend();
// The one backend instance of this process: the cad_* tools and the server's /cad endpoints share its live model.
std::shared_ptr<CadBackend> shared_cad_backend();
// One live model per session (created on first use); "" gives the process-wide instance used for availability checks.
std::shared_ptr<CadBackend> cad_backend_for(const std::string & session);
// Serializes kernel calls across sessions (OpenCascade is not built with TBB here; keep it simple and safe).
std::mutex & cad_kernel_mutex();
// A backend that fails every call with `why`; used when no kernel is compiled in.
std::shared_ptr<CadBackend> make_stub_cad_backend(const std::string & why = "no CAD kernel compiled in");

}  // namespace judas
