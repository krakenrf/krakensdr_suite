#pragma once

// LTE turbo code (3GPP TS 36.212 5.1.3.2: PCCC, two 8-state constituent
// encoders g0 = 13, g1 = 15 octal, QPP interleaver) and its rate matching
// (5.1.4.1: sub-block interleavers, circular buffer, redundancy versions) -
// the PDSCH's channel code (SIB1 / SI messages). Bits 0 / 1; soft bits
// > 0 = 0. Shared by the decoder and the synthetic generator.

#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace cells::fec {

// code block segmentation of a single block (36.212 5.1.2, B <= 6144): the
// code block size K >= B from the QPP table (0 if B is too large); F = K - B filler bits
int turbo_k(int B);

// QPP interleaver of size K: pi(i) = (f1 i + f2 i^2) mod K
std::vector<int> qpp(int K);

// c: K bits (the F filler bits first, as 0) -> d[3] of K + 4 bits
void turbo_encode(const std::vector<uint8_t>& c, std::vector<uint8_t> d[3]);

// Rate matching of one code block: where each of the E transmitted bits
// comes from (stream, index into d), for redundancy version rv; filler bits
// (F at the start of streams 0 and 1) are skipped like the dummies
std::vector<std::pair<int, int>> turbo_rate_match(int K, int F, int E, int rv);

// Max-log-MAP turbo decoding of soft d[3] (K + 4 each): up to `iters`
// iterations, stopping early when ok(bits) says so (the CRC). Returns the K
// bits of the last iteration.
std::vector<uint8_t> turbo_decode(const std::vector<float> s[3], int K, int iters, const std::function<bool(const std::vector<uint8_t>&)>& ok,
                                  int* used_iters = nullptr);

}  // namespace cells::fec
