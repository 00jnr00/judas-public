// Event-to-render mapping for the terminal UI. A Stream consumes Judas events (SSE frames, or stored
// messages when a session is loaded) and produces (a) committed lines, printed once into the terminal's
// normal scrollback, and (b) the live rows (running tools, the partial line being streamed, thinking,
// workers, the spinner) that are redrawn in place above the input box. No terminal access: unit tested in
// tests/test_tui_view.cpp.
#pragma once
#include <deque>
#include <optional>
#include <string>
#include <vector>

#include "core/json.h"
#include "tui/text.h"

namespace judas::tui {

struct ToolLog {
    int n = 0;
    std::string name, label, output;
    bool ok = true;
};

struct AgentInfo {
    std::string id, parent, role, status = "queued", task, activity;
};

struct Approval {
    std::string id, what;
};

// A short human label for a tool call's arguments: the path/command/pattern, else compact JSON.
std::string tool_label(const std::string & name, const judas::json & args, int max_cols = 90);
// First non-empty line of tool output plus a count of the rest: "42 lines" style summary.
std::string output_summary(const std::string & out, int max_cols, int * total_lines = nullptr);

class Stream {
public:
    Stream(Palette pal, int width);
    void set_width(int w) { width_ = w < 20 ? 20 : w; }
    int width() const { return width_; }
    void set_verbose(bool v) { verbose_ = v; }
    bool verbose() const { return verbose_; }
    void reset();                                   // new or switched session: drop all state

    // Commits the "> text" block and ends the previous reply. Attachment markers in `text` are collapsed to chips;
    // `chips` are extra chip lines (attachments sent with this message), shown dim.
    void user_message(const std::string & text, const std::vector<std::string> & chips = {});
    void on_event(const judas::json & ev);          // one SSE frame
    // Stored messages of a session (GET /api/sessions/<id> messages + agents) rendered as scrollback.
    // Messages with id > max_id are skipped (a turn in progress is replayed from events instead); max_id < 0 = all.
    void load_history(const judas::json & messages, const judas::json & agents, long long max_id = -1);
    void note(const std::string & text, char level = 'd');  // commit one line: d dim, w warn, e error, n normal
    void commit(const std::vector<std::string> & rows);

    std::vector<std::string> take_committed();
    std::vector<std::string> live(int spinner_frame, double elapsed_s) const;

    // state for the status line and commands
    bool running() const { return running_; }
    const std::string & turn_status() const { return turn_status_; }  // idle running paused done failed killed
    const std::string & pause_reason() const { return reason_; }
    const std::string & plan() const { return plan_; }
    const std::vector<AgentInfo> & agents() const { return agents_; }
    long long turn_tokens() const { return turn_tokens_; }
    std::vector<std::string> replies() const;       // oldest first, including the one in progress
    const ToolLog * tool(int n) const;              // n <= 0: the last one
    std::vector<std::string> expand_rows(int n) const;
    std::optional<Approval> approval;
    void set_running(bool r);                       // the TUI sets it on send and on a closed stream

private:
    struct Pending {
        ToolLog log;
        std::string id, agent;
        int indent = 0;
        bool done = false;
    };
    Palette pal_;
    int width_;
    bool verbose_ = false, running_ = false, text_started_ = false, hinted_ = false;
    std::string turn_status_ = "idle", reason_, plan_, tbuf_, rbuf_, cur_reply_;
    long long turn_tokens_ = 0;
    int tool_counter_ = 0;
    MdState md_;
    std::deque<Pending> pending_;
    std::deque<ToolLog> log_;
    std::vector<AgentInfo> agents_;
    std::vector<std::string> replies_, out_;

    AgentInfo & agent_of(const judas::json & ev);
    void flush_text(bool all);
    void flush_reasoning();
    void end_reply();
    void add_text(const std::string & t);
    void tool_call(const std::string & agent, bool lead, const std::string & id, const std::string & name,
                   const judas::json & args);
    void tool_result(const std::string & agent, const std::string & id, const std::string & name, bool ok,
                     const std::string & output);
    void commit_done_tools();
    std::vector<std::string> tool_rows(const Pending & p) const;
    void end_turn(const std::string & status, const std::string & reason);
};

}  // namespace judas::tui
