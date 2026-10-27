// Skills guard: implements scan_skill_text() from section D of the learning contract.
// Scans line by line over a lowercased copy (no std::regex: ECMAScript regex lacks
// lookbehind and case flags we need, and large {n,} repeats misbehave). Every rule is a
// plain byte scan, so nothing can throw at construction. Known limit: a static text scan
// cannot follow dynamic destinations; it only sees the literal patterns below.
#include "skills/guard.h"

#include <regex>

#include <cctype>

namespace judas {

namespace {

constexpr size_t kExcerptCap = 80;  // excerpt length cap
constexpr size_t kB64Min = 501;     // base64 runs longer than 500 chars are flagged

int line_at(const std::string & text, size_t pos) {
    int line = 1;
    for (size_t i = 0; i < pos && i < text.size(); ++i)
        if (text[i] == '\n') line++;
    return line;
}

std::string lowered(std::string s) {
    for (char & c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool alnum(char c) {
    return std::isalnum((unsigned char)c) != 0;
}

// True when the char before pos and the char after pos+len both sit at a boundary
// (start/end of text or a non-alnum), so keywords like "su" do not hit "sudo".
bool bounded(const std::string & text, size_t pos, size_t len) {
    if (pos > 0 && alnum(text[pos - 1])) return false;
    if (pos + len < text.size() && alnum(text[pos + len])) return false;
    return true;
}

// Trimmed, newline-free snippet of up to kExcerptCap chars from pos.
std::string excerpt(const std::string & text, size_t pos, size_t len) {
    if (len > kExcerptCap) len = kExcerptCap;
    std::string out = text.substr(pos, len);
    for (char & c : out) {
        if (c == '\n' || c == '\r') c = ' ';
        if (c == '\t') c = ' ';
    }
    const size_t b = out.find_first_not_of(' ');
    if (b == std::string::npos) return "";
    const size_t e = out.find_last_not_of(' ');
    return out.substr(b, e - b + 1);
}

GuardFinding make(const std::string & sev, const std::string & rule, int line, const std::string & ex) {
    GuardFinding f;
    f.severity = sev;
    f.rule = rule;
    f.line = line;
    f.excerpt = ex;
    return f;
}

// First occurrence of any needle in s from `from`, or npos; stores which needle matched.
size_t find_any(const std::string & line, const char * needles[], size_t count, size_t from, size_t * which) {
    size_t best = std::string::npos;
    for (size_t i = 0; i < count; ++i) {
        const std::string needle(needles[i]);
        const size_t p = line.find(needle, from);
        if (p != std::string::npos && p < best) {
            best = p;
            if (which) *which = i;
        }
    }
    return best;
}

// The plain word right after the first '|' from `from`: its length and [pos_out, length] span,
// or npos when there is no pipe or the next thing is not a plain word (e.g. a command substitution).
size_t word_after_pipe(const std::string & line, size_t from, size_t * pos_out) {
    const size_t bar = line.find('|', from);
    if (bar == std::string::npos) return std::string::npos;
    size_t j = bar + 1;
    while (j < line.size() && (line[j] == ' ' || line[j] == '\t')) j++;
    if (j == line.size()) return std::string::npos;
    size_t k = j;
    while (k < line.size() && alnum(line[k])) k++;
    if (k == j) return std::string::npos;
    *pos_out = j;
    return k - j;
}

void add(GuardReport & report, const std::string & text, const std::string & sev, const std::string & rule,
         size_t abs_pos, size_t len) {
    report.findings.push_back(make(sev, rule, line_at(text, abs_pos), excerpt(text, abs_pos, len)));
}

// Rule 3a: curl/wget/nc combined with an upload/POST flag on one line.
void scan_exfil_upload(GuardReport & report, const std::string & text, const std::string & line, size_t lbegin) {
    static const char *tools[] = {"curl", "wget", "nc"};
    static const char *flags[] = {"--upload-file", "--data-ascii", "--data-binary", "--data-raw", "--data-urlencode",
                                  "--data", "--form", "--upload-", "--post-file", "--post-data", "--post", "--put",
                                  "-d", "-t"};   // lines are lowercased: -t also covers -T
    size_t ti = 0;
    const size_t t = find_any(line, tools, 3, 0, &ti);
    if (t == std::string::npos) return;
    if (!bounded(line, t, std::string(tools[ti]).size())) return;  // "tractor" is not "nc"
    // -F (lowercased to -f, which alone is curl --fail) uploads only with a name=@file or name=<file value
    static const std::regex form(R"((^|\s)(-f|--form)\s+\S*=[@<])");
    if (std::regex_search(line.begin() + (long) t, line.end(), form)) {
        add(report, text, "high", "exfil", lbegin + t, line.size() - t);
        return;
    }
    for (const char *flag : flags) {
        const std::string f(flag);
        size_t p = line.find(f, t);
        while (p != std::string::npos) {
            // A flag must start after a separator and must be whole ("-t timeout" is a different flag).
            const bool sep_before = p == t || !(alnum(line[p - 1]));
            const bool whole = p + f.size() >= line.size() || !(alnum(line[p + f.size()]));
            if (sep_before && whole) {
                add(report, text, "high", "exfil", lbegin + t, line.size() - t);
                return;
            }
            p = line.find(f, p + 1);
        }
    }
}

// Rule 3b: base64 piped straight to a network tool ("base64 id_rsa | curl ...").
void scan_b64_pipe(GuardReport & report, const std::string & text, const std::string & line, size_t lbegin) {
    static const char *nets[] = {"curl", "wget", "nc"};
    size_t b = line.find("base64");
    while (b != std::string::npos) {
        if (bounded(line, b, 6)) {
            size_t p = 0;
            const size_t len = word_after_pipe(line, b, &p);
            if (len != std::string::npos) {
                for (const char *net : nets) {
                    if (line.compare(p, len, net) == 0) {
                        add(report, text, "high", "exfil", lbegin + b, line.size() - b);
                        return;
                    }
                }
            }
        }
        b = line.find("base64", b + 6);
    }
}

// Rule 1: curl/wget piped into an interpreter, at any later pipe stage, bare or by path (`| tee x | sh`,
// `| /bin/bash`, `| sudo sh`). Running fetched code is the whole risk, so it is high (quarantined).
void scan_pipe_to_shell(GuardReport & report, const std::string & text, const std::string & line, size_t lbegin) {
    static const std::regex re(
        R"((^|[^A-Za-z0-9_])(curl|wget)([^A-Za-z0-9_][^\n]*)?\|\s*(sudo\s+)?(\S*/)?(sh|bash|zsh|ash|dash|ksh|fish|python3?|perl|ruby|node)([^A-Za-z0-9_]|$))");
    std::smatch m;
    if (std::regex_search(line, m, re)) {
        size_t at = (size_t) m.position(2);
        add(report, text, "high", "pipe-to-shell", lbegin + at, line.size() - at);
    }
}

// Rule 4: sudo/su/doas/pkexec. `line` is mutable so already-reported spans can be masked out.
void scan_privilege(GuardReport & report, const std::string & text, std::string & line, size_t lbegin) {
    static const char *cmd[] = {"sudo", "pkexec", "doas", "su"};
    for (const char *name : cmd) {
        const size_t n = std::string(name).size();
        size_t p = line.find(name);
        while (p != std::string::npos) {
            if (bounded(line, p, n)) {
                add(report, text, "medium", "sudo-su", lbegin + p, n);
                line.replace(p, n, n, '*');  // do not report the same span twice
            }
            p = line.find(name, p + n);
        }
    }
}

// Rule 5: prompt injection phrases.
void scan_injection(GuardReport & report, const std::string & text, const std::string & line, size_t lbegin,
                    int line_no) {
    static const char *phrases[] = {"ignore previous instructions", "disregard the system prompt", "you are now"};
    for (const char *ph : phrases) {
        size_t p = line.find(ph);
        while (p != std::string::npos) {
            report.findings.push_back(make("low", "prompt-injection", line_no,
                                           excerpt(text, lbegin + p, std::string(ph).size())));
            p = line.find(ph, p + std::string(ph).size());
        }
    }
}

// Rule 2: known credential paths and files.
void scan_secrets(GuardReport & report, const std::string & text, const std::string & line, size_t lbegin) {
    // "secrets.env" before ".env" so an ".env" hit inside "secrets.env" is not a duplicate.
    static const char *kw[] = {"~/.ssh", "secrets.env", "id_rsa", ".gnupg", "/etc/shadow", ".env"};
    for (const char *k : kw) {
        const std::string key(k);
        size_t p = line.find(key);
        while (p != std::string::npos) {
            const bool part_of_pairs = key == ".env" && p >= 7 && line.compare(p - 7, 7, "secrets") == 0;
            const bool whole = p + key.size() >= line.size() || !(alnum(line[p + key.size()]));
            if (!part_of_pairs && whole) add(report, text, "medium", "reading-secrets", lbegin + p, key.size());
            p = line.find(key, p + key.size());
        }
    }
}

// Rule 7: very long base64-looking runs (data hidden in plain sight).
void scan_long_b64(GuardReport & report, const std::string & text, const std::string & line, size_t lbegin) {
    size_t i = 0;
    while (i < line.size()) {
        const char c = line[i];
        if (alnum(c) || c == '/' || c == '+' || c == '=' || c == '_') {
            size_t j = i + 1;
            while (j < line.size()) {
                const char d = line[j];
                if (alnum(d) || d == '/' || d == '+' || d == '=' || d == '_') j++; else break;
            }
            if (j - i >= kB64Min) add(report, text, "low", "long-base64", lbegin + i, j - i);
            i = j;
        } else {
            ++i;
        }
    }
}

// True when the three bytes at offset i form a hidden unicode code point:
// zero width (U+200B..U+200F), bidi overrides (U+202A..U+202E), isolates (U+2066..U+2069),
// U+FEFF, and tag characters (U+E0000..U+E007F).
bool is_hidden_byte3(unsigned char a, unsigned char b, unsigned char c) {
    if (a == 0xE2 && b == 0x80 && c >= 0x8B && c <= 0x8F) return true;
    if (a == 0xE2 && b == 0x80 && c >= 0xAA && c <= 0xAE) return true;   // U+202A..U+202E (not the bullet at U+2022)
    if (a == 0xE2 && b == 0x81 && c >= 0xA6 && c <= 0xAA) return true;
    return a == 0xEF && b == 0xBB && c == 0xBF;
}

// Tag characters U+E0000..U+E007F: F3 A0 80 80 .. F3 A0 81 BF.
bool is_tag_char(const std::string & s, size_t i) {
    return i + 3 < s.size() && (unsigned char) s[i] == 0xF3 && (unsigned char) s[i + 1] == 0xA0 &&
           ((unsigned char) s[i + 2] == 0x80 || (unsigned char) s[i + 2] == 0x81);
}

}  // namespace

GuardReport scan_skill_text(const std::string & markdown) {
    GuardReport report;
    const std::string lower = lowered(markdown);

    // Split into lines once; every rule runs per line so findings stay 1-based and local.
    size_t lbegin = 0;
    int line_no = 1;
    while (lbegin <= lower.size()) {
        size_t lend = lower.find('\n', lbegin);
        if (lend == std::string::npos) lend = lower.size();
        std::string line = lower.substr(lbegin, lend - lbegin);
        scan_pipe_to_shell(report, markdown, line, lbegin);
        scan_exfil_upload(report, markdown, line, lbegin);
        scan_b64_pipe(report, markdown, line, lbegin);
        scan_privilege(report, markdown, line, lbegin);
        scan_injection(report, markdown, line, lbegin, line_no);
        scan_secrets(report, markdown, line, lbegin);
        scan_long_b64(report, markdown, line, lbegin);
        if (lend == lower.size()) break;
        lbegin = lend + 1;
        ++line_no;
    }

    // Rule 6 walks the raw UTF-8 bytes (hidden code points survive lowercasing unchanged).
    for (size_t i = 0; i + 2 < markdown.size(); ++i) {
        if (is_tag_char(markdown, i)) {
            add(report, markdown, "high", "hidden-unicode", i, 4);   // invisible instructions: quarantine
            i += 3;
            continue;
        }
        if (is_hidden_byte3((unsigned char)markdown[i], (unsigned char)markdown[i + 1],
                            (unsigned char)markdown[i + 2])) {
            add(report, markdown, "medium", "hidden-unicode", i, 3);
            i += 2;
        }
    }

    return report;
}

}  // namespace judas
