#include "trunk_follow.hpp"

#include <deque>
#include <map>
#include <mutex>

// Stage A: the trunk manager itself is not written yet - the events the
// decoders hand over are only kept (a small ring per VFO) for tests.

namespace trunk {

namespace {
std::mutex mu;
std::map<int, std::deque<std::pair<std::string, PluginEvent>>> recent;
}  // namespace

void on_plugin_event(int vfo, const std::string& plugin_id, PluginEvent&& ev) {
    std::lock_guard<std::mutex> lk(mu);
    auto& q = recent[vfo];
    q.emplace_back(plugin_id, std::move(ev));
    while (q.size() > RECENT_MAX) q.pop_front();
}

std::vector<std::pair<std::string, PluginEvent>> recent_events(int vfo) {
    std::lock_guard<std::mutex> lk(mu);
    auto it = recent.find(vfo);
    if (it == recent.end()) return {};
    return {it->second.begin(), it->second.end()};
}

void clear_recent() {
    std::lock_guard<std::mutex> lk(mu);
    recent.clear();
}

}  // namespace trunk
