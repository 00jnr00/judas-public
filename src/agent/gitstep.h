// git-per-change: record an agent's file edit as a commit on a dedicated ref (refs/<prefix><session>), without
// ever touching the user's checked-out branch, working tree or real index. The commit is built with a temporary
// index (GIT_INDEX_FILE), so a parallel worker, the user's own git commands and our commits cannot fight over
// .git/index.lock. Calls are serialized per repository and every child process is waited for.
#pragma once

#include <string>
#include <vector>

namespace judas {

struct GitStepResult {
    bool committed = false;   // a new commit was made
    std::string commit;       // its sha
    std::string ref;          // e.g. refs/judas/ab12cd34
    std::string error;        // set when git failed or cwd is not in a repo (not an error for the caller)
};

// paths: the files this step changed (absolute, or relative to cwd). Only those are staged, so build output and
// other edits are never swept in. With no paths, everything not ignored is staged, minus common build dirs
// (target, build*, node_modules, __pycache__, .venv, .cache). session_short names the ref.
GitStepResult git_commit_step(const std::string & cwd, const std::string & ref_prefix,
                              const std::string & session_short, const std::string & message,
                              const std::vector<std::string> & paths);

}  // namespace judas
