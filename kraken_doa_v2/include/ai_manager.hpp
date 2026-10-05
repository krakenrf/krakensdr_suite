#pragma once

// AI Signal Lab (sidebar "🤖 AI Signal Lab"): runs ai/kraken_ai.py - which
// drives an LLM coding agent (Claude Code by default) - to identify the
// signal on a VFO and to write decoder plugins (plugins/<id>/) for it, plus
// plugin builds and imports. One job at a time, in its own process group so
// Stop kills the agent and everything it started.
//
// The bridge prints one JSON event per line ({"ev":...}); each is relayed to
// browsers as {"ai_event":{...}} and kept (last 300) so a page opened later
// sees the job. The lab is OFF unless enabled on the Pi itself
// (`python3 ai/kraken_ai.py setup` writes ai/ai_config.json) - the web UI
// can't enable it, since it lets UI users run an agent and native code here.

#include <atomic>
#include <deque>
#include <mutex>
#include <string>
#include <sys/types.h>
#include <thread>
#include <vector>

class AiManager {
public:
    static AiManager& instance();
    // Loads the newest session; call once at startup
    void init();

    // All called on the uWS loop. They return "" or an error message.
    std::string investigate(int vfo_id, double freq_hz, double seconds, const std::string& instructions);
    std::string create(const std::string& plugin_id, const std::string& instructions);
    std::string ask(const std::string& question);
    std::string test();
    void cancel();

    bool enabled() const;          // ai/ai_config.json "enabled": true
    bool busy() const { return busy_.load(); }
    // {"ai_state":{...}} - config, job, current session, recent events
    std::string state_json(bool with_log) const;
    static std::string plugins_message();   // {"plugins":[...]}

    // --- saved investigations (ai/sessions/<id>/), one per analysed signal ---
    // The file work runs on a worker thread; results are broadcast:
    //   sessions_async()       -> {"ai_sessions":[{id,freq_hz,signal_name,...}]}
    //   session_async(id)      -> {"ai_session":{...,"chat":[..],"activity":[..]}}
    //   delete_session(id)     -> removes it (and its Claude transcript), then
    //                             the new list; "" or an error
    //   select_session(id)     -> makes it the current one (Create / Ask use it)
    static void sessions_async();
    static void session_async(const std::string& id);
    std::string delete_session(const std::string& id);
    std::string select_session(const std::string& id);
    static bool valid_session_id(const std::string& id);

private:
    AiManager() = default;
    ~AiManager();
    std::string start_job(const std::string& kind, const std::vector<std::string>& argv, const std::string& label);
    void reader(int fd, pid_t pid, std::string kind);
    void on_line(const std::string& kind, const std::string& line);
    void finished(const std::string& kind, int status);
    void load_session(const std::string& id);   // current session <- session.json

    mutable std::mutex mu_;
    std::atomic<bool> busy_{false};
    std::thread thread_;
    pid_t pid_ = -1;
    std::string job_kind_, job_label_;
    int64_t job_started_ms_ = 0;
    std::deque<std::string> log_;      // relayed event objects (JSON)
    uint64_t seq_ = 0;
    // current session
    std::string session_, analysis_, signal_name_, suggested_id_, plugin_id_, session_state_;
    double session_freq_ = 0;
    int session_vfo_ = -1;
    std::string last_result_;          // last "done" event (JSON)
};
