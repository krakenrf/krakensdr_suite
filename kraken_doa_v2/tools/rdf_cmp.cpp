// The C++ mobile DF engine (src/rdf_engine.cpp) on records written by
// tools/replay_validate.mjs, for comparing its JavaScript port (the ⏯ log
// replay's worker in kraken_doa.html) with it:
//   g++ -std=c++20 -O2 -Iinclude tools/rdf_cmp.cpp src/rdf_engine.cpp -o rdf_cmp
//   node tools/replay_validate.mjs --cpp ./rdf_cmp
// rdf_cmp RECS OUTDIR: RECS = u32 n, then per record f64 t, lat, lon, f32 conf,
// i32 frames, f32 p[720]. Writes OUTDIR/est.txt (after each record: valid lat
// lon r50 r95 a b ang mass edge fine), OUTDIR/coarse.bin + fine.bin (the final
// rasters, 12 nats) and prints the final offset
#include "rdf_engine.hpp"

#include <cstdio>
#include <string>
#include <utility>

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: rdf_cmp RECS OUTDIR\n"); return 2; }
    FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::perror(argv[1]); return 1; }
    const std::string dir = argv[2];
    uint32_t n = 0;
    if (std::fread(&n, 4, 1, f) != 1) return 1;
    rdf::Solver s({}, 10, 256);
    FILE* o = std::fopen((dir + "/est.txt").c_str(), "w");
    if (!o) return 1;
    for (uint32_t i = 0; i < n; i++) {
        double t, lat, lon;
        float conf;
        int32_t frames;
        rdf::Record r;
        if (std::fread(&t, 8, 1, f) != 1 || std::fread(&lat, 8, 1, f) != 1 || std::fread(&lon, 8, 1, f) != 1 ||
            std::fread(&conf, 4, 1, f) != 1 || std::fread(&frames, 4, 1, f) != 1 ||
            std::fread(r.p.data(), 4, rdf::BINS, f) != static_cast<size_t>(rdf::BINS))
            return 1;
        r.t_ms = static_cast<int64_t>(t);
        r.lat = lat;
        r.lon = lon;
        r.conf = conf;
        r.frames = frames;
        s.add(r);
        const rdf::Estimate& e = s.estimate();
        std::fprintf(o, "%d %.9f %.9f %.3f %.3f %.3f %.3f %.3f %.6f %d %d\n", e.valid, e.lat, e.lon, e.r50_m, e.r95_m, e.ell_a_m,
                     e.ell_b_m, e.ell_ang_deg, e.mode_mass, e.at_edge, s.fine() ? 1 : 0);
    }
    std::fclose(o);
    std::fclose(f);
    for (const auto& [g, name] : {std::pair{s.coarse(), "/coarse.bin"}, std::pair{s.fine(), "/fine.bin"}}) {
        FILE* b = std::fopen((dir + name).c_str(), "wb");
        if (!b) return 1;
        if (g) {
            const auto ras = g->raster(12);
            std::fwrite(ras.data(), 1, ras.size(), b);
        }
        std::fclose(b);
    }
    std::printf("%.4f %.4f\n", s.estimate().offset_deg, s.estimate().offset_sd_deg);
    return 0;
}
