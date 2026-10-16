#include "station_info.hpp"

#include <chrono>
#include "globals.hpp"
#include "networking/gpsd_client.hpp"

void StationInfo::setId(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    id_ = id;
}

std::string StationInfo::getId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return id_;
}

void StationInfo::setSource(LocationSource s) {
    std::lock_guard<std::mutex> lock(mutex_);
    source_ = s;
}

LocationSource StationInfo::getSource() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return source_;
}

void StationInfo::setStatic(double lat, double lon, double heading) {
    std::lock_guard<std::mutex> lock(mutex_);
    static_lat_ = lat;
    static_lon_ = lon;
    static_heading_ = heading;
}

void StationInfo::getStatic(double& lat, double& lon, double& heading) const {
    std::lock_guard<std::mutex> lock(mutex_);
    lat = static_lat_;
    lon = static_lon_;
    heading = static_heading_;
}

void StationInfo::setPlaybackOverride(double lat, double lon, double heading, bool moving, double speed) {
    std::lock_guard<std::mutex> lock(mutex_);
    ov_on_ = true;
    ov_lat_ = lat;
    ov_lon_ = lon;
    ov_heading_ = heading;
    ov_moving_ = moving;
    ov_speed_ = speed;
}

void StationInfo::clearPlaybackOverride() {
    std::lock_guard<std::mutex> lock(mutex_);
    ov_on_ = false;
}

StationLocation StationInfo::resolve() const {
    StationLocation out;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        out.id = id_;
        if (ov_on_) {
            // a recording plays: where it was made (a static station, or a
            // fix of its GPS track at the playback's position)
            out.source = ov_moving_ ? LocationSource::GPS : LocationSource::STATIC;
            out.lat = ov_lat_;
            out.lon = ov_lon_;
            out.heading = ov_heading_ >= 0 ? ov_heading_ : 0.0;
            out.heading_valid = ov_heading_ >= 0;
            out.speed = ov_speed_;
            if (ov_moving_)
                out.timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch()).count();
            return out;
        }
        out.source = source_;
        if (source_ == LocationSource::STATIC) {
            out.lat = static_lat_;
            out.lon = static_lon_;
            out.heading = static_heading_;
            out.heading_valid = true;   // user-specified
            out.from_compass = false;
        }
        // MOBILE: leave everything at 0 (phone-app path not implemented).
    }

    if (out.source == LocationSource::GPS) {
        GpsFix f = gps_client.get();
        if (f.has_fix()) {
            out.lat = f.lat;
            out.lon = f.lon;
            out.speed = f.speed;
            out.timestamp_ms = f.timestamp_ms;
        }
        // A compass heading can be valid even without a position fix; otherwise
        // fall back to the movement-derived track. Mirrors GpsFix::heading().
        if (f.has_compass) {
            out.heading = f.compass_heading;
            out.heading_valid = true;
            out.from_compass = true;
        } else if (f.track_valid) {
            out.heading = f.track;
            out.heading_valid = true;
            out.from_compass = false;
        }
    }

    return out;
}
