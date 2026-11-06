# Artifacts

Artifacts are things the model makes for you that stay: a web page, a document, a diagram (SVG), a 3D model view or an
image. They are kept on disk, every change is a new version, and you open them in the Judas web app on the desktop or on
the phone over the tailnet. The model can also render a picture of its own artifact and look at it with vision, so it can
iterate. The feature is mode agnostic; Design, Engineering, Research and Write modes can all use it.

## Single binary

Everything here is part of the one `judas` binary. There are no new daemons, services, sidecar processes, Python or shell
helpers and no extra install steps.

- Storage, serving, versions, the API and the tools are C++ in `src/artifacts` and `src/tools/artifact_tools.cpp`.
- The viewers (three.js with orbit controls, markdown viewer, capture helper, html-to-image) are plain files under
  `web/vendor/artifacts`, embedded into the binary at build time like the rest of `web/`.
- Previews are drawn inside the binary: a software rasterizer for STL and GLB (z-buffer, flat shading, key and fill light,
  crease lines), nanosvg for SVG, stb_image_write for PNG. No browser, no external program is ever started.
- html and markdown need a browser engine, and the binary has none. Those previews are captured by an open Judas web page
  (see Rendering). If no page is open, `artifact_render` says so and returns the source instead.

## Kinds

| kind | files | what the user sees |
| --- | --- | --- |
| html | `index.html` (or exactly one .html file) plus css, js, images | the page, running in a sandbox |
| svg | one `.svg` (prefer `image.svg`) | the SVG |
| markdown | one `.md` (prefer `document.md`) | a rendered document (viewer page is generated) |
| model3d | one `.stl` or `.glb` (prefer `model.stl`) | orbit viewer, three.js (viewer page is generated) |
| image | one png, jpg, gif or webp | the image |

For markdown and model3d the entry `index.html` is generated per version and cannot be sent by the model. The viewer code
itself lives in `web/vendor/artifacts/` (`viewer3d.js`, `mdview.js`), so improving a viewer improves old versions too.

## Storage

Under the same data directory Judas already uses (`data_dir`, default `~/.local/share/judas`):

```
artifacts/<id>/meta.json          title, kind, created/updated, session, version list with one-line notes and file lists
artifacts/<id>/v1/...             immutable once written
artifacts/<id>/v2/...             unchanged files are hard links to the previous version (copied when links fail)
artifacts/<id>/renders/v<N>.png   preview of version N; derived data, replaced when rendered again
```

- `<id>` is 10 lowercase hex characters.
- A change writes a temp directory and renames it into place, then rewrites `meta.json` atomically. A version is never
  modified. Versions are removed only when the whole artifact is deleted (`DELETE /api/artifacts/<id>`; there is no tool
  for it, only the UI).
- Limits: 64 files per version, 4 MB per text file the model sends inline, 64 MB per file copied from disk, 128 MB per
  version, 500 versions per artifact, 2000 artifacts, titles 120 characters.
- File paths are relative, letters, digits, `.`, `_`, `-` and `/` only; no `..`, no empty or hidden segments, and a first
  segment like `v3` is refused because it would collide with version URLs.

## Tools (lead agent only)

| tool | does |
| --- | --- |
| `artifact_create(title, kind, files, source, source_name, note)` | make an artifact, version 1. `files` maps path to text; `source` copies a file from disk (STL, GLB, images) through the normal read-path policy |
| `artifact_update(id, files, remove, source, title, note)` | new version: send only changed files, the rest carry forward |
| `artifact_read(id, version, path)` | files of a version and the text of one (default the main file) |
| `artifact_list(query)` | id, title, kind, version count, newest first |
| `artifact_render(id, version, width, height, views)` | PNG the model can look at (see below) |

Tool results carry a `[[artifact:ID|title|kind|N]]` marker; the desktop and phone chat turn it into a compact card with a
preview that opens the artifact. Workers, reviewers and background agents do not get these tools. They need no approval
because they only write inside Judas's own data directory and never delete.

## Rendering

`artifact_render` returns the picture as an image the model sees on its next step (the same `[[image:ID|name]]` route
as `view_image`) and saves it as the version's preview.

- model3d (STL binary or ASCII, GLB 2.0 with triangle meshes and node transforms): rasterized inside Judas. The default is
  four tiles, iso (top left), front (top right), right (bottom left), top (bottom right); `views` can name up to four of
  iso, front, back, left, right, top, bottom. Orthographic, each tile framed to the part, flat shading with a key and a
  fill light, dark lines at silhouettes and creases, small axis marks (X red, Y green, Z blue, Z up; GLB Y-up is converted).
  About 0.1 s for a 250 KB mesh. Meshes are capped at about 4 million triangles.
- svg: rasterized inside Judas with nanosvg on white.
- image: PNG files are returned as they are.
- html and markdown: captured client side. `artifact_render` publishes an `artifact.render` event on the global event feed.
  Any open Judas page (desktop or phone) loads that version in a hidden sandboxed iframe, asks the page (the server injects
  `capture.js` into every html artifact) for a PNG made with html-to-image, and POSTs it to
  `POST /api/artifacts/<id>/render-result?version=N`. The tool waits up to 25 s. With no page open it fails with a clear
  message plus the source text. The same path fills missing thumbnails when the Artifacts list opens. The sandboxed page
  never calls `/api` itself: it only answers `postMessage` from its parent, and the parent checks `event.source`.
  html-to-image cannot draw WebGL canvases or cross-origin resources, so those parts may be blank in a capture.
- Nothing here touches the GPU (the rasterizer is CPU only, the captures run in the user's browser).

## Serving and security model

- `GET /a/<id>/` serves the latest version, `/a/<id>/v<N>/` a specific one, and `/a/<id>/v<N>/<path>` a file. Only files
  listed in `meta.json` are served, paths are cleaned first, and ids are validated.
- Every artifact response carries `Content-Security-Policy: default-src 'none'; script-src 'self' 'unsafe-inline';
  style-src 'self' 'unsafe-inline'; img-src 'self' data: blob:; font-src 'self' data:; media-src 'self' data: blob:;
  connect-src 'self' data: blob:; worker-src blob:; frame-ancestors 'self'; base-uri 'self'; form-action 'none';
  sandbox allow-scripts`. There is no CDN or foreign origin. The CSP `sandbox` directive (without `allow-same-origin`)
  makes the page a null-origin document even when opened in its own tab; the app embeds it in an iframe that also has
  `sandbox="allow-scripts"`. No forms, popups, top navigation or storage.
- A null-origin page cannot call the API. The pre-routing guard answers 403 to any request outside `/a/` and
  `/vendor/artifacts/` that has `Origin: null` (a fetch from a sandboxed page) or is a cross-site tag load with no Origin
  (`<img src="/api/...">`, no-cors fetch). Reads of the page's own files get `Access-Control-Allow-Origin: null`; the
  vendored viewer libraries get `*` because ES modules from a null-origin page need CORS. The browser also withholds
  cookies from a null origin, and Judas has no cookie auth anyway; the guard is what keeps the unauthenticated API closed
  to artifact code.
- Artifact pages are the model's output: treat them as untrusted code with no network and no access to your chats.
  They can still draw, compute and burn CPU in your browser.

## API

| route | |
| --- | --- |
| `GET /api/artifacts?q=` | list, newest first, with version lists |
| `GET /api/artifacts/<id>` | one artifact |
| `DELETE /api/artifacts/<id>` | delete the whole artifact and all versions |
| `POST /api/artifacts/<id>/render` | `{version, width, height, views, wait_ms}`; model3d, svg and images render at once; html and markdown wait for a web page; 503 when none answers |
| `POST /api/artifacts/<id>/render-result?version=N` | raw PNG body from a web page; saved as the preview and answers a waiting `artifact_render` |
| `GET /api/artifacts/<id>/thumb?v=N` | preview PNG; model3d and svg draw one on first request; 404 for html and markdown until a capture exists |
| `GET /api/artifacts/<id>/download?v=N&path=` | a file as an attachment (default the main file) |

## UI

- Desktop: Artifacts in the Tools group of the rail. A grid of cards (preview, title, kind, age) with search; a viewer with
  a version picker, open in tab, download, refresh preview and delete (with confirm).
- Phone: Artifacts in the menu opens a list sheet; an artifact opens full screen in a sandboxed iframe with a version
  picker and a tab link. Android back closes it.
- Chat: a card under the tool result on both.

## Limits and open ends

- GLB: triangle meshes only, float positions, no Draco or sparse accessors; materials and textures are ignored in the
  rendered picture (the three.js viewer shows them). `.gltf` with external buffers is not accepted.
- html captures depend on html-to-image, so they are a good guide, not a pixel-exact browser screenshot.
- Tests: `tests/test_artifacts.cpp` (store, versions, paths, kinds, renderers, broker) and the artifact block in
  `tests/test_server.cpp` (sandbox headers, traversal, API, guard).
