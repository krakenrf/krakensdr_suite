#pragma once

#include <cstdint>
#include <mutex>
#include <string>

// StationInfo
// -----------
// Holds the station identity (callsign) and the chosen location source, and
// resolves them into the single "effective" location that gets reported on
// DOA_value.html and the WebSocket API. Mirrors the legacy KrakenSDR "Station
// Information" settings.
//
// Location sources:
//   MOBILE  - location comes from a phone app (Android/iOS). Not implemented
//             yet, so this resolves to 0's.
//   STATIC  - fixed lat/lon/heading typed in by the user.
//   GPS     - live fix from the gpsd-connected receiver (see GpsdClient).

enum class LocationSource { MOBILE = 0, STATIC = 1, GPS = 2 };

// Effective, ready-to-report station identity + location.
struct StationLocation {
    std::string   id;               // callsign / station name
    LocationSource source = LocationSource::GPS;
    double        lat = 0.0;        // degrees (0 when unavailable)
    double        lon = 0.0;        // degrees (0 when unavailable)
    double        heading = 0.0;    // degrees (0 when unavailable)
    bool          heading_valid = false;
    bool          from_compass = false;  // true => heading is a magnetic/compass heading
    double        speed = 0.0;      // m/s
    int64_t       timestamp_ms = 0; // epoch ms (0 if unknown)

    const char* source_str() const {
        switch (source) {
            case LocationSource::STATIC: return "static";
            case LocationSource::GPS:    return "gps";
            default:                     return "mobile";
        }
    }

    // Richer heading-origin label for the API/UI ("GPS"/"Compass" are the only
    // values the DOA_value.html sensor field uses; see websocket_server.cpp).
    const char* heading_source_str() const {
        switch (source) {
            case LocationSource::STATIC: return heading_valid ? "Static" : "None";
            case LocationSource::MOBILE: return "Mobile";
            case LocationSource::GPS:
                if (from_compass) return "Compass";
                return heading_valid ? "GPS" : "None";
        }
        return "None";
    }
};

class StationInfo {
public:
    void setId(const std::string& id);
    std::string getId() const;

    void setSource(LocationSource s);
    LocationSource getSource() const;

    void setStatic(double lat, double lon, double heading);
    void getStatic(double& lat, double& lon, double& heading) const;

    // 📼 Playback of a recording: where the station was while it was recorded
    // (static, or moved along the recording's GPS track) - resolve() returns
    // it instead of the live source until cleared (the map, mobile DF, the
    // north reference of the bearings follow the recording). moving = from a
    // GPS track: reported as a GPS fix (speed, time of now) so mobile DF runs
    void setPlaybackOverride(double lat, double lon, double heading, bool moving = false, double speed = 0);   // heading < 0 = none
    void clearPlaybackOverride();

    // Resolve identity + the effective location for the current source.
    // For GPS this reads the live gpsd snapshot; for STATIC the manual values;
    // for MOBILE, zeros (until the phone-app path exists).
    StationLocation resolve() const;

private:
    mutable std::mutex mutex_;
    std::string    id_ = "KrakenSDR";
    LocationSource source_ = LocationSource::GPS;
    double         static_lat_ = 0.0;
    double         static_lon_ = 0.0;
    double         static_heading_ = 0.0;
    bool           ov_on_ = false;
    double         ov_lat_ = 0.0, ov_lon_ = 0.0, ov_heading_ = -1.0, ov_speed_ = 0.0;
    bool           ov_moving_ = false;
};
