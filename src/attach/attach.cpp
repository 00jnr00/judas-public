// Attachments: content-addressed storage, text extraction, image markers. Contract: docs/ATTACHMENTS.md.
#include "attach/attach.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <sstream>

extern "C" {
#include "sha256/sha256.h"
}

namespace judas {
namespace fs = std::filesystem;

namespace {

constexpr int TOOL_TIMEOUT_S = 60;

std::string hex_of(const unsigned char * d, size_t n) {
    static const char * k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += k[d[i] >> 4]; s += k[d[i] & 15]; }
    return s;
}

std::string content_id(const std::string & data) {
    sha256_t c;
    sha256_init(&c);
    sha256_update(&c, reinterpret_cast<const unsigned char *>(data.data()), data.size());
    unsigned char dg[SHA256_DIGEST_SIZE];
    sha256_final(&c, dg);
    return hex_of(dg, sizeof dg).substr(0, 16);
}

std::string lower(std::string s) {
    for (auto & c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

bool read_file_bytes(const fs::path & p, std::string & out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

// Write via a temp file and rename, so a reader never sees half a file.
bool write_atomic(const fs::path & p, const std::string & data) {
    fs::path tmp = p;
    static std::atomic<unsigned> seq{0};
    tmp += ".tmp" + std::to_string(getpid()) + "-" + std::to_string(seq++);
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize) data.size());
        if (!f) { f.close(); std::error_code e; fs::remove(tmp, e); return false; }
    }
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

// File name as shown in markers and attributes: no directories, no characters that break "[[image:ID|name]]"
// or an XML-ish attribute, at most 120 bytes, never empty.
std::string sanitize_name(const std::string & in) {
    std::string base = in;
    for (auto & c : base) if (c == '\\') c = '/';
    if (auto sl = base.rfind('/'); sl != std::string::npos) base = base.substr(sl + 1);
    std::string out;
    for (unsigned char c : base) {
        if (c < 0x20 || c == 0x7f || c == '|' || c == '[' || c == ']' || c == '"' || c == '<' || c == '>') out += '_';
        else out += (char) c;
    }
    if (out.size() > 120) {
        size_t cut = 120;
        while (cut > 0 && ((unsigned char) out[cut] & 0xC0) == 0x80) cut--;   // do not split a UTF-8 sequence
        out = out.substr(0, cut);
    }
    size_t a = out.find_first_not_of(" .");
    if (a == std::string::npos) return "file";
    return out.substr(a);
}

std::string ext_of(const std::string & name) {
    auto dot = name.rfind('.');
    if (dot == std::string::npos || dot + 1 >= name.size()) return "";
    std::string e = lower(name.substr(dot + 1));
    if (e.size() > 8) return "";
    for (char c : e) if (!std::isalnum((unsigned char) c)) return "";
    return e;
}

bool valid_utf8(const std::string & s) {
    size_t i = 0, n = s.size();
    while (i < n) {
        unsigned char c = (unsigned char) s[i];
        int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!len || i + len > n) return false;
        if (c == 0) return false;
        for (int k = 1; k < len; k++) if (((unsigned char) s[i + k] & 0xC0) != 0x80) return false;
        i += len;
    }
    return true;
}

size_t utf8_chars(const std::string & s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

const std::set<std::string> IMAGE_EXT = {"png", "jpg", "jpeg", "gif", "bmp", "webp"};
const std::set<std::string> DOC_EXT = {"docx", "xlsx", "pptx", "odt", "ods", "odp", "rtf", "epub", "html", "htm"};

std::string mime_for_ext(const std::string & e) {
    static const std::map<std::string, std::string> m = {
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"},
        {"bmp", "image/bmp"}, {"webp", "image/webp"}, {"pdf", "application/pdf"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"odt", "application/vnd.oasis.opendocument.text"}, {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
        {"odp", "application/vnd.oasis.opendocument.presentation"}, {"rtf", "application/rtf"},
        {"epub", "application/epub+zip"}, {"html", "text/html"}, {"htm", "text/html"},
        {"md", "text/markdown"}, {"csv", "text/csv"}, {"json", "application/json"}, {"txt", "text/plain"}};
    auto it = m.find(e);
    return it == m.end() ? "" : it->second;
}

// Magic bytes: returns an extension ("png", "jpg", "gif", "webp", "pdf", "zip") or "".
std::string sniff(const std::string & b) {
    auto starts = [&](const char * p, size_t n) { return b.size() >= n && std::memcmp(b.data(), p, n) == 0; };
    if (starts("\x89PNG\r\n\x1a\n", 8)) return "png";
    if (starts("\xff\xd8\xff", 3)) return "jpg";
    if (starts("GIF87a", 6) || starts("GIF89a", 6)) return "gif";
    if (b.size() >= 12 && std::memcmp(b.data(), "RIFF", 4) == 0 && std::memcmp(b.data() + 8, "WEBP", 4) == 0) return "webp";
    if (starts("%PDF-", 5)) return "pdf";
    if (starts("PK\x03\x04", 4)) return "zip";
    return "";
}

std::string find_on_path(const std::string & name) {
    if (name.find('/') != std::string::npos) return access(name.c_str(), X_OK) == 0 ? name : "";
    const char * path = std::getenv("PATH");
    std::string p = path ? path : "/usr/bin:/bin";
    size_t s = 0;
    while (s <= p.size()) {
        size_t e = p.find(':', s);
        if (e == std::string::npos) e = p.size();
        std::string dir = p.substr(s, e - s);
        if (!dir.empty()) {
            std::string full = dir + "/" + name;
            std::error_code ec;
            if (fs::is_regular_file(full, ec) && access(full.c_str(), X_OK) == 0) return full;
        }
        s = e + 1;
    }
    return "";
}

// Strip XML/HTML tags; </w:p>, </a:p>, <br>, </p>, </text:p>, </row> style closers become newlines.
std::string strip_tags(const std::string & x) {
    std::string out;
    size_t i = 0;
    auto unent = [](std::string s) {
        static const std::pair<const char *, const char *> ents[] = {
            {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&#39;", "'"}, {"&nbsp;", " "}, {"&amp;", "&"}};
        for (auto & [k, v] : ents) {
            size_t pos = 0;
            std::string ks = k;
            while ((pos = s.find(ks, pos)) != std::string::npos) { s.replace(pos, ks.size(), v); pos += std::strlen(v); }
        }
        return s;
    };
    std::string text;
    while (i < x.size()) {
        if (x[i] == '<') {
            size_t e = x.find('>', i);
            if (e == std::string::npos) break;
            std::string tag = lower(x.substr(i + 1, e - i - 1));
            bool brk = tag == "br" || tag == "br/" || tag == "/p" || tag == "/w:p" || tag == "/a:p" || tag == "/text:p" ||
                       tag == "/text:h" || tag == "/tr" || tag == "/li" || tag == "/div" || tag == "/row" ||
                       tag == "/si" || tag == "/h1" || tag == "/h2" || tag == "/h3" || tag.rfind("/table:table-row", 0) == 0;
            bool tab = tag == "w:tab/" || tag == "/td" || tag == "/th" || tag == "/c";
            if (tag.rfind("script", 0) == 0 || tag.rfind("style", 0) == 0) {   // drop the whole element body
                size_t close = lower(x).find(tag.substr(0, tag.find(' ')) == "script" ? "</script>" : "</style>", e);
                if (close != std::string::npos) { i = x.find('>', close) + 1; continue; }
            }
            if (brk) text += '\n';
            else if (tab) text += '\t';
            i = e + 1;
        } else {
            text += x[i++];
        }
    }
    out = unent(text);
    // collapse runs of blank lines
    std::string clean;
    int nl = 0;
    for (char c : out) {
        if (c == '\n') { if (++nl <= 2) clean += c; }
        else if (c == '\r') {}
        else { nl = 0; clean += c; }
    }
    return clean;
}

std::string trim(const std::string & s) {
    size_t a = s.find_first_not_of(" \t\r\n\f");
    if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n\f");
    return s.substr(a, b - a + 1);
}

// Text out of an office/zip document without helper tools: unzip the XML parts and strip tags.
std::string unzip_text(const std::string & file, const std::string & ext) {
    if (!tool_available("unzip")) return "";
    std::vector<std::string> members;
    if (ext == "docx") members = {"word/document.xml"};
    else if (ext == "odt" || ext == "ods" || ext == "odp") members = {"content.xml"};
    else if (ext == "xlsx") members = {"xl/sharedStrings.xml"};
    else if (ext == "pptx") {
        std::string list;
        run_tool({"unzip", "-Z1", file}, TOOL_TIMEOUT_S, list);
        std::vector<std::pair<int, std::string>> slides;
        std::istringstream is(list);
        std::string l;
        while (std::getline(is, l)) {
            if (l.rfind("ppt/slides/slide", 0) == 0 && l.size() > 4 && l.substr(l.size() - 4) == ".xml")
                slides.push_back({std::atoi(l.c_str() + 16), l});
        }
        std::sort(slides.begin(), slides.end());
        for (auto & s : slides) members.push_back(s.second);
    } else if (ext == "epub") {
        std::string list;
        run_tool({"unzip", "-Z1", file}, TOOL_TIMEOUT_S, list);
        std::istringstream is(list);
        std::string l;
        while (std::getline(is, l)) {
            std::string e = ext_of(l);
            if (e == "xhtml" || e == "html" || e == "htm") members.push_back(l);
        }
    }
    std::string all;
    int n = 0;
    for (auto & m : members) {
        std::string xml;
        run_tool({"unzip", "-p", file, m}, TOOL_TIMEOUT_S, xml);
        std::string t = trim(strip_tags(xml));
        if (t.empty()) continue;
        if (ext == "pptx") all += "[slide " + std::to_string(++n) + "]\n";
        all += t + "\n\n";
    }
    return trim(all);
}

}  // namespace

// ---------- process helpers ----------

int run_tool(const std::vector<std::string> & argv, int timeout_s, std::string & out) {
    out.clear();
    if (argv.empty()) return 127;
    std::string exe = find_on_path(argv[0]);
    if (exe.empty()) return 127;
    int fds[2];
    if (pipe(fds) != 0) return 127;
    std::vector<char *> av;
    for (auto & a : argv) av.push_back(const_cast<char *>(a.c_str()));
    av.push_back(nullptr);
    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return 127; }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(fds[1], 1);
        int dn = open("/dev/null", O_RDWR);
        if (dn >= 0) { dup2(dn, 2); dup2(dn, 0); }
        close(fds[0]); close(fds[1]);
        execv(exe.c_str(), av.data());
        _exit(127);
    }
    setpgid(pid, pid);
    close(fds[1]);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_s);
    const size_t hard_limit = 64u << 20;
    char buf[16384];
    int status = 0;
    bool exited = false, timed_out = false;
    while (true) {
        struct pollfd pf{fds[0], POLLIN, 0};
        int pr = poll(&pf, 1, 100);
        if (pr > 0) {
            ssize_t n = read(fds[0], buf, sizeof buf);
            if (n > 0) { if (out.size() < hard_limit) out.append(buf, (size_t) n); continue; }
            if (n == 0) break;
        }
        if (!exited && waitpid(pid, &status, WNOHANG) == pid) exited = true;
        if (exited && pr <= 0) break;
        if (std::chrono::steady_clock::now() > deadline) {
            timed_out = true;
            kill(-pid, SIGKILL);
            kill(pid, SIGKILL);
            break;
        }
    }
    close(fds[0]);
    if (!exited) waitpid(pid, &status, 0);
    if (timed_out) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 128 + (WIFSIGNALED(status) ? WTERMSIG(status) : 0);
}

bool tool_available(const std::string & name) { return !find_on_path(name).empty(); }

bool valid_attachment_id(const std::string & id) {
    if (id.size() != 16) return false;
    for (char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

std::string capture_screenshot(bool region, std::string & err) {
    err.clear();
    if (!tool_available("grim")) { err = "no-grim"; return ""; }
    std::vector<std::string> argv = {"grim"};
    if (region) {
        if (!tool_available("slurp")) { err = "slurp is not installed (needed for region capture)"; return ""; }
        std::string geom;
        int rc = run_tool({"slurp"}, TOOL_TIMEOUT_S, geom);
        geom = trim(geom);
        if (rc != 0 || geom.empty()) { err = "cancelled"; return ""; }
        argv.push_back("-g");
        argv.push_back(geom);
    }
    argv.push_back("-");
    std::string png;
    int rc = run_tool(argv, TOOL_TIMEOUT_S, png);
    if (rc != 0 || sniff(png) != "png") {
        err = "grim failed (is a Wayland session reachable from the server? exit " + std::to_string(rc) + ")";
        return "";
    }
    return png;
}

// ---------- base64 ----------

std::string base64_encode(const std::string & b) {
    static const char * t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string o;
    o.reserve((b.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < b.size(); i += 3) {
        unsigned v = ((unsigned char) b[i] << 16) | ((unsigned char) b[i + 1] << 8) | (unsigned char) b[i + 2];
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += t[v & 63];
    }
    if (i + 1 == b.size()) {
        unsigned v = (unsigned char) b[i] << 16;
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += "==";
    } else if (i + 2 == b.size()) {
        unsigned v = ((unsigned char) b[i] << 16) | ((unsigned char) b[i + 1] << 8);
        o += t[v >> 18]; o += t[(v >> 12) & 63]; o += t[(v >> 6) & 63]; o += '=';
    }
    return o;
}

std::string base64_decode(const std::string & s) {
    std::string o;
    unsigned v = 0;
    int bits = 0, pad = 0;
    for (unsigned char c : s) {
        if (std::isspace(c)) continue;
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else if (c == '=') { pad++; continue; }
        else return "";
        if (pad) return "";   // data after padding
        v = (v << 6) | (unsigned) d;
        bits += 6;
        if (bits >= 8) { bits -= 8; o += (char) ((v >> bits) & 0xFF); }
    }
    return o;
}

// ---------- markers ----------

std::vector<ImageRef> find_image_markers(const std::string & text) {
    std::vector<ImageRef> out;
    static const std::string open = "[[image:";
    size_t i = 0;
    while ((i = text.find(open, i)) != std::string::npos) {
        size_t idb = i + open.size();
        size_t bar = text.find('|', idb);
        if (bar == std::string::npos) break;
        std::string id = text.substr(idb, bar - idb);
        size_t end = text.find("]]", bar + 1);
        if (end == std::string::npos) break;
        std::string name = text.substr(bar + 1, end - bar - 1);
        bool ok = valid_attachment_id(id) && name.find('\n') == std::string::npos && name.find("[[") == std::string::npos;
        if (ok) {
            out.push_back({id, name, i, end + 2 - i});
            i = end + 2;
        } else {
            i += open.size();
        }
    }
    return out;
}

// ---------- store ----------

AttachmentStore::AttachmentStore(const Config & cfg) : cfg_(cfg) {
    dir_ = (fs::path(expand_home(cfg.data_dir)) / "attachments").string();
}

json attachment_json(const Attachment & a) {
    return {{"id", a.id}, {"name", a.name}, {"mime", a.mime}, {"kind", a.kind}, {"bytes", a.bytes},
            {"text_chars", (long long) utf8_chars(a.text)}, {"truncated", a.truncated}, {"pages", a.pages},
            {"url", "/api/attachments/" + a.id}};
}

std::optional<Attachment> AttachmentStore::get(const std::string & id) const {
    if (!valid_attachment_id(id)) return std::nullopt;
    std::string raw;
    if (!read_file_bytes(fs::path(dir_) / (id + ".json"), raw)) return std::nullopt;
    json j = json::parse(raw, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    Attachment a;
    a.id = id;
    a.name = j.value("name", "file");
    a.mime = j.value("mime", "application/octet-stream");
    a.kind = j.value("kind", "text");
    a.bytes = j.value("bytes", 0LL);
    a.truncated = j.value("truncated", false);
    if (j.contains("pages") && j["pages"].is_array())
        for (auto & p : j["pages"]) if (p.is_string() && valid_attachment_id(p.get<std::string>())) a.pages.push_back(p);
    // The stored file name is rebuilt from the id; a tampered "file" field can never leave the directory.
    std::string file = j.value("file", "");
    if (file.rfind(id + ".", 0) != 0 || file.find('/') != std::string::npos || file.find("..") != std::string::npos) return std::nullopt;
    a.path = (fs::path(dir_) / file).string();
    read_file_bytes(fs::path(dir_) / (id + ".txt"), a.text);
    return a;
}

std::string AttachmentStore::read_bytes(const std::string & id) const {
    auto a = get(id);
    std::string b;
    if (a) read_file_bytes(a->path, b);
    return b;
}

namespace {

std::string cut_text(std::string t, int max_chars, bool & truncated) {
    truncated = false;
    if (max_chars <= 0 || t.size() <= (size_t) max_chars) return t;
    size_t cut = (size_t) max_chars;
    while (cut > 0 && ((unsigned char) t[cut] & 0xC0) == 0x80) cut--;
    truncated = true;
    return t.substr(0, cut);
}

std::string ocr_of(const Config & cfg, const std::string & file) {
    if (!cfg.attachments.ocr || !tool_available("tesseract")) return "";
    std::string out;
    if (run_tool({"tesseract", file, "-"}, TOOL_TIMEOUT_S, out) != 0) return "";
    return trim(out);
}

}  // namespace

Attachment AttachmentStore::ingest(const std::string & bytes, const std::string & filename, const std::string & mime_in) {
    if (bytes.empty()) throw std::runtime_error("empty upload");
    if ((long long) bytes.size() > cfg_.attachments.max_bytes)
        throw std::runtime_error("upload is larger than attachments.max_bytes (" + std::to_string(cfg_.attachments.max_bytes) + ")");
    std::string name = sanitize_name(filename.empty() ? "upload" : filename);
    std::string id = content_id(bytes);
    fs::create_directories(dir_);
    if (auto have = get(id)) {
        // Same bytes: keep the first stored record, but a pages/name lookup stays stable.
        return *have;
    }

    // Kind: extension, then mime, then magic bytes.
    std::string ext = ext_of(name);
    std::string magic = sniff(bytes);
    std::string kind;
    if (IMAGE_EXT.count(ext)) kind = "image";
    else if (ext == "pdf") kind = "pdf";
    else if (DOC_EXT.count(ext)) kind = "doc";
    if (kind.empty()) {
        std::string m = lower(mime_in);
        if (m.rfind("image/", 0) == 0 && m != "image/svg+xml") kind = "image";
        else if (m == "application/pdf") kind = "pdf";
    }
    if (kind.empty()) {
        if (magic == "png" || magic == "jpg" || magic == "gif" || magic == "webp") kind = "image";
        else if (magic == "pdf") kind = "pdf";
        else if (valid_utf8(bytes)) kind = "text";
        else kind = "doc";   // unknown binary: stored, no text
    }
    // A sniffed image type beats a wrong extension for the stored name's extension and mime.
    std::string store_ext = ext;
    std::string mime = mime_for_ext(ext);
    if (kind == "image" && !magic.empty() && magic != "pdf" && magic != "zip") {
        if (ext != magic && !(ext == "jpeg" && magic == "jpg")) { store_ext = magic; mime = mime_for_ext(magic); }
    }
    if (kind == "pdf") { store_ext = "pdf"; mime = "application/pdf"; }
    if (mime.empty()) {
        std::string m = lower(mime_in);
        if (!m.empty() && m != "application/octet-stream") mime = m.substr(0, m.find(';'));
    }
    if (mime.empty()) mime = kind == "text" ? "text/plain" : kind == "image" && !magic.empty() ? mime_for_ext(magic) : "application/octet-stream";
    if (store_ext.empty()) store_ext = !magic.empty() ? (magic == "zip" ? "bin" : magic) : (kind == "text" ? "txt" : "bin");
    if (store_ext == "json" || store_ext == "txt" || store_ext == "tmp") store_ext = "dat";   // keep <id>.json/.txt for our own files

    Attachment a;
    a.id = id;
    a.name = name;
    a.mime = mime;
    a.kind = kind;
    a.bytes = (long long) bytes.size();
    std::string file = id + "." + store_ext;
    a.path = (fs::path(dir_) / file).string();
    if (!write_atomic(a.path, bytes)) throw std::runtime_error("cannot write " + a.path);

    std::string text;
    if (kind == "text") {
        text = bytes;
    } else if (kind == "image") {
        text = ocr_of(cfg_, a.path);
    } else if (kind == "pdf") {
        std::string out;
        if (run_tool({"pdftotext", "-layout", a.path, "-"}, TOOL_TIMEOUT_S, out) == 0) {
            size_t pages = (size_t) std::count(out.begin(), out.end(), '\f');
            size_t visible = 0;
            for (unsigned char c : out) if (!std::isspace(c)) visible++;
            for (auto & c : out) if (c == '\f') c = '\n';
            text = trim(out);
            if (pages > 0 && visible < 40 * pages && cfg_.attachments.pdf_max_pages > 0 && tool_available("pdftoppm")) {
                // Scanned: render the first pages to PNG and ingest each as an image attachment.
                fs::path tmp = fs::path(dir_) / ("render-" + id + "-" + std::to_string(getpid()));
                std::error_code ec;
                fs::create_directories(tmp, ec);
                std::string ignore;
                run_tool({"pdftoppm", "-png", "-r", "110", "-l", std::to_string(cfg_.attachments.pdf_max_pages), a.path,
                          (tmp / "p").string()}, TOOL_TIMEOUT_S, ignore);
                std::vector<fs::path> pngs;
                for (auto & e : fs::directory_iterator(tmp, ec))
                    if (e.path().extension() == ".png") pngs.push_back(e.path());
                std::sort(pngs.begin(), pngs.end(), [](const fs::path & x, const fs::path & y) {
                    std::string a1 = x.filename().string(), b1 = y.filename().string();
                    return a1.size() != b1.size() ? a1.size() < b1.size() : a1 < b1;   // p-2 before p-10
                });
                int n = 0;
                for (auto & p : pngs) {
                    std::string pb;
                    if (!read_file_bytes(p, pb) || pb.empty()) continue;
                    try {
                        Attachment pg = ingest(pb, name + " p." + std::to_string(++n), "image/png");
                        a.pages.push_back(pg.id);
                    } catch (...) {}
                }
                fs::remove_all(tmp, ec);
            }
        }
    } else if (kind == "doc") {
        if (ext == "html" || ext == "htm") {
            text = trim(strip_tags(bytes));
        } else if (tool_available("markitdown")) {
            std::string out;
            if (run_tool({"markitdown", a.path}, TOOL_TIMEOUT_S, out) == 0) text = trim(out);
        }
        if (text.empty() && ext != "html" && ext != "htm" && tool_available("pandoc")) {
            std::string out;
            if (run_tool({"pandoc", "-t", "plain", a.path}, TOOL_TIMEOUT_S, out) == 0) text = trim(out);
        }
        if (text.empty() && ext != "html" && ext != "htm") text = unzip_text(a.path, ext);
    }
    if (!valid_utf8(text)) {   // tools can emit odd bytes; keep only a clean prefix so JSON and the model never choke
        std::string clean;
        for (size_t i = 0; i < text.size();) {
            unsigned char c = (unsigned char) text[i];
            int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
            bool ok = len && i + len <= text.size();
            for (int k = 1; ok && k < len; k++) ok = ((unsigned char) text[i + k] & 0xC0) == 0x80;
            if (ok && c != 0) { clean.append(text, i, len); i += len; }
            else i++;
        }
        text = clean;
    }
    a.text = cut_text(text, cfg_.attachments.max_text_chars, a.truncated);
    if (!a.text.empty()) write_atomic(fs::path(dir_) / (id + ".txt"), a.text);

    json meta = attachment_json(a);
    meta["file"] = file;
    meta["path"] = a.path;
    // The metadata file is written last: its existence means the record is complete.
    if (!write_atomic(fs::path(dir_) / (id + ".json"), meta.dump())) throw std::runtime_error("cannot write metadata");
    return a;
}

Attachment AttachmentStore::ingest_file(const std::string & path) {
    std::string bytes;
    if (!read_file_bytes(path, bytes)) throw std::runtime_error("cannot read " + path);
    return ingest(bytes, fs::path(path).filename().string());
}

std::string AttachmentStore::message_block(const Attachment & a) const {
    std::string name = sanitize_name(a.name);
    if (a.kind == "image") return "[[image:" + a.id + "|" + name + "]]";
    std::string s = "<attachment name=\"" + name + "\" id=\"" + a.id + "\" kind=\"" + a.kind + "\">\n" + a.text;
    if (a.truncated) s += "\n[... text cut at the attachment size limit ...]";
    s += "\n</attachment>";
    for (auto & pid : a.pages) {
        auto pg = get(pid);
        std::string pn = pg ? sanitize_name(pg->name) : name;
        s += "\n[[image:" + pid + "|" + pn + "]]";
    }
    return s;
}

// ---------- expansion ----------

namespace {

// Expand one message. `budget` = how many more images may load (decremented). Markers are visited last to
// first so the budget goes to the newest ones; text edits run back to front so spans stay valid.
void expand_one(AttachmentStore & store, ChatMessage & m, bool vision, int & budget) {
    if (m.content.find("[[image:") == std::string::npos) return;
    auto refs = find_image_markers(m.content);
    std::vector<std::string> loaded;   // newest first
    for (size_t k = refs.size(); k-- > 0;) {
        const ImageRef & r = refs[k];
        auto att = store.get(r.id);
        std::string repl;
        bool keep = false;
        if (!att || att->kind != "image") {
            repl = "[image: " + r.name + ", not available]";
        } else if (!vision) {
            repl = "[image: " + r.name + ", not shown: the loaded model has no vision]";
            if (!att->text.empty()) repl += "\n" + att->text;
        } else if (budget <= 0) {
            repl = "[image: " + r.name + ", shown earlier]";
        } else {
            std::string b = store.read_bytes(r.id);
            if (b.empty()) {
                repl = "[image: " + r.name + ", not available]";
            } else {
                budget--;
                loaded.push_back(std::move(b));
                keep = true;
            }
        }
        if (!keep) m.content.replace(r.pos, r.len, repl);
    }
    for (size_t k = loaded.size(); k-- > 0;) m.images.push_back(std::move(loaded[k]));   // oldest marker first
}

}  // namespace

void expand_images(AttachmentStore & store, ChatMessage & m, bool vision) {
    int budget = INT_MAX;
    expand_one(store, m, vision, budget);
}

void expand_request_images(AttachmentStore & store, std::vector<ChatMessage> & msgs, bool vision, int max_images) {
    int budget = std::max(0, max_images);
    for (size_t i = msgs.size(); i-- > 0;) expand_one(store, msgs[i], vision, budget);
}

}  // namespace judas
