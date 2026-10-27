// Skills guard: static scan of a skill's Markdown for common shell/data pitfalls.
#pragma once

#include <string>
#include <vector>

namespace judas {

// A single detected issue. severity is one of: high, medium, low.
struct GuardFinding {
    std::string severity;  // high | medium | low
    std::string rule;      // short rule name
    int line;              // 1-based line number, 0 if not locatable
    std::string excerpt;   // trimmed snippet of the matched text
};

// Aggregate of findings. blocked() is true when at least one high-severity issue is present.
struct GuardReport {
    std::vector<GuardFinding> findings;
    bool blocked() const {
        for (const auto &f : findings)
            if (f.severity == "high") return true;
        return false;
    }
};

// Scan skill Markdown and return the detected findings. Known limit: this is a static text
// scan over the literal patterns in guard.cpp; it cannot follow dynamic destinations
// (a URL or path computed at runtime is invisible here).
// See docs/SKILLS.md for the rule list.
GuardReport scan_skill_text(const std::string & markdown);

}  // namespace judas
