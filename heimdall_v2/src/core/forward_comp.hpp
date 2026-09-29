#pragma once

#include "types.hpp"
#include <string>
#include <vector>
#include <complex>
#include <utility>

// Forward (feed-forward) phase/amplitude compensation from VNA S2P files.
//
// See the ForwardCompensation struct in types.hpp for the data model and the
// math. These helpers parse Touchstone .s2p files, (re)load the per-channel
// selection, list the calibration folder, and (re)compute the correction
// vector for a given center frequency. They are all COLD PATH (web / control /
// startup threads); the sample hot path only ever reads forward_comp.vector.
namespace fwdcomp {

// Folder (relative to the working directory, like heimdall_settings.conf and
// index.html) scanned for user-supplied .s2p calibration files.
constexpr const char* CAL_DIR = "s2p_calibration";

// Parse a 2-port (or 1-port) Touchstone file into a frequency-sorted table of
// (frequency_Hz, S21). The port count comes from the extension (.s2p / .s1p,
// as Touchstone v1 defines it). For a 1-port file S11 is used instead, with a logged
// warning - reflection is not the through response, but we accept it rather
// than fail. Handles the RI / MA / DB data formats and the HZ/KHZ/MHZ/GHZ
// frequency units from the option line. Returns true and fills `out` on
// success; false with `err` set otherwise. Thread-safe (no shared state).
bool parse_touchstone(const std::string& path,
                      std::vector<std::pair<double, std::complex<double>>>& out,
                      std::string& err);

// Set (or clear, when filename is empty) the S2P file for channel `ch`. The
// file is resolved INSIDE CAL_DIR (a bare filename, no path separators). Parses
// it into forward_comp.tables[ch] and updates forward_comp.files/loaded. Does
// NOT recompute the vector - call recompute() afterwards. Returns false on a
// bad channel / filename / parse error (err set); the selected filename is
// still recorded so the UI can show the failed choice.
bool set_channel_file(int ch, const std::string& filename, std::string& err);

// Recompute forward_comp.vector for center frequency fc_hz from the loaded
// tables: vector[k] = S21_ref(fc)/S21_k(fc) (amplitude+phase), forced to unit
// magnitude (phase-only) when correct_amplitude is false. A channel with no
// loaded file maps to identity (left untouched). Sets forward_comp.ready.
// Cheap; safe to call on every retune.
void recompute(double fc_hz);

// List the .s2p / .s1p files in CAL_DIR (bare filenames, sorted, case-
// insensitive extension match). Creates the folder if it does not exist.
// Never throws.
std::vector<std::string> list_files();

// Save an uploaded calibration file (browser upload) into CAL_DIR. `filename`
// must be a bare name with a .s2p/.s1p extension (no path separators / "..");
// `content` is the raw file body, which is validated by parsing it as a
// Touchstone file BEFORE anything is written (a file that does not parse is
// rejected and nothing is saved). Creates CAL_DIR if needed. Does NOT assign
// the file to any channel or recompute. Returns false with `err` set on a bad
// name, bad extension, parse failure, or write error.
bool save_uploaded_file(const std::string& filename,
                        const std::string& content, std::string& err);

// Delete a calibration file from CAL_DIR and clear it from any channel that
// currently references it (so that channel reverts to identity on the next
// recompute()). `filename` is validated like set_channel_file. Returns false
// with `err` set on a bad name or if the file does not exist / cannot be
// removed. Call recompute() afterwards to rebuild the correction vector.
bool delete_file(const std::string& filename, std::string& err);

}  // namespace fwdcomp
