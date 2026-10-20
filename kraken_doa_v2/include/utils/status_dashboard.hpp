#pragma once

// Live, in-place terminal status dashboard.
//
// Replaces the old 30-second scrolling status dump (main.cpp's status thread)
// with a full-screen TUI that redraws in place a few times per second. Every
// parameter is shown live and updated continuously; the program's own cout/cerr
// output is captured into a bounded ring and shown in a "log" pane at the bottom
// so the scattered one-off log lines never scroll the readout away.
//
// The dashboard only takes over when stdout is an interactive terminal
// (isatty; Windows: a real console with VT processing - not mintty's pipe).
// When output is a pipe/file/systemd journal, active() stays false
// and the caller keeps the plain periodic text dump, so a logfile never fills
// with ANSI escape garbage. The KRAKEN_DOA_NO_TUI environment variable forces
// the plain path even on a terminal.
namespace StatusDashboard {

// True once begin() has decided the live TUI should run (stdout is a TTY and
// the opt-out env var is unset). Meaningful only after begin().
bool active();

// Enter the alternate screen, hide the cursor and start capturing cout/cerr
// into the log ring. No-op (and leaves active() == false) when not on a
// terminal or when opted out.
void begin();

// Draw one frame from the current live program state. Cheap; intended to be
// called on a timer (~4 Hz). No-op when inactive.
void render();

// Restore the terminal (leave the alternate screen, show the cursor), stop
// capturing, and echo the tail of the captured log back to the normal screen so
// nothing from the session is lost. Safe to call even if begin() no-op'd.
void end();

}  // namespace StatusDashboard
