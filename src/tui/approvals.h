// Inline approvals for the terminal UI: the queue of pending requests, how a request is printed, and the
// single-key state machine of the "Approve?" prompt. Pure (no terminal, no network), unit tested in
// tests/test_tui_approvals.cpp. The daemon side is documented in docs/API.md (Approvals).
#pragma once
#include <string>
#include <vector>

#include "core/json.h"
#include "tui/input.h"
#include "tui/text.h"

namespace judas::tui {

struct ApprovalReq {
    std::string id, agent, role, tool, what;
    bool strict = false;
    long long created_ms = 0;
};

// From an SSE frame ({type, agent, role, data:{id,tool,what,strict,created_ms}}) or a list entry
// (GET /api/sessions/<id>/approvals). Empty id = not an approval.
ApprovalReq approval_from_json(const judas::json & j);

// Pending requests for the current session, oldest first; the front one is the one being asked.
class ApprovalQueue {
public:
    bool add(ApprovalReq r);                              // false when the id is already queued
    bool remove(const std::string & id);                  // true when it was queued
    void sync(const std::vector<ApprovalReq> & pending);  // adopt the daemon's list (drops ones it no longer has)
    void clear() { items_.clear(); }
    bool empty() const { return items_.empty(); }
    size_t size() const { return items_.size(); }
    const ApprovalReq * current() const { return items_.empty() ? nullptr : &items_.front(); }
    const std::vector<ApprovalReq> & items() const { return items_; }

private:
    std::vector<ApprovalReq> items_;
};

// Diff-aware body rows of the request text, wrapped to `width`: "+" lines in pal.ok, "-" lines in pal.err.
// The first line of `what` (the action summary) is the header and is not part of the body.
std::vector<std::string> approval_body_rows(const ApprovalReq & r, const Palette & pal, int width);
// The request as first printed: header, up to max_body body rows, a "more" hint when cut.
std::vector<std::string> approval_rows(const ApprovalReq & r, const Palette & pal, int width, int max_body = 12);
// Rows [from, from+n) of the full body for the pager; *next = the index to continue from.
std::vector<std::string> approval_page(const std::vector<std::string> & body, size_t from, int n, size_t * next);
// The one-line prompt under the input box. viewing = the pager is open (shown/total body rows).
std::string approval_prompt(const Palette & pal, size_t waiting_more, bool viewing, size_t shown, size_t total);
// Rows for /approvals.
std::vector<std::string> approval_list_rows(const ApprovalQueue & q, const Palette & pal, int width);

enum class ApprovalAct { None, Approve, Deny, ViewMore, StopView };
// y approves, n or Esc denies, v starts the pager and space or v continues it, q closes it. Anything else
// (Enter included, so a stray return never approves) is swallowed: typing is paused while a request waits.
ApprovalAct approval_key(bool viewing, const Key & k);

}  // namespace judas::tui
