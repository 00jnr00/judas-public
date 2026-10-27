#include "agent/gitstep.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>

extern char ** environ;

namespace judas {

namespace fs = std::filesystem;

namespace {

struct Out {
    int code = -1;
    std::string text;
};

// posix_spawn git with a pipe for stdout+stderr, then waitpid. Safe in a multithreaded process (no bare fork).
Out run_git(const std::string & cwd, const std::vector<std::string> & args, const std::vector<std::string> & env_extra) {
    Out out;
    int fds[2];
    if (pipe2(fds, O_CLOEXEC) != 0) return out;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);

    std::vector<std::string> argv_s = {"git", "-C", cwd};
    argv_s.insert(argv_s.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto & s : argv_s) argv.push_back(const_cast<char *>(s.c_str()));
    argv.push_back(nullptr);
    std::vector<std::string> env_s;
    for (char ** e = environ; e && *e; e++) env_s.push_back(*e);
    for (auto & e : env_extra) env_s.push_back(e);  // later entries win in glibc getenv; also strip duplicates below
    std::vector<char *> envp;
    for (size_t i = 0; i < env_s.size(); i++) {
        std::string key = env_s[i].substr(0, env_s[i].find('='));
        bool shadowed = false;
        for (size_t j = i + 1; j < env_s.size(); j++)
            if (env_s[j].substr(0, env_s[j].find('=')) == key) shadowed = true;
        if (!shadowed) envp.push_back(const_cast<char *>(env_s[i].c_str()));
    }
    envp.push_back(nullptr);

    pid_t pid;
    int rc = posix_spawnp(&pid, "git", &fa, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return out;
    }
    char buf[4096];
    for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) out.text.append(buf, (size_t) n);
    close(fds[0]);
    int st = 0;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR) {}
    out.code = WIFEXITED(st) ? WEXITSTATUS(st) : -1;
    while (!out.text.empty() && (out.text.back() == '\n' || out.text.back() == '\r')) out.text.pop_back();
    return out;
}

std::mutex & repo_mutex(const std::string & key) {
    static std::mutex table_mu;
    static std::map<std::string, std::unique_ptr<std::mutex>> table;
    std::lock_guard lk(table_mu);
    auto & m = table[key];
    if (!m) m = std::make_unique<std::mutex>();
    return *m;
}

}  // namespace

GitStepResult git_commit_step(const std::string & cwd, const std::string & ref_prefix,
                              const std::string & session_short, const std::string & message,
                              const std::vector<std::string> & paths) {
    GitStepResult r;
    auto top = run_git(cwd, {"rev-parse", "--show-toplevel"}, {});
    if (top.code != 0) {
        r.error = "not a git repository";
        return r;
    }
    auto gd = run_git(cwd, {"rev-parse", "--absolute-git-dir"}, {});
    if (gd.code != 0) {
        r.error = "no git dir";
        return r;
    }
    r.ref = ref_prefix.rfind("refs/", 0) == 0 ? ref_prefix + session_short : "refs/" + ref_prefix + session_short;
    std::lock_guard lk(repo_mutex(gd.text));

    std::string index = gd.text + "/judas-index-" + session_short;
    std::vector<std::string> env = {"GIT_INDEX_FILE=" + index, "GIT_AUTHOR_NAME=Judas",
                                    "GIT_AUTHOR_EMAIL=judas@localhost", "GIT_COMMITTER_NAME=Judas",
                                    "GIT_COMMITTER_EMAIL=judas@localhost"};
    std::error_code ec;
    fs::remove(index, ec);
    struct Cleanup {
        std::string p;
        ~Cleanup() {
            std::error_code e;
            fs::remove(p, e);
        }
    } cleanup{index};

    // Parent: the session ref if it exists, else the current HEAD (so the ref starts from where the user is).
    std::string old_ref;
    auto cur = run_git(cwd, {"rev-parse", "--verify", "-q", r.ref}, env);
    std::string parent;
    if (cur.code == 0) {
        old_ref = parent = cur.text;
    } else {
        auto head = run_git(cwd, {"rev-parse", "--verify", "-q", "HEAD"}, env);
        if (head.code == 0) parent = head.text;
    }
    if (!parent.empty()) {
        auto rt = run_git(cwd, {"read-tree", parent}, env);
        if (rt.code != 0) {
            r.error = "read-tree: " + rt.text;
            return r;
        }
    }

    std::vector<std::string> add = {"add", "-A", "--"};
    if (paths.empty()) add.push_back(".");
    for (auto & p : paths) add.push_back(p);
    for (const char * x : {"target", "build*", "node_modules", "__pycache__", ".venv", ".cache"})
        add.push_back(std::string(":(exclude,glob)**/") + x + "/**");
    auto ad = run_git(cwd, add, env);
    if (ad.code != 0) {
        r.error = "add: " + ad.text;
        return r;
    }
    auto wt = run_git(cwd, {"write-tree"}, env);
    if (wt.code != 0) {
        r.error = "write-tree: " + wt.text;
        return r;
    }
    if (!parent.empty()) {
        auto pt = run_git(cwd, {"rev-parse", parent + "^{tree}"}, env);
        if (pt.code == 0 && pt.text == wt.text) return r;  // nothing changed
    }
    std::vector<std::string> ct = {"commit-tree", wt.text, "-m", message};
    if (!parent.empty()) ct.insert(ct.end(), {"-p", parent});
    auto c = run_git(cwd, ct, env);
    if (c.code != 0) {
        r.error = "commit-tree: " + c.text;
        return r;
    }
    std::vector<std::string> ur = {"update-ref", r.ref, c.text};
    ur.push_back(old_ref.empty() ? std::string(40, '0') : old_ref);  // compare-and-swap
    auto u = run_git(cwd, ur, env);
    if (u.code != 0) {
        r.error = "update-ref: " + u.text;
        return r;
    }
    r.committed = true;
    r.commit = c.text;
    return r;
}

}  // namespace judas
