// Attachments: files a user drops into the chat (images, screenshots, PDFs, office docs, text/code).
// Stored once under <data_dir>/attachments/ by content hash. Documents are turned into text at ingest; images stay
// images and reach a vision model as ChatMessage::images. Contract: docs/ATTACHMENTS.md.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/config.h"
#include "core/json.h"
#include "engine/engine.h"

namespace judas {

struct Attachment {
    std::string id;          // 16 hex chars of the content sha256
    std::string name;        // original file name (sanitized, no slashes)
    std::string mime;        // best guess: image/png, application/pdf, text/plain, ...
    std::string kind;        // image | pdf | doc | text
    long long bytes = 0;
    std::string path;        // absolute path of the stored original
    std::string text;        // extracted text (pdf/doc/text; OCR text for images when tesseract exists), may be empty
    std::vector<std::string> pages;  // ids of page images (scanned PDFs rendered to PNG), kind image each
    bool truncated = false;  // text was cut at attachments.max_text_chars
};

class AttachmentStore {
public:
    explicit AttachmentStore(const Config & cfg);
    // Store bytes under their hash and extract text/pages. Same bytes -> same id (idempotent).
    // Throws std::runtime_error on an empty or oversized (> attachments.max_bytes) upload.
    Attachment ingest(const std::string & bytes, const std::string & filename, const std::string & mime = "");
    // Store a file already on disk (TUI /attach, tools). Throws if unreadable.
    Attachment ingest_file(const std::string & path);
    std::optional<Attachment> get(const std::string & id) const;
    // Original bytes of an attachment ("" when missing).
    std::string read_bytes(const std::string & id) const;
    std::string dir() const { return dir_; }

    // The text a user message carries for an attachment:
    //   image             -> "[[image:ID|name]]"
    //   pdf / doc / text  -> "<attachment name=\"..\" id=\"ID\" kind=\"..\">\n...text...\n</attachment>"
    //                        plus one "[[image:PAGEID|name p.N]]" line per rendered page when the PDF had no text
    std::string message_block(const Attachment & a) const;

private:
    const Config & cfg_;
    std::string dir_;
};

json attachment_json(const Attachment & a);   // {id,name,mime,kind,bytes,text_chars,truncated,pages,url}

// Markers in message text.
struct ImageRef {
    std::string id, name;
    size_t pos = 0, len = 0;   // byte span of the marker in the text
};
std::vector<ImageRef> find_image_markers(const std::string & text);

// Before a request: for every [[image:ID|name]] in m.content, load the bytes into m.images when `vision`;
// otherwise replace the marker with "[image: name, not shown: the loaded model has no vision]" plus the
// OCR text if the attachment has any. Markers stay in m.content when vision is on (the engine ignores them as text).
void expand_images(AttachmentStore & store, ChatMessage & m, bool vision);

// Request-wide expansion (what the agent calls before each engine request): walks messages newest to oldest and
// loads bytes for at most `max_images` markers (newest first); older markers become "[image: name, shown earlier]".
// Without vision every marker becomes the not-shown text. All roles are covered.
void expand_request_images(AttachmentStore & store, std::vector<ChatMessage> & msgs, bool vision, int max_images = 8);

// Run an external program with argv exec (no shell), stdout captured, stderr dropped. Returns the exit code,
// -1 on timeout, 127 when the program is missing or cannot start.
int run_tool(const std::vector<std::string> & argv, int timeout_s, std::string & out);
bool tool_available(const std::string & name);   // executable found on PATH
// Screenshot of the host screen as PNG bytes via grim (and slurp when `region`). Returns "" and sets `err` on
// failure; err == "no-grim" when grim is not installed, "cancelled" when the region pick was cancelled.
std::string capture_screenshot(bool region, std::string & err);
// "<id>" is exactly 16 lowercase hex chars.
bool valid_attachment_id(const std::string & id);

std::string base64_encode(const std::string & bytes);
std::string base64_decode(const std::string & b64);   // ignores whitespace; "" on invalid input

}  // namespace judas
