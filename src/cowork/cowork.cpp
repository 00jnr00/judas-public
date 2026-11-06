#include <vector>
#include <optional>
#include <cmath>
#include <chrono>
#include <algorithm>
#include "cowork/cowork.h"

#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

namespace judas::cowork {

namespace fs = std::filesystem;

namespace {

const char * DESIGN_PROMPT = R"DSN(Design mode: this is the loose mode. You are iterating on a design with the user, fast, and nothing here promises precision.
- Every visible result is an artifact (SVG, HTML or model3d). Make the first one early, rough is fine; do not describe a design you have not drawn.
- After each change, render it with artifact_render and look at the image before you answer. Say what you see and what you would change.
- Prefer updating the same artifact (artifact_update, a new version) over making a new one. Versions are the history; the user compares them.
- The CAD tools (cad_*) are allowed for rough geometry and cross-sections. No precision promises: label anything sketched as concept, and say which numbers are placeholders.
- Offer two or three directions when the brief is open, then follow the one the user picks. Keep notes short; the canvas is the answer.)DSN";

const char * RESEARCH_PROMPT = R"RSC(Research mode: find and read sources, then keep one current report.
- Search with web_search and read pages with web_fetch. Do not answer from memory when a source can settle it.
- Record every source you rely on with source_add (url, title, a short quote, a note). Fetched pages are also recorded automatically; add the title and quote when you use one.
- The report is an artifact, not a file: create it with artifact_create (kind "markdown") as soon as you have a first finding, then update it with artifact_update (a new version) as findings land. The user's Research view shows only that artifact, so never write the report with write_file; save a copy to a file only when the user asks. Keep exactly one report artifact per session.
- Cite sources in the report as [n], where n is the number source_add returned, so each claim points at a source in the list.
- Say what is unknown, what sources disagree about, and what you could not find. Never invent a source, quote or number.
- Proprietary files stay on this machine: do not put their content into search queries or fetched URLs.)RSC";

const char * WRITE_PROMPT = R"WRT(Write mode: the text is the user's. Never rewrite it.
- Do not edit any existing file in the project; edit_file and writes to existing files are refused. Give feedback with doc_suggest only.
- Spelling and clear typos: a suggestion of kind "spelling" with the exact text to find (it must occur once in the file) and the fixed text. Keep the user's voice and syntax; unusual syntax is intentional, never correct it.
- Everything else (structure, a buried lede, pacing, clarity, a question about intent) is a suggestion of kind "note": find anchors the note to exact text that occurs once, replace is empty, why is the feedback itself.
- You may create new files the user asks for (an outline, a draft they requested), never over an existing one.
- Read the whole document before suggesting. Be specific and brief; the user decides what to accept.)WRT";

// calc_record checks its own arithmetic: the expression after its last '=' is evaluated when it is plain numbers and
// operators (+ - * / ^, parentheses, pi, sqrt). Anything else (names, units) is recorded unchecked.
struct Arith {
    const std::string & s; size_t i = 0; bool ok = true;
    explicit Arith(const std::string & src) : s(src) {}
    void ws() { while (i < s.size() && std::isspace((unsigned char) s[i])) i++; }
    bool eat(char c) { ws(); if (i < s.size() && s[i] == c) { i++; return true; } return false; }
    double expr() { double v = term(); while (ok) { if (eat('+')) v += term(); else if (eat('-')) v -= term(); else break; } return v; }
    double term() {
        double v = power();
        while (ok) {
            ws();
            if (eat('*')) v *= power();
            else if (eat('/')) { double d = power(); if (d == 0) ok = false; else v /= d; }
            else if (i < s.size() && (s[i] == '(' )) v *= power();   // 2(3+4)
            else break;
        }
        return v;
    }
    double power() { double b = unary(); if (eat('^')) return std::pow(b, power()); if (ws(), s.compare(i, 2, "**") == 0) { i += 2; return std::pow(b, power()); } return b; }
    double unary() { if (eat('-')) return -unary(); if (eat('+')) return unary(); return atom(); }
    double atom() {
        ws();
        if (eat('(')) { double v = expr(); if (!eat(')')) ok = false; return v; }
        if (s.compare(i, 2, "pi") == 0 && !(i + 2 < s.size() && std::isalnum((unsigned char) s[i + 2]))) { i += 2; return M_PI; }
        if (s.compare(i, 4, "sqrt") == 0) { i += 4; if (!eat('(')) { ok = false; return 0; } double v = expr(); if (!eat(')') || v < 0) ok = false; return std::sqrt(v); }
        size_t j = i;
        while (j < s.size() && (std::isdigit((unsigned char) s[j]) || s[j] == '.')) j++;
        if (j < s.size() && (s[j] == 'e' || s[j] == 'E') && j + 1 < s.size() &&
            (std::isdigit((unsigned char) s[j + 1]) || ((s[j + 1] == '-' || s[j + 1] == '+') && j + 2 < s.size() && std::isdigit((unsigned char) s[j + 2])))) {
            j += 2; while (j < s.size() && std::isdigit((unsigned char) s[j])) j++;
        }
        if (j == i) { ok = false; return 0; }
        double v = std::strtod(s.substr(i, j - i).c_str(), nullptr); i = j; return v;
    }
};

}  // namespace

std::optional<double> eval_arith(const std::string & text) {
    // "M = F*L = 500*80 = 40000 N*mm": use the rightmost '='-separated part that is arithmetic with an operator in it
    // (a bare number is the claimed result, not something to check against).
    std::vector<std::string> parts;
    size_t st = 0;
    for (size_t k = 0; k <= text.size(); k++)
        if (k == text.size() || text[k] == '=') { parts.push_back(text.substr(st, k - st)); st = k + 1; }
    for (size_t k = parts.size(); k-- > 0;) {
        const std::string & e = parts[k];
        Arith p(e);
        double v = p.expr();
        p.ws();
        if (!p.ok || p.i != e.size() || !std::isfinite(v)) continue;
        std::string t = e;
        t.erase(0, t.find_first_not_of(" \t"));
        bool has_op = t.find_first_of("*/^+(") != std::string::npos || t.find("sqrt") != std::string::npos ||
                      t.find("pi") != std::string::npos || t.find('-', 1) != std::string::npos;
        if (has_op) return v;
    }
    return std::nullopt;
}

namespace {

ToolResult fail(const std::string & m) { return {false, m, false}; }
ToolResult okr(const std::string & m) { return {true, m, false}; }

std::string scalar(const json & a, const char * k) {
    if (!a.contains(k)) return "";
    const json & v = a[k];
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number() || v.is_boolean()) return v.dump();
    return "";
}

size_t count_occurrences(const std::string & hay, const std::string & needle) {
    if (needle.empty()) return 0;
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) n++;
    return n;
}

bool slurp(const std::string & path, std::string & out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool looks_binary(const std::string & s) {
    return s.find('\0') != std::string::npos;
}

std::string wrong_mode(const ToolContext & ctx, const char * tool, const char * mode) {
    if (ctx.mode.empty() || ctx.mode == mode) return "";
    return std::string(tool) + " is only available in " + mode + " mode.";
}

}  // namespace

const char * design_prompt() { return DESIGN_PROMPT; }
const char * research_prompt() { return RESEARCH_PROMPT; }
const char * write_prompt() { return WRITE_PROMPT; }

bool valid_mode(const std::string & m) {
    return m == "chat" || m == "cowork" || m == "code" || m == "learn" || m == "swarm" || m == "engineering" ||
           m == "design" || m == "research" || m == "write";
}

bool mode_change_allowed(const std::string & from, const std::string & to) {
    auto work = [](const std::string & m) { return m == "engineering" || m == "design" || m == "research" || m == "write"; };
    return from == to || (!work(from) && !work(to));
}

bool tool_allowed(const std::string & tool, const std::string & mode, bool lead) {
    if (tool.rfind("cad_", 0) == 0) return mode == "engineering" || mode == "design";
    if (tool == "calc_record") return mode == "engineering" && lead;
    if (tool == "source_add") return mode == "research" && lead;
    if (tool == "doc_suggest") return mode == "write" && lead;
    return true;
}

json calc_json(const CalcRow & c) {
    return {{"n", c.n}, {"name", c.name}, {"expr", c.expr}, {"value", c.value}, {"unit", c.unit}, {"note", c.note},
            {"created_ms", c.created_ms}};
}
json source_json(const SourceRow & s) {
    return {{"n", s.n}, {"url", s.url}, {"title", s.title}, {"quote", s.quote}, {"note", s.note}, {"auto", s.autoadded},
            {"created_ms", s.created_ms}};
}
json suggestion_json(const SuggestionRow & g) {
    return {{"id", g.id}, {"path", g.path}, {"kind", g.kind}, {"find", g.find}, {"replace", g.replace}, {"why", g.why},
            {"status", g.status}, {"created_ms", g.created_ms}};
}

std::string resolve_in_cwd(const std::string & cwd, const std::string & rel, bool must_exist, std::string & out) {
    if (rel.empty()) return "path is empty";
    if (rel.find('\0') != std::string::npos) return "bad path";
    fs::path r(rel);
    if (r.is_absolute()) return "path must be relative to the session folder";
    std::error_code ec;
    fs::path root = fs::weakly_canonical(fs::path(cwd), ec);
    if (ec || root.empty()) return "the session folder is not available";
    fs::path full = fs::weakly_canonical(root / r, ec);
    if (ec) return "bad path";
    std::string rs = root.string(), fsr = full.string();
    if (!(fsr.size() > rs.size() && fsr.compare(0, rs.size(), rs) == 0 && fsr[rs.size()] == '/'))
        return "path is outside the session folder";
    if (must_exist && !fs::is_regular_file(full, ec)) return "no such file: " + rel;
    out = fsr;
    return "";
}

std::vector<std::pair<std::string, long long>> list_documents(const std::string & cwd, size_t limit) {
    namespace fs = std::filesystem;
    std::vector<std::pair<std::string, long long>> out;
    std::error_code ec;
    fs::path root = fs::weakly_canonical(cwd, ec);
    if (cwd.empty() || ec || !fs::is_directory(root, ec)) return out;
    static const char * exts[] = {".md", ".markdown", ".txt", ".tex", ".rst", ".org", ".adoc", ".html"};
    static const char * skip[] = {"node_modules", "build", "dist", "target", "__pycache__", "venv"};
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    size_t seen = 0;
    for (; it != end && seen < 20000; it.increment(ec), seen++) {
        if (ec) break;
        const fs::path & p = it->path();
        std::string name = p.filename().string();
        if (it->is_symlink(ec)) { if (it->is_directory(ec)) it.disable_recursion_pending(); continue; }
        if (it->is_directory(ec)) {
            bool drop = it.depth() >= 3 || (!name.empty() && name[0] == '.');
            for (auto * k : skip) if (name == k) drop = true;
            if (drop) it.disable_recursion_pending();
            continue;
        }
        if (!it->is_regular_file(ec) || (!name.empty() && name[0] == '.')) continue;
        std::string ext = p.extension().string();
        for (auto & c : ext) c = (char) std::tolower((unsigned char) c);
        bool ok = false;
        for (auto * e : exts) if (ext == e) ok = true;
        if (!ok) continue;
        struct stat st{};
        long long ms = ::stat(p.c_str(), &st) == 0 ? (long long) st.st_mtim.tv_sec * 1000 + st.st_mtim.tv_nsec / 1000000 : 0;
        out.push_back({fs::relative(p, root, ec).generic_string(), ms});
    }
    std::sort(out.begin(), out.end(), [](auto & a, auto & b) { return a.second > b.second; });
    if (out.size() > limit) out.resize(limit);
    return out;
}

FileResult read_file(const std::string & cwd, const std::string & rel) {
    FileResult r;
    std::string p;
    std::string why = resolve_in_cwd(cwd, rel, true, p);
    if (!why.empty()) {
        r.status = why.rfind("no such file", 0) == 0 ? 404 : why == "path is empty" || why == "bad path" ? 400 : 403;
        r.error = why;
        return r;
    }
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (ec) { r.status = 404; r.error = "no such file: " + rel; return r; }
    if (sz > kMaxFileBytes) { r.status = 413; r.error = "file is over 2 MB"; return r; }
    if (!slurp(p, r.text)) { r.status = 404; r.error = "cannot read " + rel; return r; }
    if (looks_binary(r.text)) { r.status = 415; r.error = "binary file"; r.text.clear(); return r; }
    r.path = p;
    r.hash = sha256_hex(r.text);
    return r;
}

FileResult write_file(const std::string & cwd, const std::string & rel, const std::string & text, const std::string & base_hash) {
    FileResult r;
    if (text.size() > kMaxFileBytes) { r.status = 413; r.error = "text is over 2 MB"; return r; }
    if (looks_binary(text)) { r.status = 415; r.error = "text contains NUL bytes"; return r; }
    std::string p;
    std::string why = resolve_in_cwd(cwd, rel, false, p);
    if (!why.empty()) { r.status = why == "path is empty" || why == "bad path" ? 400 : 403; r.error = why; return r; }
    std::error_code ec;
    bool exists = fs::exists(p, ec);
    if (exists) {
        if (!fs::is_regular_file(p, ec)) { r.status = 400; r.error = rel + " is not a file"; return r; }
        std::string cur;
        if (!slurp(p, cur)) { r.status = 403; r.error = "cannot read " + rel; return r; }
        if (sha256_hex(cur) != base_hash) {
            r.status = 409;
            r.error = "the file changed since you loaded it";
            r.hash = sha256_hex(cur);
            return r;
        }
    } else if (!base_hash.empty()) {
        r.status = 409;
        r.error = "the file no longer exists";
        return r;
    } else {
        fs::create_directories(fs::path(p).parent_path(), ec);
    }
    std::string tmp = p + ".judas-tmp-" + std::to_string(getpid());
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { r.status = 403; r.error = "cannot write " + rel; return r; }
        f << text;
        if (!f) { fs::remove(tmp, ec); r.status = 500; r.error = "write failed"; return r; }
    }
    struct stat st{};
    if (exists && ::stat(p.c_str(), &st) == 0) ::chmod(tmp.c_str(), st.st_mode & 07777);
    fs::rename(tmp, p, ec);
    if (ec) { fs::remove(tmp, ec); r.status = 500; r.error = "rename failed"; return r; }
    r.path = p;
    r.hash = sha256_hex(text);
    return r;
}

Outcome accept_suggestion(Store & store, Journal * journal, const std::string & session, const std::string & cwd,
                          const std::string & gid) {
    auto g = store.get_suggestion(session, gid);
    if (!g) return {404, {{"error", {{"message", "no such suggestion"}, {"type", "not_found"}}}}};
    auto err = [&](int status, const std::string & m) {
        return Outcome{status, {{"error", {{"message", m}, {"type", status == 409 ? "conflict" : "invalid_request_error"}}},
                                {"suggestion", suggestion_json(*g)}}};
    };
    if (g->status != "open") return err(409, "suggestion is already " + g->status);
    if (g->kind == "note") {
        store.set_suggestion_status(session, gid, "accepted");
        g->status = "accepted";
        return {200, {{"ok", true}, {"suggestion", suggestion_json(*g)}}};
    }
    auto fail_stale = [&](const std::string & m) {
        store.set_suggestion_status(session, gid, "stale");
        g->status = "stale";
        return err(409, m);
    };
    std::string p;
    if (!resolve_in_cwd(cwd, g->path, true, p).empty()) return fail_stale("the file is gone or outside the session folder");
    std::string text;
    if (!slurp(p, text)) return fail_stale("cannot read the file");
    if (count_occurrences(text, g->find) != 1) return fail_stale("the text to replace no longer occurs exactly once");
    std::string cp;
    if (journal) {
        cp = journal->begin(session, "user", "accept suggestion " + gid + " " + g->path);
        journal->record(cp, p);
    }
    size_t pos = text.find(g->find);
    text.replace(pos, g->find.size(), g->replace);
    std::string tmp = p + ".judas-tmp-" + std::to_string(getpid());
    struct stat st{};
    bool have_st = ::stat(p.c_str(), &st) == 0;
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        f << text;
        if (!f) {
            std::error_code ec;
            fs::remove(tmp, ec);
            if (journal) journal->commit(cp);
            return err(500, "write failed");
        }
    }
    if (have_st) ::chmod(tmp.c_str(), st.st_mode & 07777);
    std::error_code ec;
    fs::rename(tmp, p, ec);
    if (journal) journal->commit(cp);
    if (ec) { fs::remove(tmp, ec); return err(500, "rename failed"); }
    store.set_suggestion_status(session, gid, "accepted");
    g->status = "accepted";
    json body = {{"ok", true}, {"suggestion", suggestion_json(*g)}};
    if (!cp.empty()) body["checkpoint"] = cp;
    return {200, body};
}

Outcome reject_suggestion(Store & store, const std::string & session, const std::string & gid) {
    auto g = store.get_suggestion(session, gid);
    if (!g) return {404, {{"error", {{"message", "no such suggestion"}, {"type", "not_found"}}}}};
    if (g->status != "open")
        return {409, {{"error", {{"message", "suggestion is already " + g->status}, {"type", "conflict"}}},
                      {"suggestion", suggestion_json(*g)}}};
    store.set_suggestion_status(session, gid, "rejected");
    g->status = "rejected";
    return {200, {{"ok", true}, {"suggestion", suggestion_json(*g)}}};
}

void register_cowork_tools(ToolRegistry & reg, Store & store) {
    {
        Tool t;
        t.name = "calc_record";
        t.description = "Engineering mode: record one calculation row in the session's calc sheet (name, the expression with "
                        "its inputs, the result value and unit, an optional note). Returns the row number. Record every "
                        "number the design relies on.";
        t.parameters = {{"type", "object"},
                        {"properties", {{"name", {{"type", "string"}, {"description", "What is computed, e.g. bolt preload"}}},
                                        {"expr", {{"type", "string"}, {"description", "Formula, then '=' and the substituted numbers only, e.g. \"sigma = 6*M/(b*t^2) = 6*40000/(30*8^2)\". Judas evaluates the part after the last '=' and rejects a value that does not match."}}},
                                        {"value", {{"type", json::array({"string", "number"})}, {"description", "The result (a number or short text)"}}},
                                        {"unit", {{"type", "string"}, {"description", "Unit of the value, \"\" if none"}}},
                                        {"note", {{"type", "string"}, {"description", "Assumption, source or check (optional)"}}}}},
                        {"required", json::array({"expr", "value"})}};
        t.run = [&store](const json & a, ToolContext & ctx) -> ToolResult {
            if (auto w = wrong_mode(ctx, "calc_record", "engineering"); !w.empty()) return fail(w);
            if (ctx.session.empty()) return fail("calc_record: no session");
            CalcRow c;
            c.name = scalar(a, "name"); c.expr = scalar(a, "expr"); c.value = scalar(a, "value");
            c.unit = scalar(a, "unit"); c.note = scalar(a, "note");
            if (c.name.empty()) {   // "sigma_allow = 276/2": the left side names the row (models often skip name)
                size_t eq = c.expr.find('=');
                if (eq != std::string::npos) {
                    std::string l = c.expr.substr(0, eq);
                    while (!l.empty() && std::isspace((unsigned char) l.back())) l.pop_back();
                    while (!l.empty() && std::isspace((unsigned char) l.front())) l.erase(0, 1);
                    if (!l.empty() && l.size() <= 60) c.name = l;
                }
            }
            if (c.name.empty() || c.expr.empty() || c.value.empty())
                return fail("calc_record needs expr and value, and a name (or an expression that starts with \"name = ...\")");
            std::string checked;
            if (auto v = eval_arith(c.expr)) {
                char buf[64];
                std::snprintf(buf, sizeof buf, "%.6g", *v);
                char * end = nullptr;
                double claimed = std::strtod(c.value.c_str(), &end);
                bool numeric = end && end != c.value.c_str();
                if (!numeric) c.value = buf;   // text value but checkable arithmetic: the computed number wins
                else if (std::fabs(claimed - *v) > 0.005 * std::max(std::fabs(*v), 1e-12))
                    return fail("calc_record: the value " + c.value + " does not match the expression, which evaluates to " +
                                buf + ". Fix the value or the expression, then record it again.");
                checked = " (arithmetic checked)";
            } else {
                checked = " (not checked: write the expression after '=' as numbers and operators only, units in unit/note, "
                          "so Judas can check it)";
            }
            long long n = store.add_calc(ctx.session, c);
            return okr("Recorded calc row " + std::to_string(n) + ": " + c.name + " = " + c.value + (c.unit.empty() ? "" : " " + c.unit) + checked);
        };
        reg.add(std::move(t));
    }
    {
        Tool t;
        t.name = "source_add";
        t.description = "Research mode: record a source you rely on (url, title, optional short quote and note). Returns its "
                        "number n; cite it in the report as [n]. The same url always gets the same number.";
        t.parameters = {{"type", "object"},
                        {"properties", {{"url", {{"type", "string"}, {"description", "http or https URL"}}},
                                        {"title", {{"type", "string"}, {"description", "Page or document title"}}},
                                        {"quote", {{"type", "string"}, {"description", "A short exact quote that supports the claim (optional)"}}},
                                        {"note", {{"type", "string"}, {"description", "Why it matters or how reliable it is (optional)"}}}}},
                        {"required", json::array({"url", "title"})}};
        t.run = [&store](const json & a, ToolContext & ctx) -> ToolResult {
            if (auto w = wrong_mode(ctx, "source_add", "research"); !w.empty()) return fail(w);
            if (ctx.session.empty()) return fail("source_add: no session");
            SourceRow s;
            s.url = scalar(a, "url"); s.title = scalar(a, "title"); s.quote = scalar(a, "quote"); s.note = scalar(a, "note");
            if (s.url.rfind("http://", 0) != 0 && s.url.rfind("https://", 0) != 0) return fail("source_add: url must start with http:// or https://");
            bool added = false;
            long long n = store.add_source(ctx.session, s, &added);
            return okr(std::string(added ? "Added source [" : "Source already recorded, updated [") + std::to_string(n) + "]: " + s.url);
        };
        reg.add(std::move(t));
    }
    {
        Tool t;
        t.name = "doc_suggest";
        t.description = "Write mode: suggest changes to one of the user's files without touching it. suggestions is a list of "
                        "{find, replace, kind, why}. kind \"spelling\": find is the exact text (occurs once in the file), "
                        "replace the fix. kind \"note\": find anchors the note (exact text, occurs once), replace is \"\", "
                        "why is the feedback. The user accepts or rejects each one.";
        t.parameters = {{"type", "object"},
                        {"properties",
                         {{"path", {{"type", "string"}, {"description", "File path relative to the session folder; it must exist"}}},
                          {"suggestions",
                           {{"type", "array"},
                            {"items", {{"type", "object"},
                                       {"properties", {{"find", {{"type", "string"}}},
                                                       {"replace", {{"type", "string"}}},
                                                       {"kind", {{"type", "string"}, {"enum", json::array({"spelling", "note"})}}},
                                                       {"why", {{"type", "string"}}}}},
                                       {"required", json::array({"find", "kind", "why"})}}}}}}},
                        {"required", json::array({"path", "suggestions"})}};
        t.run = [&store](const json & a, ToolContext & ctx) -> ToolResult {
            if (auto w = wrong_mode(ctx, "doc_suggest", "write"); !w.empty()) return fail(w);
            if (ctx.session.empty()) return fail("doc_suggest: no session");
            std::string rel = scalar(a, "path");
            if (!a.contains("suggestions") || !a["suggestions"].is_array() || a["suggestions"].empty())
                return fail("doc_suggest needs a non-empty suggestions list");
            std::string p;
            if (std::string why = resolve_in_cwd(ctx.cwd, rel, true, p); !why.empty()) return fail("doc_suggest: " + why);
            FileResult f = read_file(ctx.cwd, rel);
            if (f.status != 200) return fail("doc_suggest: " + f.error);
            std::string out;
            int ok = 0, bad = 0;
            size_t i = 0;
            for (const auto & it : a["suggestions"]) {
                i++;
                std::string find = scalar(it, "find"), replace = scalar(it, "replace"), kind = scalar(it, "kind"), why = scalar(it, "why");
                std::string reject;
                if (!it.is_object()) reject = "not an object";
                else if (kind != "spelling" && kind != "note") reject = "kind must be spelling or note";
                else if (find.empty()) reject = "find is empty";
                else if (size_t n = count_occurrences(f.text, find); n != 1)
                    reject = n == 0 ? "find does not occur in the file" : "find occurs " + std::to_string(n) + " times; use longer text so it occurs once";
                else if (kind == "spelling" && replace == find) reject = "replace equals find";
                else if (kind == "spelling" && replace.empty()) reject = "a spelling fix needs replace";
                if (reject.empty() && it.is_object()) {
                    // the same open suggestion again (the model repeats itself): keep the first, say so
                    for (auto & e : store.list_suggestions(ctx.session))
                        if (e.status == "open" && e.path == rel && e.kind == kind && e.find == find &&
                            (kind == "note" || e.replace == replace)) { reject = "already suggested as " + e.id; break; }
                }
                if (!reject.empty()) {
                    bad++;
                    out += "Rejected item " + std::to_string(i) + ": " + reject + "\n";
                    continue;
                }
                SuggestionRow g;
                g.path = rel; g.kind = kind; g.find = find; g.replace = kind == "note" ? "" : replace; g.why = why;
                std::string id = store.add_suggestion(ctx.session, g);
                ok++;
                out += "Stored " + id + " (" + kind + ")\n";
            }
            out += std::to_string(ok) + " stored, " + std::to_string(bad) + " rejected.";
            return {ok > 0, out, false};
        };
        reg.add(std::move(t));
    }
}

}  // namespace judas::cowork
