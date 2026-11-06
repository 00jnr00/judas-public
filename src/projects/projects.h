// Projects: a name + a folder + optional instructions, attached to sessions (the folder becomes the session's cwd, the
// instructions go into the system prompt of every agent in the session). Two kinds: "existing" (any folder the user
// picks) and "made" (a new folder Judas creates under projects.root, default ~/Judas/Projects/<slug>/). Judas never
// deletes a project folder on its own: deleting a project removes the record only, unless the caller asks for the
// folder explicitly (delete_project with delete_folder, which the API only allows with a second confirmation).
#pragma once

#include <optional>
#include <string>

#include "core/config.h"
#include "store/store.h"

namespace judas::projects {

// "My Cool Project!" -> "my-cool-project": lower case ASCII letters and digits joined by single dashes, at most 48
// characters, never empty ("project" when nothing usable is left).
std::string slugify(const std::string & name);

// root/slug, or root/slug-2, root/slug-3 ... when that path already exists. root is expanded (~).
std::string unique_folder(const std::string & root, const std::string & name);

struct Result {
    std::optional<ProjectRow> project;
    std::string error;   // set when project is empty
};

// mode "make": creates a fresh folder under cfg.projects.root (kind "made"). mode "existing": folder must be an
// absolute path (~ allowed) to a directory that exists; nothing is created (kind "existing").
Result create(Store & store, const Config & cfg, const std::string & name, const std::string & mode,
              const std::string & folder, const std::string & instructions);

// Make sure the project's folder is there; a "made" project whose folder vanished gets it back. "" if usable, else
// the reason.
std::string ensure_folder(const ProjectRow & p);

// "" if the path is safe to remove recursively as a project folder, else why not (home, root, system dirs, shallow paths).
std::string folder_delete_refusal(const std::string & folder);

struct DeleteResult {
    bool ok = false;
    bool folder_removed = false;
    std::string error;
};
// Removes the record (its sessions stay, detached). The folder is touched only when delete_folder is true and
// folder_delete_refusal says it is safe.
DeleteResult remove(Store & store, const std::string & id, bool delete_folder);

// The project a session belongs to, if any.
std::optional<ProjectRow> of_session(const Store & store, const std::string & session_id);

// The system-prompt block for a project ("" when it has no instructions to add beyond the folder line).
std::string prompt_block(const ProjectRow & p);

}  // namespace judas::projects
