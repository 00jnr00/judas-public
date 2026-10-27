// Memory: one Markdown file with "## " sections that the agents (and any other assistant sharing it) read and append to.
// The prompt block (chosen sections, capped) goes in the system prompt; memory_read / memory_search /
// memory_append reach the rest. Pure text helpers are exposed for tests.
#pragma once

#include <string>
#include <vector>

#include "core/config.h"
#include "tools/tools.h"

namespace judas {

void register_memory_tools(ToolRegistry & reg);

// Names of the "## " sections, in file order.
std::vector<std::string> memory_section_names(const std::string & text);
// A section's text including its "## " heading and any "### " subsections ("" if there is none; name match
// ignores case).
std::string memory_section_text(const std::string & text, const std::string & name);
// Insert "- <date> [judas] <fact>" at the end of the section; a missing section is created before "## Log"
// (or at the end of the file when there is no Log). `line` is the complete bullet.
std::string memory_insert_bullet(const std::string & text, const std::string & section, const std::string & line);
// Bullets (lines starting with "- ") containing query (case-insensitive), as "[Section] bullet", at most max.
std::vector<std::string> memory_search_text(const std::string & text, const std::string & query, size_t max = 40);
// "" if the fact is acceptable; otherwise the reason it is rejected (reject_dashes and it has an em or en dash).
std::string memory_check_fact(const std::string & fact, bool reject_dashes = true);

// Remove the first line equal to `line` (both trimmed); *removed says whether one was found.
std::string memory_remove_line(const std::string & text, const std::string & line, bool * removed);
// True for a bullet memory_append wrote: "- YYYY-MM-DD [judas] ...". A line that only mentions [judas] is not.
bool memory_is_judas_line(const std::string & line);
// Remove one [judas]-tagged bullet from cfg.memory.path under the same lock memory_append uses. Bullets other
// assistants wrote are refused. Returns "" on success, else the reason.
std::string memory_forget(const Config & cfg, const std::string & line);

// The system-prompt block for cfg.memory.inject_sections, capped at cfg.prompt.max_memory_tokens (~4 bytes per
// token). "" when the file or sections are missing. Re-reads the file when its mtime changes.
std::string memory_prompt_block(const Config & cfg);

}  // namespace judas
