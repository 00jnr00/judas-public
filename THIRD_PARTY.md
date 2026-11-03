# Third-party notices

## Hermes Agent

The learning loop (background review prompts, the `/learn` prompt, the skill curator design and parts of the
skills guard patterns) is adapted from Hermes Agent by Nous Research, used under the MIT License:

    MIT License

    Copyright (c) 2025 Nous Research

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## Linked at build time (not bundled in this repository)

- llama.cpp and ggml (MIT), including its vendored cpp-httplib (MIT), nlohmann/json (MIT) and sha256 code
- toml++ (MIT)
- SQLite (public domain)
- OpenSSL (Apache 2.0)

## KaTeX

The web app renders math with KaTeX 0.16.11 (`web/vendor/katex/`: katex.min.js, katex.min.css and the woff2
fonts, served from the Judas binary, no network at runtime), used under the MIT License:

    The MIT License (MIT)

    Copyright (c) 2013-2020 Khan Academy and other contributors

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

## Strata

The optional Strata backend (`-DJUDAS_STRATA=ON`, `backend = "strata"`, see docs/STRATA-BACKEND.md) starts the
Strata inference engine (https://github.com/Niko1221/Strata) as a separate process and speaks its stdin/stdout
request protocol. The engine is compiled into the judas binary from a Strata checkout at build time (nothing is
copied into this repository), together with Strata's expert profile data file (data/expert-profile.bin) and its
pinned ggml. Strata is used under the MIT License:

    MIT License

    Copyright (c) Niko1221 and the Strata contributors

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE.

The Strata model files (Qwen3.8-Flash-Next, Apache-2.0) keep their own licenses.

## OpenRig (Swarm screen: Map, Table, motion)

Parts of the Swarm screen are ported or adapted from OpenRig (https://github.com/mvschwarz/openrig), Copyright 2026
Mike Schwarz, used under the Apache License 2.0 (full text in LICENSES/Apache-2.0.txt). The code was translated from
React, Tailwind and React Flow into vanilla JavaScript and plain CSS and changed to use Judas skin variables and
Judas agent activities, so it is modified from the original. What was taken, and where it lives:

- Activity motion keyframes and their timings (activity-ring-*, activity-card-*, activity-pulse-running, hot-potato-flow,
  node-enter, edge-draw-in) from packages/ui/src/globals.css: web/app.css, "Motion" block.
- Hot-potato edge (a packet travelling a hand-off edge by SVG animateMotion, plus the marching dashed overlay) from
  packages/ui/src/components/topology/HotPotatoEdge.tsx: `tmHot` in web/app.js, `.tm-packet` and `.tm-flow` in web/app.css.
- Agent card anatomy (header bar, inverted header for lead roles, activity dot, runtime, context percent colored by band,
  token total), rig frame with counts and dashed pod frames, ActivityRing: from HybridTopologyNodes.tsx, RigGroupNode.tsx
  and ActivityRing.tsx: `tmMapCard`, `tmUpdateCard`, `tmBuildMap` in web/app.js, `.tmc-*` and `.tm-rig`, `.tm-pod` in web/app.css.
- Layout rule (pods as columns, members stacked and delegation read top to bottom) and the four glyph vocabulary with
  edge colors by kind, from packages/tui/src/topology/layout.ts and glyphs.ts: `tmLayout`, `tmRows`, `tmGlyph`.
- View mode tabs (Map, Panes, Table, File) from TopologyViewModeTabs.tsx: `tmRenderViewTabs`, `.tm-vtab`.
- Table view, sortable columns and the active-row shimmer from TopologyTableView.tsx and topology-table-shimmer.css:
  `tmRenderTable`, `.tm-table`, `.tm-shimmer`.
- Smoked glass output preview from TerminalPreviewPopover.tsx (static text only): `tmShowPop`, `.tm-pop`.

## herdr (Swarm screen: Panes)

The Panes view follows the layout of herdr (https://github.com/herdrdev/herdr), used under the Apache License 2.0
(LICENSES/Apache-2.0.txt). herdr is a Rust terminal application; nothing was compiled or copied as code. The visual
design was adapted into HTML and CSS from src/ui/sidebar.rs, src/ui/panes.rs, src/pane/agent_detection.rs and
assets/screenshot.png, so it is modified from the original:

- Agent list with a status glyph, the name, and the state word right-aligned (working, waiting, blocked, idle, done),
  blocked and failed sorted to the top and highlighted: `tmRenderSide`, `.tms-*` in web/app.css.
- Large panes with a bright accent border on the focused pane and a dim border on the others: `.tpane`, `.tpane.focused`.
- The status line under each pane (folder > model > context bar and percent > tok/s > tokens): `tmPaneStatus`, `.tp-status`.
