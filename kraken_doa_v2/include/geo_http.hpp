#pragma once

// Online address search for the incident map (incidents.cpp): OpenStreetMap's
// Nominatim (nominatim.openstreetmap.org) over HTTPS, the certificate
// verified. Follows its usage policy: an identifying User-Agent, at most one
// request a second (callers wait their turn), answers cached for a day so a
// re-paged address costs nothing. When the server says to slow down (HTTP 429)
// or is unavailable (502/503/504), no requests are made for a while (1 min,
// doubling up to 30 min) - those failures count as "offline" (*offline =
// true: try again later). No dependencies on the rest of kraken_doa
// (tools/geo_test.cpp uses it).

#include <cstdint>
#include <map>
#include <mutex>
#include <string>

namespace geo {

// HTTPS GET. *status = the HTTP status (0 = no answer at all)
bool https_get(const std::string& host, const std::string& path_query, const std::string& user_agent,
               std::string* body, int* status, std::string* err);

class Nominatim {
public:
    // geo_address.hpp SearchFn: free text within the box s, w, n, e
    bool search(const std::string& query, double s, double w, double n, double e, std::string* json, std::string* err,
                bool* offline);
    uint64_t requests() const { return requests_; }   // made (not cached)
private:
    std::mutex mu_;
    int64_t last_ms_ = 0;
    int64_t blocked_until_ms_ = 0;   // after 429 / 5xx
    int64_t backoff_ms_ = 0;
    uint64_t requests_ = 0;
    std::map<std::string, std::pair<int64_t, std::string>> cache_;   // URL -> (time, answer)
};

}  // namespace geo
