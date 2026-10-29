#include "tui/attach.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

namespace judas::tui {

namespace {
const char * CLIP = "\xF0\x9F\x93\x8E";   // paperclip
const char * PIC = "\xF0\x9F\x96\xBC";    // framed picture

size_t count_cps(const std::string & s) {
    size_t n = 0;
    for (unsigned char c : s) if ((c & 0xC0) != 0x80) n++;
    return n;
}

std::string attr(const std::string & tag, const char * key) {
    std::string k = std::string(key) + "=\"";
    size_t p = tag.find(k);
    if (p == std::string::npos) return "";
    p += k.size();
    size_t e = tag.find('"', p);
    return e == std::string::npos ? tag.substr(p) : tag.substr(p, e - p);
}

std::string home_dir() {
    const char * h = std::getenv("HOME");
    return h ? h : "";
}
}  // namespace

std::vector<DisplayLine> collapse_attachments(const std::string & text) {
    std::vector<DisplayLine> out;
    size_t pos = 0;
    const std::string open = "<attachment ", close = "</attachment>";
    while (true) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        size_t next = nl == std::string::npos ? text.size() : nl + 1;
        bool last = nl == std::string::npos;
        if (line.compare(0, open.size(), open) == 0) {
            size_t tag_end = text.find('>', pos);
            // a name may contain '>': prefer the first ">" that follows a closing quote
            size_t q = text.find("\">", pos);
            if (q != std::string::npos && (tag_end == std::string::npos || q + 1 > tag_end)) tag_end = q + 1;
            if (tag_end != std::string::npos) {
                std::string tag = text.substr(pos, tag_end - pos + 1);
                size_t cl = text.find(close, tag_end);
                size_t body_b = tag_end + 1, body_e = cl == std::string::npos ? text.size() : cl;
                std::string body = body_b < body_e ? text.substr(body_b, body_e - body_b) : "";
                if (!body.empty() && body.front() == '\n') body.erase(0, 1);
                if (!body.empty() && body.back() == '\n') body.pop_back();
                std::string name = attr(tag, "name"), kind = attr(tag, "kind");
                if (name.empty()) name = "attachment";
                out.push_back({std::string(CLIP) + " " + name + " (" + (kind.empty() ? "file" : kind) + ", " +
                                   std::to_string(count_cps(body)) + " chars)",
                               true});
                if (cl == std::string::npos) return out;
                pos = cl + close.size();
                if (pos < text.size() && text[pos] == '\n') pos++;
                else if (pos >= text.size()) return out;
                continue;
            }
        }
        // image markers
        const std::string im = "[[image:";
        size_t a = line.find(im);
        if (a != std::string::npos) {
            std::string res;
            size_t i = 0;
            bool only = false;
            while (true) {
                size_t m = line.find(im, i);
                size_t e = m == std::string::npos ? std::string::npos : line.find("]]", m);
                if (m == std::string::npos || e == std::string::npos) { res += line.substr(i); break; }
                res += line.substr(i, m - i);
                std::string inner = line.substr(m + im.size(), e - m - im.size());
                size_t bar = inner.find('|');
                std::string name = bar == std::string::npos ? "image" : inner.substr(bar + 1);
                if (name.empty()) name = "image";
                res += std::string(PIC) + " " + name;
                i = e + 2;
                only = res.size() && m == 0 && i >= line.size();
            }
            out.push_back({res, only});
        } else {
            out.push_back({line, false});
        }
        if (last) break;
        pos = next;
    }
    return out;
}

std::vector<std::string> shell_split(const std::string & s) {
    std::vector<std::string> out;
    std::string cur;
    bool have = false;
    char quote = 0;
    for (size_t i = 0; i < s.size(); i++) {
        char c = s[i];
        if (quote) {
            if (c == quote) quote = 0;
            else if (c == '\\' && quote == '"' && i + 1 < s.size()) cur += s[++i];
            else cur += c;
        } else if (c == '\'' || c == '"') {
            quote = c;
            have = true;
        } else if (c == '\\' && i + 1 < s.size()) {
            cur += s[++i];
            have = true;
        } else if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            if (have || !cur.empty()) out.push_back(cur);
            cur.clear();
            have = false;
        } else {
            cur += c;
            have = true;
        }
    }
    if (have || !cur.empty()) out.push_back(cur);
    return out;
}

std::string percent_decode(const std::string & s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() + 0 && std::isxdigit((unsigned char) s[i + 1]) && std::isxdigit((unsigned char) s[i + 2])) {
            o += (char) std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            o += s[i];
        }
    }
    return o;
}

std::string url_encode(const std::string & s) {
    static const char * hex = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char) c;
        else { o += '%'; o += hex[c >> 4]; o += hex[c & 15]; }
    }
    return o;
}

std::string resolve_file(const std::string & token, const std::string & cwd, bool strict) {
    std::string t = token;
    if (t.empty()) return "";
    bool uri = false;
    if (t.rfind("file://", 0) == 0) {
        t = t.substr(7);
        if (t.rfind("localhost/", 0) == 0) t = t.substr(9);
        t = percent_decode(t);
        uri = true;
    }
    bool pathy = uri || t[0] == '/' || t.rfind("~/", 0) == 0 || t == "~" || t.rfind("./", 0) == 0 || t.rfind("../", 0) == 0;
    if (strict && !pathy) return "";
    if (t == "~" || t.rfind("~/", 0) == 0) t = home_dir() + t.substr(1);
    fs::path p(t);
    if (p.is_relative()) p = fs::path(cwd) / p;
    std::error_code ec;
    if (!fs::is_regular_file(p, ec)) return "";
    fs::path n = p.lexically_normal();
    return n.string();
}

std::vector<std::string> detect_paths(const std::string & text, const std::string & cwd) {
    std::vector<std::string> out;
    auto toks = shell_split(text);
    if (toks.empty() || toks.size() > 12) return {};
    for (auto & t : toks) {
        std::string r = resolve_file(t, cwd, true);
        if (r.empty()) return {};
        out.push_back(r);
    }
    return out;
}

std::vector<std::string> parse_attach_args(const std::string & arg, const std::string & cwd, std::vector<std::string> * missing) {
    std::vector<std::string> out;
    for (auto & t : shell_split(arg)) {
        std::string r = resolve_file(t, cwd, false);
        if (r.empty()) { if (missing) missing->push_back(t); }
        else out.push_back(r);
    }
    return out;
}

std::string human_bytes(long long n) {
    char b[32];
    if (n < 1024) std::snprintf(b, sizeof b, "%lld B", n);
    else if (n < 1024 * 1024) std::snprintf(b, sizeof b, "%.1f KB", n / 1024.0);
    else if (n < 1024LL * 1024 * 1024) std::snprintf(b, sizeof b, "%.1f MB", n / 1048576.0);
    else std::snprintf(b, sizeof b, "%.1f GB", n / 1073741824.0);
    return b;
}

std::string guess_mime(const std::string & filename) {
    static const std::map<std::string, std::string> m = {
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"},
        {"webp", "image/webp"}, {"bmp", "image/bmp"}, {"pdf", "application/pdf"}, {"txt", "text/plain"},
        {"md", "text/markdown"}, {"json", "application/json"}, {"csv", "text/csv"}, {"html", "text/html"},
        {"rtf", "application/rtf"}, {"epub", "application/epub+zip"},
        {"docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {"xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {"pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
        {"odt", "application/vnd.oasis.opendocument.text"}, {"ods", "application/vnd.oasis.opendocument.spreadsheet"},
        {"odp", "application/vnd.oasis.opendocument.presentation"}};
    size_t d = filename.rfind('.');
    if (d == std::string::npos) return "application/octet-stream";
    std::string e = filename.substr(d + 1);
    for (auto & c : e) c = (char) std::tolower((unsigned char) c);
    auto it = m.find(e);
    return it == m.end() ? "application/octet-stream" : it->second;
}

std::string chip_text(const PendingAttachment & a, int index) {
    std::string s = "[" + std::to_string(index) + "] ";
    if (a.kind == "image") return s + PIC + " " + a.name;
    return s + CLIP + " " + a.name + " " + human_bytes(a.bytes);
}

std::vector<std::string> render_chip_rows(const std::vector<PendingAttachment> & p, int width, const Palette & pal) {
    std::vector<std::string> rows;
    std::string cur;
    int curw = 0;
    for (size_t i = 0; i < p.size(); i++) {
        std::string c = truncate_width(chip_text(p[i], (int) i + 1), std::max(10, width - 4));
        int w = display_width(c);
        if (!cur.empty() && curw + 2 + w > width - 2) {
            rows.push_back(cur + pal.reset);
            cur.clear();
            curw = 0;
        }
        if (cur.empty()) { cur = "  "; curw = 2; }
        else { cur += "  "; curw += 2; }
        cur += pal.accent + c + pal.reset;
        curw += w;
    }
    if (!cur.empty()) rows.push_back(cur + pal.reset);
    return rows;
}

}  // namespace judas::tui
