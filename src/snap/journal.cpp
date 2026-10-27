// Snapshot journal implementation. See journal.h for the contract.
//
// Layout:  <root>/objects/<sha256 hex>      file bytes (reflinked); symlink targets live in objects/l-<hex>
//          <root>/checkpoints/<id>.json     metadata (files, modes, notes, reverted flag)
//
// Directory rule: record(dir) records every regular file, symlink and sub-directory inside it, plus the
// directory itself (before_is_dir). Revert restores those and recreates empty dirs. Files that were created
// inside the directory LATER were not in the before-set and are not recorded, so revert leaves them alone
// (tools that create files should record() that path first, which stores "did not exist"). A recorded path
// that did not exist and is now a directory is removed only if it is empty after the rest is reverted.
#include "snap/journal.h"

#include <fcntl.h>
#include <linux/fs.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <set>
#include <sstream>

#include "core/json.h"

extern "C" {
#include "sha256/sha256.h"
}

namespace fs = std::filesystem;

namespace judas {

namespace {

using json = judas::json;

long long now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string to_hex(const unsigned char * d, size_t n) {
    static const char * k = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) { s += k[d[i] >> 4]; s += k[d[i] & 15]; }
    return s;
}

std::string sha256_bytes(const std::string & data) {
    sha256_t c;
    sha256_init(&c);
    sha256_update(&c, reinterpret_cast<const unsigned char *>(data.data()), data.size());
    unsigned char dg[SHA256_DIGEST_SIZE];
    sha256_final(&c, dg);
    return to_hex(dg, sizeof dg);
}

// In-memory state of uncommitted checkpoints (the header has no room for it). Keyed by root + id.
struct Pending {
    CheckpointInfo info;
    std::map<std::string, unsigned> modes;  // path -> st_mode & 07777
    std::set<std::string> seen;             // recorded paths (idempotence)
};
std::mutex g_pend_mu;
std::map<std::string, Pending> g_pending;
std::atomic<unsigned> g_counter{0};

std::string pkey(const fs::path & root, const std::string & id) { return root.string() + "\n" + id; }

fs::path obj_path(const fs::path & root, const std::string & h) { return root / "objects" / h; }
fs::path cp_path(const fs::path & root, const std::string & id) { return root / "checkpoints" / (id + ".json"); }

bool exists_nofollow(const fs::path & p) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    return !ec && st.type() != fs::file_type::not_found;
}

// Content-addressed store of a regular file; returns its hash ("" on failure).
std::string store_file(const fs::path & root, const fs::path & p) {
    std::string h = sha256_file(p);
    if (h.empty()) return "";
    fs::path dst = obj_path(root, h);
    if (exists_nofollow(dst)) return h;
    static std::atomic<unsigned> n{0};
    fs::path tmp = root / "objects" / (".tmp-" + std::to_string(getpid()) + "-" + std::to_string(n++));
    if (!reflink_copy(p, tmp)) { std::error_code ec; fs::remove(tmp, ec); return ""; }
    std::string h2 = sha256_file(tmp);  // the file may have changed between hash and copy
    std::error_code ec;
    if (h2.empty()) { fs::remove(tmp, ec); return ""; }
    fs::path dst2 = obj_path(root, h2);
    if (exists_nofollow(dst2)) fs::remove(tmp, ec);
    else fs::rename(tmp, dst2, ec);
    return ec ? "" : h2;
}

std::string store_symlink(const fs::path & root, const fs::path & p) {
    std::error_code ec;
    std::string target = fs::read_symlink(p, ec).string();
    if (ec) return "";
    std::string h = "l-" + sha256_bytes(target);
    fs::path dst = obj_path(root, h);
    if (!exists_nofollow(dst)) {
        std::ofstream f(dst, std::ios::binary);
        f << target;
    }
    return h;
}

// Hash of the current state of a path: file hash, "l-<hex>" for symlinks, "dir", or "" if missing.
// store=true also puts the content into the object store.
std::string state_hash(const fs::path & root, const fs::path & p, bool store) {
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (ec || st.type() == fs::file_type::not_found) return "";
    if (st.type() == fs::file_type::symlink) {
        if (store) return store_symlink(root, p);
        return "l-" + sha256_bytes(fs::read_symlink(p, ec).string());
    }
    if (st.type() == fs::file_type::directory) return "dir";
    if (st.type() == fs::file_type::regular) return store ? store_file(root, p) : sha256_file(p);
    return "?";
}

unsigned mode_of(const fs::path & p) {
    struct stat sb;
    if (lstat(p.c_str(), &sb) != 0) return 0644;
    return sb.st_mode & 07777;
}

bool write_json_atomic(const fs::path & file, const json & j) {
    fs::path tmp = file;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << j.dump(1);
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, file, ec);
    return !ec;
}

json info_to_json(const CheckpointInfo & i, const std::map<std::string, unsigned> & modes) {
    json files = json::array();
    for (auto & f : i.files) {
        json o = {{"path", f.path}, {"before", f.before_hash}, {"after", f.after_hash}, {"dir", f.before_is_dir}};
        auto it = modes.find(f.path);
        if (it != modes.end()) o["mode"] = it->second;
        files.push_back(o);
    }
    return {{"id", i.id}, {"session", i.session}, {"agent", i.agent}, {"label", i.label},
            {"created_ms", i.created_ms}, {"committed", i.committed}, {"reverted", i.reverted},
            {"files", files}, {"irreversible", i.irreversible}};
}

// Ids and object names come back through the API and tools, so only plain names are accepted: no "/", no
// "..", no absolute paths that would let root / "objects" / h escape the store.
bool safe_name(const std::string & s) {
    if (s.empty() || s.size() > 128 || s == "." || s == "..") return false;
    for (char c : s)
        if (!(std::isalnum((unsigned char) c) || c == '-' || c == '_' || c == '.')) return false;
    return s.find("..") == std::string::npos;
}

bool load_cp(const fs::path & root, const std::string & id, CheckpointInfo & out,
             std::map<std::string, unsigned> * modes = nullptr) {
    if (!safe_name(id)) return false;
    std::ifstream f(cp_path(root, id), std::ios::binary);
    if (!f) return false;
    try {
        json j = json::parse(f);
        out = CheckpointInfo{};
        out.id = j.value("id", id);
        out.session = j.value("session", "");
        out.agent = j.value("agent", "");
        out.label = j.value("label", "");
        out.created_ms = j.value("created_ms", 0LL);
        out.committed = j.value("committed", false);
        out.reverted = j.value("reverted", false);
        for (auto & o : j.value("files", json::array())) {
            FileChange fc;
            fc.path = o.value("path", "");
            fc.before_hash = o.value("before", "");
            fc.after_hash = o.value("after", "");
            fc.before_is_dir = o.value("dir", false);
            out.files.push_back(fc);
            if (modes && o.contains("mode")) (*modes)[fc.path] = o["mode"].get<unsigned>();
        }
        for (auto & s : j.value("irreversible", json::array())) out.irreversible.push_back(s.get<std::string>());
        return true;
    } catch (...) {
        return false;
    }
}

std::vector<std::string> list_ids(const fs::path & root) {
    std::vector<std::string> ids;
    std::error_code ec;
    for (auto & e : fs::directory_iterator(root / "checkpoints", ec)) {
        auto n = e.path().filename().string();
        if (n.size() > 5 && n.ends_with(".json")) ids.push_back(n.substr(0, n.size() - 5));
    }
    std::sort(ids.begin(), ids.end(), std::greater<>());
    return ids;
}

// ---------- line diff ----------

std::vector<std::string> split_lines(const std::string & s) {
    std::vector<std::string> v;
    size_t i = 0;
    while (i < s.size()) {
        size_t j = s.find('\n', i);
        if (j == std::string::npos) { v.push_back(s.substr(i)); break; }
        v.push_back(s.substr(i, j - i + 1));
        i = j + 1;
    }
    return v;
}

struct Op { char t; const std::string * line; };

std::vector<Op> diff_ops(const std::vector<std::string> & a, const std::vector<std::string> & b) {
    size_t p = 0;
    while (p < a.size() && p < b.size() && a[p] == b[p]) p++;
    size_t s = 0;
    while (s < a.size() - p && s < b.size() - p && a[a.size() - 1 - s] == b[b.size() - 1 - s]) s++;
    size_t n = a.size() - p - s, m = b.size() - p - s;
    std::vector<Op> ops;
    for (size_t i = 0; i < p; i++) ops.push_back({' ', &a[i]});
    if (n * m > 4000000) {  // too big for the table: show the middle as replaced
        for (size_t i = 0; i < n; i++) ops.push_back({'-', &a[p + i]});
        for (size_t j = 0; j < m; j++) ops.push_back({'+', &b[p + j]});
    } else {
        // lcs[i][j] = LCS length of a[p+i..] and b[p+j..]
        std::vector<uint32_t> t((n + 1) * (m + 1), 0);
        auto at = [&](size_t i, size_t j) -> uint32_t & { return t[i * (m + 1) + j]; };
        for (size_t i = n; i-- > 0;)
            for (size_t j = m; j-- > 0;)
                at(i, j) = a[p + i] == b[p + j] ? at(i + 1, j + 1) + 1 : std::max(at(i + 1, j), at(i, j + 1));
        size_t i = 0, j = 0;
        while (i < n || j < m) {
            if (i < n && j < m && a[p + i] == b[p + j]) { ops.push_back({' ', &a[p + i]}); i++; j++; }
            else if (i < n && (j == m || at(i + 1, j) >= at(i, j + 1))) { ops.push_back({'-', &a[p + i]}); i++; }
            else { ops.push_back({'+', &b[p + j]}); j++; }
        }
    }
    for (size_t i = 0; i < s; i++) ops.push_back({' ', &a[a.size() - s + i]});
    return ops;
}

void emit_line(std::string & out, char t, const std::string & l) {
    out += t;
    out += l;
    if (l.empty() || l.back() != '\n') out += "\n\\ No newline at end of file\n";
}

std::string unified(const std::string & oldname, const std::string & newname,
                    const std::string & before, const std::string & after) {
    auto a = split_lines(before), b = split_lines(after);
    auto ops = diff_ops(a, b);
    const int ctx = 3;
    std::vector<size_t> ch;
    for (size_t i = 0; i < ops.size(); i++) if (ops[i].t != ' ') ch.push_back(i);
    if (ch.empty()) return "";
    // cumulative old/new line counts before each op
    std::vector<size_t> op_old(ops.size() + 1, 0), op_new(ops.size() + 1, 0);
    for (size_t i = 0; i < ops.size(); i++) {
        op_old[i + 1] = op_old[i] + (ops[i].t != '+');
        op_new[i + 1] = op_new[i] + (ops[i].t != '-');
    }
    std::string out = "--- " + oldname + "\n+++ " + newname + "\n";
    size_t g = 0;
    while (g < ch.size()) {
        size_t first = ch[g], last = ch[g];
        while (g + 1 < ch.size() && ch[g + 1] - last <= (size_t)(2 * ctx)) last = ch[++g];
        g++;
        size_t lo = first > (size_t)ctx ? first - ctx : 0;
        size_t hi = std::min(ops.size(), last + ctx + 1);
        size_t ol = op_old[hi] - op_old[lo], nl = op_new[hi] - op_new[lo];
        size_t os = ol == 0 ? op_old[lo] : op_old[lo] + 1, ns = nl == 0 ? op_new[lo] : op_new[lo] + 1;
        out += "@@ -" + std::to_string(os) + "," + std::to_string(ol) + " +" + std::to_string(ns) + "," +
               std::to_string(nl) + " @@\n";
        for (size_t i = lo; i < hi; i++) emit_line(out, ops[i].t, *ops[i].line);
    }
    return out;
}

bool read_all(const fs::path & p, std::string & out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    std::ostringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

bool looks_binary(const std::string & s) {
    return std::memchr(s.data(), 0, std::min<size_t>(s.size(), 8192)) != nullptr;
}

// ---------- revert core (caller holds the lock) ----------

RevertResult revert_locked(const fs::path & root, const std::string & id, bool strict) {
    RevertResult r;
    CheckpointInfo cp;
    std::map<std::string, unsigned> modes;
    if (!load_cp(root, id, cp, &modes)) { r.ok = false; r.error = "no such checkpoint: " + id; return r; }
    if (!cp.committed) { r.ok = false; r.error = "checkpoint not committed: " + id; return r; }
    if (cp.reverted) { r.ok = false; r.error = "already reverted: " + id; return r; }
    r.irreversible = cp.irreversible;

    for (auto & f : cp.files) {
        if (f.before_is_dir || (f.before_hash.empty() && f.after_hash == "dir")) continue;
        if (state_hash(root, f.path, false) != f.after_hash) r.conflicts.push_back(f.path);
    }
    if (strict && !r.conflicts.empty()) {
        r.ok = false;
        r.error = "strict revert refused: " + std::to_string(r.conflicts.size()) + " file(s) changed since the checkpoint";
        return r;
    }

    std::error_code ec;
    std::vector<const FileChange *> gone_dirs;
    for (auto & f : cp.files) {
        fs::path p = f.path;
        if (f.before_is_dir) {
            if (!exists_nofollow(p)) {
                fs::create_directories(p, ec);
                if (ec) { r.ok = false; r.error = "cannot create " + f.path + ": " + ec.message(); continue; }
                auto m = modes.find(f.path);
                if (m != modes.end()) ::chmod(p.c_str(), m->second);
                r.restored.push_back(f.path);
            }
            continue;
        }
        if (f.before_hash.empty()) {
            auto st = fs::symlink_status(p, ec);
            if (st.type() == fs::file_type::directory) { gone_dirs.push_back(&f); continue; }
            if (st.type() != fs::file_type::not_found && !ec) {
                fs::remove(p, ec);
                if (ec) { r.ok = false; r.error = "cannot remove " + f.path + ": " + ec.message(); }
                else r.removed.push_back(f.path);
            }
            continue;
        }
        // restore content
        if (!safe_name(f.before_hash)) { r.ok = false; r.error = "bad object name in checkpoint for " + f.path; continue; }
        fs::path obj = obj_path(root, f.before_hash);
        if (!exists_nofollow(obj)) { r.ok = false; r.error = "missing object " + f.before_hash + " for " + f.path; continue; }
        fs::create_directories(p.parent_path(), ec);
        if (exists_nofollow(p)) {
            auto st = fs::symlink_status(p, ec);
            // A directory now sits where the file was: never delete it (it may hold files the journal never saw).
            if (st.type() == fs::file_type::directory) {
                r.ok = false;
                r.error = "not restored: " + f.path + " is now a directory; move it away and revert again";
                continue;
            }
            fs::remove(p, ec);
        }
        if (f.before_hash.rfind("l-", 0) == 0) {
            std::string target;
            read_all(obj, target);
            fs::create_symlink(target, p, ec);
            if (ec) { r.ok = false; r.error = "cannot restore symlink " + f.path + ": " + ec.message(); continue; }
        } else {
            if (!reflink_copy(obj, p)) { r.ok = false; r.error = "cannot restore " + f.path; continue; }
            auto m = modes.find(f.path);
            if (m != modes.end()) ::chmod(p.c_str(), m->second);
        }
        r.restored.push_back(f.path);
    }
    // directories that did not exist before: remove only if now empty (deepest first)
    std::sort(gone_dirs.begin(), gone_dirs.end(),
              [](auto * a, auto * b) { return a->path.size() > b->path.size(); });
    for (auto * f : gone_dirs) {
        if (fs::is_empty(f->path, ec) && !ec && fs::remove(f->path, ec)) r.removed.push_back(f->path);
    }

    // A partial revert stays retryable: only a clean pass marks the checkpoint reverted.
    if (r.ok) {
        cp.reverted = true;
        write_json_atomic(cp_path(root, id), info_to_json(cp, modes));
    }
    return r;
}

}  // namespace

// ---------- free functions ----------

std::string sha256_file(const fs::path & p) {
    int fd = ::open(p.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    sha256_t c;
    sha256_init(&c);
    std::vector<unsigned char> buf(1 << 20);
    for (;;) {
        ssize_t n = ::read(fd, buf.data(), buf.size());
        if (n < 0) { if (errno == EINTR) continue; ::close(fd); return ""; }
        if (n == 0) break;
        sha256_update(&c, buf.data(), (size_t)n);
    }
    ::close(fd);
    unsigned char dg[SHA256_DIGEST_SIZE];
    sha256_final(&c, dg);
    return to_hex(dg, sizeof dg);
}

bool reflink_copy(const fs::path & src, const fs::path & dst) {
    int in = ::open(src.c_str(), O_RDONLY | O_CLOEXEC);
    if (in < 0) return false;
    struct stat sb;
    if (fstat(in, &sb) != 0) { ::close(in); return false; }
    mode_t mode = sb.st_mode & 07777;
    int out = ::open(dst.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode | 0200);
    if (out < 0) { ::close(in); return false; }
    int rc = ::ioctl(out, FICLONE, in);
    ::close(in);
    bool ok = rc == 0;
    int err = errno;
    ::close(out);
    if (!ok) {
        // EOPNOTSUPP / EXDEV / EINVAL (and anything else): plain copy
        (void)err;
        std::error_code ec;
        ok = fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec) && !ec;
        if (!ok) return false;
    }
    ::chmod(dst.c_str(), mode);
    return true;
}

// ---------- Journal ----------

Journal::Journal(fs::path root) : root_(std::move(root)) {
    std::error_code ec;
    fs::create_directories(root_ / "objects", ec);
    fs::create_directories(root_ / "checkpoints", ec);
}

std::string Journal::begin(const std::string & session, const std::string & agent, const std::string & label) {
    std::lock_guard<std::mutex> lk(mu_);
    std::lock_guard<std::mutex> lk2(g_pend_mu);
    auto t = std::chrono::system_clock::now();
    std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    localtime_r(&tt, &tm);
    char stamp[32];
    std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    // counter: above the process counter and above any existing id in the same second
    unsigned n = g_counter.load() + 1;
    std::error_code ec;
    std::string prefix = std::string(stamp) + "-";
    for (auto & e : fs::directory_iterator(root_ / "checkpoints", ec)) {
        auto name = e.path().filename().string();
        if (name.rfind(prefix, 0) == 0 && name.size() >= prefix.size() + 4) {
            try { n = std::max(n, (unsigned)std::stoul(name.substr(prefix.size(), 4)) + 1); } catch (...) {}
        }
    }
    for (auto & kv : g_pending) {
        if (kv.second.info.id.rfind(prefix, 0) == 0 && kv.first.rfind(root_.string() + "\n", 0) == 0) {
            try { n = std::max(n, (unsigned)std::stoul(kv.second.info.id.substr(prefix.size(), 4)) + 1); } catch (...) {}
        }
    }
    g_counter = n;
    char num[16];
    std::snprintf(num, sizeof num, "%04u", n);
    Pending p;
    p.info.id = prefix + num;
    p.info.session = session;
    p.info.agent = agent;
    p.info.label = label;
    p.info.created_ms = now_ms();
    std::string id = p.info.id;
    g_pending[pkey(root_, id)] = std::move(p);
    return id;
}

void Journal::record(const std::string & checkpoint, const fs::path & path) {
    std::lock_guard<std::mutex> lk(mu_);
    std::lock_guard<std::mutex> lk2(g_pend_mu);
    auto it = g_pending.find(pkey(root_, checkpoint));
    if (it == g_pending.end()) return;
    Pending & pd = it->second;

    auto add = [&](const fs::path & p, bool is_dir) {
        std::string ps = p.string();
        if (!pd.seen.insert(ps).second) return;
        FileChange fc;
        fc.path = ps;
        fc.before_is_dir = is_dir;
        if (is_dir) {
            pd.modes[ps] = mode_of(p);
        } else {
            std::string h = state_hash(root_, p, true);
            fc.before_hash = h == "dir" ? "" : h;  // (not reached for dirs; defensive)
            if (!fc.before_hash.empty() && fc.before_hash.rfind("l-", 0) != 0) pd.modes[ps] = mode_of(p);
        }
        pd.info.files.push_back(fc);
    };

    fs::path p = fs::absolute(path).lexically_normal();
    if (!p.has_filename() && p.has_parent_path()) p = p.parent_path();
    std::error_code ec;
    auto st = fs::symlink_status(p, ec);
    if (!ec && st.type() == fs::file_type::directory) {
        add(p, true);
        for (fs::recursive_directory_iterator it2(p, fs::directory_options::skip_permission_denied, ec), end;
             !ec && it2 != end; it2.increment(ec)) {
            auto est = it2->symlink_status(ec);
            if (ec) { ec.clear(); continue; }
            if (est.type() == fs::file_type::directory) add(it2->path(), true);
            else if (est.type() == fs::file_type::regular || est.type() == fs::file_type::symlink) add(it2->path(), false);
        }
    } else {
        add(p, false);  // regular file, symlink, or missing
    }
}

void Journal::note_irreversible(const std::string & checkpoint, const std::string & what) {
    std::lock_guard<std::mutex> lk(mu_);
    std::lock_guard<std::mutex> lk2(g_pend_mu);
    auto it = g_pending.find(pkey(root_, checkpoint));
    if (it != g_pending.end()) it->second.info.irreversible.push_back(what);
}

void Journal::commit(const std::string & checkpoint) {
    std::lock_guard<std::mutex> lk(mu_);
    std::lock_guard<std::mutex> lk2(g_pend_mu);
    auto it = g_pending.find(pkey(root_, checkpoint));
    if (it == g_pending.end()) return;
    Pending pd = std::move(it->second);
    g_pending.erase(it);
    if (pd.info.files.empty() && pd.info.irreversible.empty()) return;
    for (auto & f : pd.info.files) {
        if (f.before_is_dir) continue;
        std::string h = state_hash(root_, f.path, true);
        f.after_hash = h;  // "dir" when a directory took the place of a file
    }
    pd.info.committed = true;
    write_json_atomic(cp_path(root_, pd.info.id), info_to_json(pd.info, pd.modes));
}

std::vector<CheckpointInfo> Journal::list(const std::string & session, size_t limit) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<CheckpointInfo> out;
    for (auto & id : list_ids(root_)) {
        if (out.size() >= limit) break;
        CheckpointInfo c;
        if (!load_cp(root_, id, c)) continue;
        if (!session.empty() && c.session != session) continue;
        out.push_back(std::move(c));
    }
    return out;
}

std::optional<CheckpointInfo> Journal::get(const std::string & id) const {
    std::lock_guard<std::mutex> lk(mu_);
    {
        std::lock_guard<std::mutex> lk2(g_pend_mu);
        auto it = g_pending.find(pkey(root_, id));
        if (it != g_pending.end()) return it->second.info;
    }
    CheckpointInfo c;
    if (!load_cp(root_, id, c)) return std::nullopt;
    return c;
}

std::string Journal::diff(const std::string & id) const {
    std::lock_guard<std::mutex> lk(mu_);
    CheckpointInfo cp;
    if (!load_cp(root_, id, cp)) return "no such checkpoint: " + id + "\n";
    std::string out;
    const size_t big = 2u << 20;
    for (auto & f : cp.files) {
        if (f.before_is_dir) continue;
        if (!f.before_hash.empty() && !safe_name(f.before_hash)) { out += "bad object name for " + f.path + "\n"; continue; }
        std::string cur = state_hash(root_, f.path, false);
        if (cur == f.before_hash) continue;
        bool sym_before = f.before_hash.rfind("l-", 0) == 0, sym_now = cur.rfind("l-", 0) == 0;
        if (sym_before || sym_now) {
            std::string a, b;
            if (sym_before) read_all(obj_path(root_, f.before_hash), a);
            std::error_code ec;
            if (sym_now) b = fs::read_symlink(f.path, ec).string();
            out += "symlink " + f.path + ": " + (sym_before ? a : "(none)") + " -> " + (sym_now ? b : "(none)") + "\n";
            continue;
        }
        if (cur == "dir") { out += "now a directory: " + f.path + "\n"; continue; }
        std::string before, after;
        bool has_before = !f.before_hash.empty(), has_after = !cur.empty();
        std::error_code ec;
        if (has_before && fs::file_size(obj_path(root_, f.before_hash), ec) > big) {
            out += "Large file differs: " + f.path + "\n"; continue;
        }
        if (has_after && fs::file_size(f.path, ec) > big) { out += "Large file differs: " + f.path + "\n"; continue; }
        if (has_before) read_all(obj_path(root_, f.before_hash), before);
        if (has_after) read_all(f.path, after);
        if (looks_binary(before) || looks_binary(after)) {
            out += "Binary file differs: " + f.path + "\n"; continue;
        }
        out += unified(has_before ? f.path : "/dev/null", has_after ? f.path : "/dev/null", before, after);
    }
    return out;
}

RevertResult Journal::revert(const std::string & id, bool strict) {
    std::lock_guard<std::mutex> lk(mu_);
    return revert_locked(root_, id, strict);
}

RevertResult Journal::revert_to(const std::string & id, const std::string & session) {
    std::lock_guard<std::mutex> lk(mu_);
    RevertResult all;
    for (auto & cid : list_ids(root_)) {  // newest first
        if (cid < id) break;
        CheckpointInfo c;
        if (!load_cp(root_, cid, c) || !c.committed || c.reverted) continue;
        if (!session.empty() && c.session != session) continue;
        RevertResult r = revert_locked(root_, cid, false);
        auto app = [](auto & dst, const auto & src) { dst.insert(dst.end(), src.begin(), src.end()); };
        app(all.restored, r.restored);
        app(all.removed, r.removed);
        app(all.conflicts, r.conflicts);
        app(all.irreversible, r.irreversible);
        if (!r.ok) { all.ok = false; all.error += (all.error.empty() ? "" : "; ") + r.error; }
    }
    return all;
}

RevertResult Journal::revert_session(const std::string & session) {
    std::lock_guard<std::mutex> lk(mu_);
    RevertResult all;
    for (auto & cid : list_ids(root_)) {  // newest first
        CheckpointInfo c;
        if (!load_cp(root_, cid, c) || !c.committed || c.reverted) continue;
        if (c.session != session) continue;
        RevertResult r = revert_locked(root_, cid, false);
        auto app = [](auto & dst, const auto & src) { dst.insert(dst.end(), src.begin(), src.end()); };
        app(all.restored, r.restored);
        app(all.removed, r.removed);
        app(all.conflicts, r.conflicts);
        app(all.irreversible, r.irreversible);
        if (!r.ok) { all.ok = false; all.error += (all.error.empty() ? "" : "; ") + r.error; }
    }
    return all;
}

size_t Journal::seal_session(const std::string & session) {
    std::lock_guard<std::mutex> lk(mu_);
    std::lock_guard<std::mutex> lk2(g_pend_mu);
    std::error_code ec;
    // Drop this session's checkpoints (keep the on-disk files as they are now).
    for (auto & id : list_ids(root_)) {
        CheckpointInfo c;
        if (!load_cp(root_, id, c) || c.session != session) continue;
        fs::remove(cp_path(root_, id), ec);
    }
    // Remove objects no remaining checkpoint (or pending op) references, summing bytes freed.
    std::set<std::string> used;
    for (auto & id : list_ids(root_)) {
        CheckpointInfo c;
        if (!load_cp(root_, id, c)) continue;
        for (auto & f : c.files) { used.insert(f.before_hash); used.insert(f.after_hash); }
    }
    for (auto & kv : g_pending) {
        if (kv.first.rfind(root_.string() + "\n", 0) != 0) continue;
        for (auto & f : kv.second.info.files) { used.insert(f.before_hash); used.insert(f.after_hash); }
    }
    size_t freed = 0;
    for (auto & e : fs::directory_iterator(root_ / "objects", ec)) {
        auto n = e.path().filename().string();
        if (n.rfind(".tmp-", 0) == 0 || used.count(n)) continue;
        auto sz = fs::file_size(e.path(), ec);
        if (fs::remove(e.path(), ec) && !ec) freed += (size_t) sz;
    }
    return freed;
}

size_t Journal::gc(int keep_days) {
    std::lock_guard<std::mutex> lk(mu_);
    std::lock_guard<std::mutex> lk2(g_pend_mu);
    long long cutoff = now_ms() - (long long)keep_days * 86400000LL;
    std::set<std::string> used;
    std::error_code ec;
    for (auto & id : list_ids(root_)) {
        CheckpointInfo c;
        if (!load_cp(root_, id, c)) continue;
        if (c.created_ms < cutoff) { fs::remove(cp_path(root_, id), ec); continue; }
        for (auto & f : c.files) { used.insert(f.before_hash); used.insert(f.after_hash); }
    }
    for (auto & kv : g_pending) {
        if (kv.first.rfind(root_.string() + "\n", 0) != 0) continue;
        for (auto & f : kv.second.info.files) { used.insert(f.before_hash); used.insert(f.after_hash); }
    }
    size_t removed = 0;
    std::vector<fs::path> victims;
    for (auto & e : fs::directory_iterator(root_ / "objects", ec)) {
        auto n = e.path().filename().string();
        if (n.rfind(".tmp-", 0) == 0) continue;  // in-flight store
        if (!used.count(n)) victims.push_back(e.path());
    }
    for (auto & v : victims) if (fs::remove(v, ec)) removed++;
    return removed;
}

}  // namespace judas
