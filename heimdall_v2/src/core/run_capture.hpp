#pragma once
// Run a short-lived helper program and capture its stdout - heimdall's only
// child process (git, for the version indicator: core/version_info.hpp).
// No shell: argv[0] is looked up on PATH (Windows: <name>.exe, never the cwd
// or heimdall's folder), stdin and stderr go to the null device, nothing
// else is inherited (Linux: close-on-exec + closefrom where glibc has it;
// Windows: an explicit inherited-handle list - sockets are inheritable
// there by default). Killed at timeout_ms.
#include <string>
#include <vector>

// true = it exited 0 within timeout_ms; *out = its stdout (at most 1 MiB)
bool run_capture(const std::vector<std::string>& argv, int timeout_ms, std::string* out);
