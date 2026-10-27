// Snapshot journal: every file a Judas tool changes is recorded first, so any step can be reverted
// by the user or by the model. Originals are stored content-addressed (sha256) under <root>/objects,
// copied with FICLONE reflinks (instant and free on btrfs until the file changes), plain copy elsewhere.
// One checkpoint = one agent step. Checkpoint metadata is JSON in <root>/checkpoints/<id>.json.
#pragma once

#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace judas {

struct FileChange {
    std::string path;            // absolute
    std::string before_hash;     // "" = file did not exist before
    std::string after_hash;      // "" = file does not exist after (deleted); filled by commit()
    bool before_is_dir = false;
};

struct CheckpointInfo {
    std::string id;              // sortable: "20261013-142233-0007"
    std::string session, agent, label;  // label = tool call summary, e.g. "edit_file ~/.config/waybar/style.css"
    long long created_ms = 0;
    bool committed = false;
    bool reverted = false;
    std::vector<FileChange> files;
    std::vector<std::string> irreversible;  // side effects a revert cannot undo ("systemctl --user restart waybar")
};

struct RevertResult {
    bool ok = true;
    std::vector<std::string> restored;   // paths written back
    std::vector<std::string> removed;    // paths deleted (they did not exist before)
    std::vector<std::string> conflicts;  // changed since the checkpoint by something else; restored anyway unless strict
    std::vector<std::string> irreversible;  // reported, not undone
    std::string error;
};

class Journal {
public:
    explicit Journal(std::filesystem::path root);   // creates root/objects and root/checkpoints

    // Start a checkpoint for one agent step. Thread-safe.
    std::string begin(const std::string & session, const std::string & agent, const std::string & label);
    // Call BEFORE modifying or deleting `path` (file or directory tree). Idempotent per checkpoint+path.
    void record(const std::string & checkpoint, const std::filesystem::path & path);
    void note_irreversible(const std::string & checkpoint, const std::string & what);
    // Hash the after-state of every recorded path and persist. A checkpoint with no files and no
    // irreversible notes is discarded.
    void commit(const std::string & checkpoint);

    std::vector<CheckpointInfo> list(const std::string & session = "", size_t limit = 50) const;  // newest first
    std::optional<CheckpointInfo> get(const std::string & id) const;
    // Unified diff (before -> current file content) for every file in the checkpoint; binary files summarized.
    std::string diff(const std::string & id) const;
    // Restore one checkpoint's before-state. strict = refuse if any file changed since commit.
    RevertResult revert(const std::string & id, bool strict = false);
    // Revert every committed checkpoint newer than or equal to id, newest first (optionally only one session).
    RevertResult revert_to(const std::string & id, const std::string & session = "");
    // Undo every change a session made: revert all its committed checkpoints, newest first.
    RevertResult revert_session(const std::string & session);
    // Keep the current on-disk state but drop this session's undo history: delete its checkpoints, then gc
    // orphaned objects. Returns bytes of objects freed. Used by "Seal" to stop snapshots bloating disk.
    size_t seal_session(const std::string & session);
    // Delete objects no checkpoint references and checkpoints older than keep_days.
    size_t gc(int keep_days = 30);

    const std::filesystem::path & root() const { return root_; }

private:
    std::filesystem::path root_;
    mutable std::mutex mu_;
};

// sha256 of a file's bytes as lowercase hex ("" if missing).
std::string sha256_file(const std::filesystem::path & p);
// Copy src to dst with FICLONE when possible, else a normal copy. Returns true on success.
bool reflink_copy(const std::filesystem::path & src, const std::filesystem::path & dst);

}  // namespace judas
