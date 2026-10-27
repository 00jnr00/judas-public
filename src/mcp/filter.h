// Which tools an agent role may see, and how many MCP tools one agent gets.
//
// A role's tool list (frontmatter `tools:`) is a whitelist, as before, with these additions:
//   name or glob      "read_file", "docs__*"          match tool names
//   mcp  /  mcp:*     every MCP tool
//   mcp:<server>      every tool of one server
//   !<entry>          exclude: "!mcp:*", "!docs__convert*", "!shell"
// No list (or a list of only "!" entries) means "everything not excluded". A tool that matches an exclusion is
// never visible. MCP tools are not in a role's whitelist unless an entry above (or their exact name) includes them.
#pragma once

#include <string>
#include <vector>

#include "tools/tools.h"

namespace judas::mcp {

bool role_allows(const std::vector<std::string> & role_tools, const Tool & t);

}  // namespace judas::mcp
