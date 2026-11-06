# Engineering mode

A sixth work surface next to Chat, Cowork, Code, Learn and Swarm: `mode = "engineering"`. It behaves like Cowork (a lead plus workers sharing one model) and, like Code, needs a project folder. It adds an engineering system prompt, a CAD tool layer behind a C++ interface, and the mode wiring in the server, TUI and web UIs.

## Single binary

Everything here is C++ in `src/` and web assets in `web/`, compiled into the one `judas` binary. Nothing outside the binary is used at runtime: no daemon, helper script, subprocess or external CAD program (MOLDAR's MCP server and SolidCraft's CLI are deliberately not wired in). Engineering mode itself (prompt, calculation workflow, UI) works with no CAD at all.

## What exists

- Mode: accepted by `POST /api/sessions`, `PATCH /api/sessions/<id>`, the message endpoint, TUI `/mode engineering`. Desktop: one rail button. Phone: an "Engineering" pill in the Chat group. A few starter prompts. Both web changes avoid CSS and markup reflow.
- Prompt: `engineering_prompt()` in `src/agent/agent.cpp`, appended to every non-review agent in an engineering session. Method: requirements as numbers with assumptions marked, first-principles calculations with units (python through the shell), sketch then model, measure and check, iterate, write notes into `<project>/artifacts/` with the normal file tools. It forbids claiming tests or results that were not run, says only one agent drives the CAD at a time, and tells the model to say plainly when this build has no CAD kernel or no render tool.
- Artifacts: minimal on purpose. The model writes files into `<project>/artifacts/` with the normal file tools. The full persistent artifact system, including rendering, is built on feature/artifacts; Engineering will use `artifact_render` from there after both branches land (the prompt already refers to it conditionally). This branch has no artifact tool, route or panel and no renderer.
- CAD interface: `src/cad/cad_backend.h`. `CadBackend` has `state`, `new_part`, `sketch`, `feature`, `set_param`, `measure`, `export_to` / `export_opts` (stl, step; STL takes a tolerance), `undo` and `mesh` (triangles of the current part with per-triangle face and body index, for the software rasterizer; no renderer is built here); millimeters and degrees. `make_cad_backend()` returns the OpenCascade backend (`src/cad/cad_occt.cpp`) when built with `JUDAS_CAD=ON` (the default), otherwise the stub (`src/cad/cad_stub.cpp`): `available()` is false with the reason "no CAD kernel compiled in" and every call returns a clear error.
- OpenCascade backend: a part is a timeline of sketches and features plus named parameters (expressions: numbers, parameter names, `+ - * / ^`, parentheses, `sin cos tan asin acos atan sqrt abs min max pow`, `pi`; trig in degrees). Every change is applied to a copy and the whole timeline is rebuilt, so a parameter change recomputes the model and a failed feature (fillet radius too big, empty boolean) is rejected with a message and leaves the model untouched. `undo` restores the previous timeline. Sketches: planes XY, YZ, XZ (offset allowed) or a planar face (`b1.f3`, origin at the face centre); entities line, rect, circle, arc, arc3, polyline; closed chains become profiles, nested loops become holes. Features: extrude (normal, reverse, symmetric; new, join, cut, intersect), revolve, box, cylinder, sphere, boolean, fillet, chamfer, hole (through or blind, counterbore), shell. `cad_state` lists every face (type, area, centre, normal, radius) and edge (type, length, midpoint) with 1-based ids such as `b1.f3` and `b1.e5`; those ids are stable for a given rebuild but can shift after a parameter change that alters topology. `cad_measure` gives volume, area, bounding box, centre of mass, mass from a density, minimum distance between bodies, faces, edges or points, and planar sections.
- CAD tools: `src/cad/cad_tools.cpp` registers `cad_state`, `cad_new_part`, `cad_sketch`, `cad_feature`, `cad_param`, `cad_measure`, `cad_export`, `cad_undo` over the backend, only when `available()` is true. With the stub the model never sees them. They are visible in engineering sessions only (`tools_for` in the agent). `cad_export` goes through the write-path policy, journals the file and refuses to overwrite.

## Config

None. There is nothing to add to `config.toml`.

## Building the kernel

`cmake/occt.cmake` builds OpenCascade as static libraries from the pinned official tag (default `V7_9_3`, `github.com/Open-Cascade-SAS/OCCT`) in an ExternalProject inside the build directory, then links them into `judas_core`. The git clone happens at build time only; nothing is downloaded at runtime and no system OCCT is used. Options: `-DJUDAS_CAD=OFF` builds the stub only (and drops `tests/test_cad_occt.cpp`); `-DJUDAS_OCCT_TAG=...`, `-DJUDAS_OCCT_SOURCE_DIR=...` (an existing checkout) and `-DJUDAS_OCCT_PREFIX=...` (an existing static install; skips building OCCT, useful to share one OCCT build between build directories).

Modules built: FoundationClasses (TKernel, TKMath), ModelingData (TKG2d, TKG3d, TKGeomBase, TKBRep), ModelingAlgorithms (TKGeomAlgo, TKTopAlgo, TKPrim, TKBO, TKBool, TKHLR, TKFillet, TKOffset, TKFeat, TKMesh, TKXMesh, TKShHealing) and the STEP and STL readers and writers (TKDE, TKXSBase, TKDESTEP, TKDESTL). TKDESTEP and TKDESTL link TKXCAF, which needs TKService, TKV3d and the OCAF toolkits (TKCDF, TKLCAF, TKCAF, TKVCAF); they are compiled without OpenGL, FreeType, Tk, Tcl, X11, TBB, RapidJSON or Draco. Draw, samples, docs, IGES, VRML, glTF, OBJ and PLY are off. OCCT runs single-threaded inside the harness (no TBB); meshing uses its own parallel option.

## CAD kernel license

Decision (2026-11-05): the public export builds with CAD on (`JUDAS_CAD=ON` stays the default) and ships the notices. How the obligations are met:

- The notices travel inside the binary: `judas licenses` lists them and `judas licenses LICENSE_LGPL_21.txt` (or `OCCT_LGPL_EXCEPTION.txt`, `THIRD_PARTY.md`) prints them; the web app serves them at `/licenses/`. Copies live in `LICENSES/` and `web/licenses/` (a test keeps `web/licenses/THIRD_PARTY.md` identical to `THIRD_PARTY.md`).
- OpenCascade source: the build fetches the pinned tag (`JUDAS_OCCT_TAG`, default V7_9_3) from github.com/Open-Cascade-SAS/OCCT; any binary we hand out names that tag in THIRD_PARTY.md, and we keep the matching source tarball available on request.
- Relinking: the Judas source is public (MIT), so anyone can rebuild Judas against a modified OpenCascade with `-DJUDAS_OCCT_SOURCE_DIR` or `-DJUDAS_OCCT_PREFIX`; that satisfies the LGPL relink requirement for binaries built from the public source. A binary built from private additions (for example the mach12 module) is for our own use and is not distributed.

The facts behind the decision:

- OpenCascade (OCCT) is licensed LGPL 2.1 with the "Open CASCADE exception" (files `LICENSE_LGPL_21.txt` and `OCCT_LGPL_EXCEPTION.txt` in the OCCT source). The OCCT CMake build itself prints a warning that LGPL 2.1 "has limitations on static linking with proprietary software" and points to Open Cascade's commercial licensing as an alternative.
- What the exception says, in short: it lets you create and distribute a larger work that incorporates OCCT (headers, templates, inline code, macros and the library) without the larger work being subject to the LGPL's conditions on a "work that uses the library", provided OCCT itself is distributed under LGPL 2.1 with the exception and its source remains available. It does not remove the obligations on OCCT itself: the copyright and license notices, source for OCCT and for any changes you make to it, and a copy of the license must accompany a binary. Read the exception text in the pinned tag before relying on this summary; this is not legal advice.
- What static linking implies: judas would be one executable that contains OCCT code. Distributing that binary means distributing OCCT under the LGPL: ship the license texts and notices (THIRD_PARTY.md has an entry), offer the corresponding OCCT source (the pinned tag plus any local patch; there are none today), and keep the user able to replace OCCT. With a plain LGPL 2.1 static link, the replaceability part is usually met by also offering the object files (or a link-ready archive) of the rest of the program so a user can relink against a modified OCCT. The exception is written to relax the "larger work" obligations for this kind of use; whether it covers the relink requirement for a statically linked executable is the point to confirm.
- Judas has a public MIT export (`judas-public`). The OCCT sources are not copied into this repository: they are fetched at build time, so the repository and the MIT export contain no OCCT code. A binary built from them with JUDAS_CAD=ON is the combined work above. Anyone building the public source gets OCCT from its own upstream under its own license.
- Options (not decided):
  1. Ship the single binary with OCCT linked in, under the exception, with notices, the pinned tag's source offer and (if desired) relinkable object files.
  2. Keep `JUDAS_CAD` ON only in private builds (the installed binary) and leave the public export building with the stub by default (`-DJUDAS_CAD=OFF` as the public default, or leave `src/cad/cad_occt.cpp` and `cmake/occt.cmake` out of the export).
  3. Buy a commercial license from Open Cascade (the CMake warning names occt3d.com) if the exception is judged insufficient.
  4. Use a different kernel or the kernel-free mesh backend (no fillets, sketches or STEP) which avoids the question.
- The binary grows by roughly the size of the linked OCCT code; the exact change is under "Verified and not".


## Starting a solo-dev test

1. `judas serve` (one model; no games alongside). Without a CAD kernel the test covers requirements, calculations and written design work only; say so in the brief.
2. Open Engineering, enter a fresh project folder, and write the brief with numbers: loads, envelope, material, process, tolerances, deliverable.
3. Stage a big job: requirements and a calculation plan first, then one calculation sheet at a time, then the summary. Judas does best on small single-file tasks.
4. Read `artifacts/` afterwards. Every number is an estimate until you check it.

## Verified and not

- `tests/test_eng_mode.cpp` (passes): prompt content (conditional CAD and render wording, no MOLDAR or render_part references, no em dash); the stub backend (unavailable, clear errors, no tools registered); the cad_* tools registered over a fake kernel map onto the interface, `cad_export` refuses overwrite, bad formats and paths outside the project, and a missing argument is a readable error.
- `tests/test_server.cpp` (passes): `engineering` is a valid session mode on creation. `test_tui_commands` updated for the sixth mode. `test_agent_policy`, `test_tools`, `test_config` pass.
- Not verified: the tool gating inside a running agent loop (only the pieces are tested), the desktop and phone UI in a browser, any model-driven Engineering session.
- Limits: no CAD kernel and no renderer in this build; the model can calculate and write but cannot yet build or look at a part.
