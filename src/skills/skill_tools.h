// skill_manage: the agent creates and maintains its own skills (create, view, patch, rewrite, archive, pin).
#pragma once

#include "tools/tools.h"

namespace judas {

class Skills;
class SkillUsage;

// usage may be null (tracking disabled); skills must outlive the registry entry.
void register_skill_tools(ToolRegistry & reg, Skills & skills, SkillUsage * usage);

}  // namespace judas
