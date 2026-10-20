#pragma once

// AI assistant (the page's docked "🤖 Assistant" column; it replaced the AI
// Signal Lab): runs ai/kraken_ai.py - which drives Claude Code (or another LLM
// CLI) - once per question of a conversation. The agent answers questions
// about what is on screen and what was received, looks things up on the
// internet, writes decoder plugins, and controls the receiver through the MCP
// server in ai_mcp.cpp (the page's "Read only" switch turns control off). One
// question at a time, in its own process group so Stop kills the agent and
// everything it started.
//
// The bridge prints one JSON event per line ({"ev":...}); each is relayed to
// browsers as {"ai_event":{...}} and kept (last 400) so a page opened later
// sees the running answer. The assistant is OFF unless enabled on the Pi
// itself (`python3 ai/kraken_ai.py setup` writes ai/ai_config.json; Windows:
// `py -3 ai/kraken_ai.py setup` on the PC) - the web UI can't enable it, since
// it lets UI users run an agent and native code here.

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace kproc {
class Process;
struct ExitStatus;
}  // namespace kproc

class AiManager {
public:
    static AiManager& instance();
    // Loads the newest conversation; call once at startup
    void init();

    // All called on the uWS loop. They return "" or an error message.
    // A question in the current conversation (new_conv: start a new one);
    // view = the asking page's screen context (JSON object, kept for the
    // MCP server's get_screen while the question runs)
    // page = the asking page's id (its map / viewshed commands go to it)
    std::string ask(const std::string& text, const std::string& view_json, bool new_conv, const std::string& page = "");
    std::string test();
    void cancel();
    void new_conversation();       // the next question starts a new one

    bool enabled() const;          // ai/ai_config.json "enabled": true
    bool busy() const { return busy_.load(); }
    // "Read only" (AI_READONLY, persisted): the MCP control tools refuse
    bool readonly() const { return readonly_.load(); }
    void set_readonly(bool on);
    // the model and effort the page picked (AI_MODEL / AI_EFFORT, persisted;
    // "" = ai_config.json's model / the CLI's default effort). false = refused
    bool set_model(const std::string& m);
    bool set_effort(const std::string& e);
    static bool valid_model(const std::string& m);
    static bool valid_effort(const std::string& e);
    // a page announces itself (AI_PAGE:<id>); the page map / viewshed commands
    // go to: the asking page while a question runs, else the last announced
    void note_page(const std::string& id);
    std::string target_page() const;
    static bool valid_page_id(const std::string& id);
    // {"ai_state":{...}} - config, job, current conversation, recent events
    std::string state_json(bool with_log) const;
    static std::string plugins_message();   // {"plugins":[...]}

    // --- conversations (ai/sessions/<id>/) ---
    // The file work runs on a worker thread; results are broadcast:
    //   sessions_async()       -> {"ai_sessions":[{id,title,created,...}]}
    //   session_async(id)      -> {"ai_session":{...,"chat":[..],"activity":[..]}}
    //   delete_session(id)     -> removes it (and its Claude transcript), then
    //                             the new list; "" or an error
    //   select_session(id)     -> makes it the current one (questions go there)
    static void sessions_async();
    static void session_async(const std::string& id);
    std::string delete_session(const std::string& id);
    std::string select_session(const std::string& id);
    static bool valid_session_id(const std::string& id);

    // --- for the MCP server (ai_mcp.cpp) ---
    std::string view_json() const;           // the running question's screen context ("{}" = none)
    std::string conversation_dir() const;    // ai/sessions/<id> of the running question ("" = none)

    // --- identifications: what the assistant found out about a frequency
    // (ai/identifications.json, its remember_signal tool) - the spectrum's
    // "AI" badges and its find_identifications tool. JSON object fields:
    // id, hz, bw, name, summary, conv, t, brg (compass deg or null), conf
    std::string remember(double hz, double bw_hz, const std::string& name, const std::string& summary,
                         double bearing_deg, const std::string& confidence, std::string* id_out);
    std::string forget(const std::string& id);          // "" or an error
    // [{..}, ..] near hz (within within_hz; hz 0 = all) and/or matching query
    std::string identifications_json(double hz, double within_hz, const std::string& query, size_t limit) const;
    std::string marks_message() const;                   // {"ai_marks":[...]} for the page

private:
    AiManager() = default;
    ~AiManager();
    std::string start_job(const std::string& kind, const std::vector<std::string>& argv, const std::string& label);
    void reader(std::shared_ptr<kproc::Process> proc, std::string kind);
    void on_line(const std::string& kind, const std::string& line);
    void finished(const std::string& kind, const kproc::ExitStatus& status);
    void load_session(const std::string& id);   // current conversation <- session.json
    void load_marks();
    void save_marks_locked() const;

    mutable std::mutex mu_;
    std::atomic<bool> busy_{false};
    std::atomic<bool> readonly_{false};
    std::string model_, effort_;       // the page's picks (mu_)
    std::string asker_page_, last_page_;   // (mu_)
    std::thread thread_;
    // the running job (its own process group - Windows: job object), under
    // mu_; the reader thread holds it too and reaps it
    std::shared_ptr<kproc::Process> proc_;
    std::string job_kind_, job_label_;
    int64_t job_started_ms_ = 0;
    std::deque<std::string> log_;      // relayed event objects (JSON)
    uint64_t seq_ = 0;
    // current conversation
    std::string session_, title_, session_state_;
    bool fresh_ = true;                // the next question starts a new conversation
    std::string view_;                 // the running question's screen context
    std::string running_dir_;          // its conversation folder
    std::string last_result_;          // last "done" event (JSON)
    // identifications (newest last)
    struct Mark {
        std::string id, name, summary, conv, conf;
        double hz = 0, bw = 0, brg = -1;   // brg < 0 = none
        int64_t t = 0;
    };
    static std::string mark_json(const Mark& m);
    std::vector<Mark> marks_;
};
