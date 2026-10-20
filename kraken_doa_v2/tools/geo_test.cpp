// Offline-ish test of the incident map's address lookup (not part of kraken_doa):
//   geo_test LAT LON KM [TEXT...]   find + position (online, Nominatim) the address in each TEXT, or stdin lines
//   geo_test --dry ...                only print the searches it would make (no network)
// Build (from kraken_doa_v2):
//   g++ -std=c++20 -O2 -Iinclude tools/geo_test.cpp src/geo_address.cpp src/geo_http.cpp src/utils/tls_compat.cpp
//       src/utils/fs_compat.cpp -lssl -lcrypto
// Windows (MSYS2 UCRT64 shell): the Windows flags and win/win_init.cpp (it
// starts Winsock), static:
//   g++ -std=c++20 -O2 -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 -DWIN32_LEAN_AND_MEAN -DNOMINMAX -DNOGDI
//       -D_USE_MATH_DEFINES -D_FILE_OFFSET_BITS=64 -include win/posix_compat.h -Iinclude tools/geo_test.cpp
//       src/geo_address.cpp src/geo_http.cpp src/utils/tls_compat.cpp src/utils/fs_compat.cpp win/win_init.cpp
//       -static -lssl -lcrypto -lws2_32 -lcrypt32 -lwinmm
#include "geo_address.hpp"
#include "geo_http.hpp"

#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>

int main(int argc, char** argv) {
    bool dry = argc > 1 && std::string(argv[1]) == "--dry";
    if (dry) { argv++; argc--; }
    if (argc < 4) {
        fprintf(stderr, "usage: geo_test LAT LON KM [TEXT...]\n");
        return 2;
    }
    const double lat = atof(argv[1]), lon = atof(argv[2]), km = atof(argv[3]);
    geo::Nominatim nom;
    geo::SearchFn fn = [&](const std::string& q, double s, double w, double n, double e, std::string* js, std::string* err,
                           bool* off) {
        if (dry) { printf("    search: %s\n", q.c_str()); *js = "[]"; return true; }
        return nom.search(q, s, w, n, e, js, err, off);
    };
    auto one = [&](const std::string& text) {
        geo::Result r;
        std::string err;
        bool offline = false;
        if (geo::geocode(text, lat, lon, km, fn, &r, &err, &offline))
            printf("%-58.58s -> %s%s [%s] @ %.5f, %.5f %s/%s alt %d, %.1f km, %d lookups%s\n", text.c_str(),
                   r.address.c_str(), r.cross.empty() ? "" : (" / " + r.cross).c_str(), r.area.c_str(), r.lat, r.lon,
                   r.precision.c_str(), r.confidence.c_str(), r.alternatives, geo::distance_km(lat, lon, r.lat, r.lon),
                   r.lookups, r.place.empty() ? "" : (" (place: " + r.place + ")").c_str());
        else
            printf("%-58.58s -> %s\n", text.c_str(), err.empty() ? "(no address)" : ("ERROR " + err + (offline ? " [offline]" : "")).c_str());
    };
    if (argc > 4)
        for (int i = 4; i < argc; i++) one(argv[i]);
    else
        for (std::string l; std::getline(std::cin, l);) one(l);
    printf("%llu requests to Nominatim\n", static_cast<unsigned long long>(nom.requests()));
    return 0;
}
