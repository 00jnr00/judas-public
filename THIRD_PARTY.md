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

## Bundled web fonts (web/fonts/)

Sixteen font families ship inside the web app (Latin subset, woff2) so the UI works offline and on phones without a CDN.
All are licensed under the SIL Open Font License 1.1; each full license text is in `LICENSES/fonts/`. The woff2 files
come from the Fontsource npm packages (https://fontsource.org, which repackages the upstream releases) at the exact
versions below, fetched from https://cdn.jsdelivr.net/npm/. The OFL permits bundling and redistribution with software;
the fonts are not sold on their own and keep their names.

| Family | Copyright holder | License | Upstream | Package and version | License file | Files |
|---|---|---|---|---|---|---|
| Inter | The Inter Project Authors | OFL-1.1 | https://github.com/rsms/inter | @fontsource-variable/inter 5.3.0 | LICENSES/fonts/Inter.txt | web/fonts/inter*.woff2 |
| IBM Plex Sans | IBM Corp. | OFL-1.1 | https://github.com/IBM/plex | @fontsource/ibm-plex-sans 5.3.0 | LICENSES/fonts/IBM-Plex-Sans.txt | web/fonts/ibm-plex-sans-*.woff2 |
| Atkinson Hyperlegible | Braille Institute of America | OFL-1.1 | https://github.com/googlefonts/atkinson-hyperlegible | @fontsource/atkinson-hyperlegible 5.3.0 | LICENSES/fonts/Atkinson-Hyperlegible.txt | web/fonts/atkinson-hyperlegible-*.woff2 |
| Lexend | The Lexend Project Authors | OFL-1.1 | https://github.com/googlefonts/lexend | @fontsource-variable/lexend 5.3.0 | LICENSES/fonts/Lexend.txt | web/fonts/lexend.woff2 |
| Source Sans 3 | Adobe | OFL-1.1 | https://github.com/adobe-fonts/source-sans | @fontsource-variable/source-sans-3 5.3.1 | LICENSES/fonts/Source-Sans-3.txt | web/fonts/source-sans-3*.woff2 |
| Nunito Sans | The Nunito Sans Project Authors | OFL-1.1 | https://github.com/googlefonts/nunito | @fontsource-variable/nunito-sans 5.3.0 | LICENSES/fonts/Nunito-Sans.txt | web/fonts/nunito-sans*.woff2 |
| Space Grotesk | The Space Grotesk Project Authors | OFL-1.1 | https://github.com/floriankarsten/space-grotesk | @fontsource-variable/space-grotesk 5.3.0 | LICENSES/fonts/Space-Grotesk.txt | web/fonts/space-grotesk.woff2 |
| Literata | The Literata Project Authors | OFL-1.1 | https://github.com/googlefonts/literata | @fontsource-variable/literata 5.3.0 | LICENSES/fonts/Literata.txt | web/fonts/literata*.woff2 |
| Source Serif 4 | Adobe | OFL-1.1 | https://github.com/adobe-fonts/source-serif | @fontsource-variable/source-serif-4 5.3.1 | LICENSES/fonts/Source-Serif-4.txt | web/fonts/source-serif-4*.woff2 |
| Lora | The Lora Project Authors | OFL-1.1 | https://github.com/cyrealtype/Lora-Cyrillic | @fontsource-variable/lora 5.3.0 | LICENSES/fonts/Lora.txt | web/fonts/lora*.woff2 |
| JetBrains Mono | JetBrains | OFL-1.1 | https://github.com/JetBrains/JetBrainsMono | @fontsource-variable/jetbrains-mono 5.3.0 | LICENSES/fonts/JetBrains-Mono.txt | web/fonts/jetbrains-mono*.woff2 |
| IBM Plex Mono | IBM Corp. | OFL-1.1 | https://github.com/IBM/plex | @fontsource/ibm-plex-mono 5.3.0 | LICENSES/fonts/IBM-Plex-Mono.txt | web/fonts/ibm-plex-mono-*.woff2 |
| Fira Code | The Fira Code Project Authors | OFL-1.1 | https://github.com/tonsky/FiraCode | @fontsource-variable/fira-code 5.3.0 | LICENSES/fonts/Fira-Code.txt | web/fonts/fira-code.woff2 |
| Source Code Pro | Adobe | OFL-1.1 | https://github.com/adobe-fonts/source-code-pro | @fontsource-variable/source-code-pro 5.3.1 | LICENSES/fonts/Source-Code-Pro.txt | web/fonts/source-code-pro*.woff2 |
| Commit Mono | The Commit Mono Project Authors | OFL-1.1 | https://github.com/eigilnikolajsen/commit-mono | @fontsource/commit-mono 5.3.0 | LICENSES/fonts/Commit-Mono.txt | web/fonts/commit-mono-*.woff2 |
| Space Mono | Colophon Foundry | OFL-1.1 | https://github.com/googlefonts/spacemono | @fontsource/space-mono 5.3.0 | LICENSES/fonts/Space-Mono.txt | web/fonts/space-mono-*.woff2 |
