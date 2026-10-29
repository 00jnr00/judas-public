# Attachments and vision (contract)

Users attach images, screenshots, PDFs, office documents and text/code files in the chat bar (web and TUI).
The model sees documents as text and images as images (when the loaded model has vision).

## Storage

`<data_dir>/attachments/` holds each upload once, named by content hash: `<id>.<ext>` (original bytes),
`<id>.json` (metadata, `attachment_json` shape plus `path`), `<id>.txt` (extracted text, when any).
`id` = first 16 hex chars of the sha256 of the bytes. Same bytes, same id.

Ingest by kind (picked from extension, then mime, then magic bytes):

| kind  | inputs | what is extracted |
|-------|--------|-------------------|
| image | png jpg jpeg gif bmp webp | nothing; OCR text via `tesseract <file> - ` when installed and `attachments.ocr` |
| pdf   | pdf | `pdftotext -layout`; when it yields < 40 chars per page the PDF is treated as scanned: `pdftoppm -png -r 110 -l pdf_max_pages` renders pages, each page is ingested as its own image attachment (`pages`) |
| doc   | docx xlsx pptx odt ods odp rtf epub html | `markitdown <file>` if on PATH, else `pandoc -t plain` if on PATH, else docx/odt/pptx/xlsx by unzipping the XML and stripping tags |
| text  | anything else that is valid UTF-8 (txt md csv json code ...) | the text itself |

External tools are optional and run with a 60 s timeout, no shell (argv exec). Missing tool = empty text,
never an error. Text is cut at `attachments.max_text_chars` (head kept, `truncated: true`).

## Message text

A user message carries its attachments inline:

- image: `[[image:ID|name]]` on its own line
- pdf / doc / text: `<attachment name="report.pdf" id="ID" kind="pdf">` + newline + text + newline + `</attachment>`
- scanned PDF: the attachment block (text may be empty) followed by one `[[image:PAGEID|report.pdf p.1]]` line per page

These are stored verbatim in the session history. UIs render them as chips (documents, collapsible) and
thumbnails (images). Before each engine request the agent calls `expand_images()` on every message: with vision,
the bytes go to `ChatMessage::images` (the engine places them at the start of the message); without vision,
the marker becomes `[image: name, not shown: the loaded model has no vision]` plus OCR text.

## HTTP API

- `POST /api/attachments?name=<filename>` body = raw file bytes (`Content-Type` = file mime). Also accepts
  `multipart/form-data` with field `file`. Returns `attachment_json`:
  `{"id","name","mime","kind","bytes","text_chars","truncated","pages":[ids],"url":"/api/attachments/ID"}`.
  413 over `attachments.max_bytes`, 400 empty.
- `GET /api/attachments/<id>` original bytes with its mime (inline). `GET /api/attachments/<id>?meta=1` the json.
- `POST /api/sessions/<id|new>/messages` gains optional `"attachments": ["ID", ...]`. The server appends
  `message_block()` of each to `content` (content may then be empty when attachments are present). New sessions with
  empty text are titled after the first attachment name.
- `POST /api/screenshot` (body `{"region": false}`) takes a screenshot on the host with `grim` (region with
  `slurp` when `region` true), ingests it and returns its `attachment_json`. 501 when grim is missing.
- `GET /api/state` gains `"vision": bool` and `"ui": {"mascot": bool}`.

## Engine

`ModelConfig.mmproj` ("" = auto: an `mmproj*.gguf` in the model's folder; "none" = off), `mmproj_gpu`,
`image_max_tokens`; remote models: `vision = true` sends images as OpenAI `image_url` data URLs.
`Engine::vision()` says whether images are used. `/v1/chat/completions` accepts `image_url` parts with
`data:` URLs (http URLs are ignored).

## Tools

- `view_image {path}`: loads an image file (png/jpg/gif/bmp/webp) under the allowed paths and returns it to the
  model as an image (tool result carries `ChatMessage::images`). Without vision it returns OCR text.
- `screenshot {region?: bool}`: grabs the screen like `/api/screenshot` and returns it as an image. Asks for
  approval like other side-effect tools outside full-auto.
