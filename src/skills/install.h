// Skills install: copy an external skill into the skills dir after a guard scan.
#pragma once

#include <string>

namespace judas {

// Outcome of installing a skill.
struct InstallResult {
    bool installed;
    bool quarantined;
    std::string dest;
    std::string report;
};

// Install an external skill from src_path into skills_dir. After a guard scan:
// - blocked (any high finding) and not force -> write to <skills_dir>/.quarantine/<name>.md, quarantined = true.
// - otherwise -> write to <skills_dir>/<name>.md.
// The frontmatter source: is set to external:<absolute source path>.
InstallResult install_skill(const std::string & src_path, const std::string & skills_dir, bool force);

}  // namespace judas
