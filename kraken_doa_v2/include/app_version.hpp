#pragma once

#include <cstddef>
#include <string>

// kraken_doa's side of the version indicator (docs/dev/version.md); the
// suite-wide part - git, ./VERSION, the compiled-in value, the text format -
// is utils/version_info.hpp (appver::), shared with heimdall.

// Gives appver its way of running git: kproc (utils/process.hpp). Call first
// in main(), before --version is handled.
void app_version_init();

// Bytes read from heimdall's control socket (any chunking): its 2 Hz status
// lines carry heimdall's own "version" object
void heimdall_version_ingest(const char* data, size_t n);

// heimdall's version (appver::to_json, its text rebuilt from the fields), ""
// when its status never had one or none came for 30 s
std::string heimdall_version_json();
