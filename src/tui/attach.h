// Attachments in the terminal UI: dropped/pasted path detection, pending-attachment chips, and collapsing of
// attachment markers in the transcript. Pure (path checks stat the filesystem), unit tested in
// tests/test_tui_attach.cpp. The HTTP contract is docs/ATTACHMENTS.md.
#pragma once
#include <string>
#include <vector>

#include "tui/text.h"

namespace judas::tui {

// One uploaded attachment waiting to be sent with the next message.
struct PendingAttachment {
    std::string id, name, kind;  // kind: image pdf doc text
    long long bytes = 0, text_chars = 0;
};

// A transcript line after collapsing: chip lines are drawn dim.
struct DisplayLine {
    std::string text;
    bool chip = false;
};
// Message text to display rows: [[image:ID|name]] becomes a "🖼 name" chip and an <attachment ...>...</attachment>
// block becomes "📎 name (kind, N chars)"; the attachment text itself is never shown.
std::vector<DisplayLine> collapse_attachments(const std::string & text);

// Split a line the way a shell would: whitespace separates, '..' and ".." quote, backslash escapes.
std::vector<std::string> shell_split(const std::string & s);
std::string percent_decode(const std::string & s);
std::string url_encode(const std::string & s);
// One token to an absolute file path: handles file:// URIs, ~, and (when !strict) relative paths. strict requires
// the token to look like a path (starts with / ~/ ./ ../ or file://). Returns "" when it is not an existing file.
std::string resolve_file(const std::string & token, const std::string & cwd, bool strict);
// Text that is exactly one or more existing file paths (a dropped file, quoted/escaped/URI forms); empty otherwise.
std::vector<std::string> detect_paths(const std::string & text, const std::string & cwd);
// Arguments of /attach: every token must resolve to an existing file; bad ones are listed in `missing`.
std::vector<std::string> parse_attach_args(const std::string & arg, const std::string & cwd, std::vector<std::string> * missing);

std::string human_bytes(long long n);  // 2.1 MB
std::string guess_mime(const std::string & filename);
std::string chip_text(const PendingAttachment & a, int index);  // "[1] 📎 report.pdf 2.1 MB" / "[2] 🖼 photo.png"
// Pending chips wrapped into rows of at most `width` columns.
std::vector<std::string> render_chip_rows(const std::vector<PendingAttachment> & p, int width, const Palette & pal);

}  // namespace judas::tui
