#pragma once

// The AI assistant's MCP server (Model Context Protocol, the streamable HTTP
// transport answering with plain JSON): what the agent sees of the receiver
// and how it controls it. http://127.0.0.1:<AI_MCP_PORT>/mcp, loopback only,
// a bearer token made at startup (the bridge gets it in its environment from
// AiManager and writes the agent's --mcp-config with it). Tools: reading
// (screen, spectrum, decoders, bearings, map points, logs, transcripts, saved
// frequencies, plugins, IQ capture + analysis, waiting for decoder events,
// the identifications the assistant remembers) and controlling - tuning, gain,
// VFOs, decoders, listening, DoA, saved frequencies - through
// ControlHandler::apply_command, i.e. exactly what a browser could do
// (persisted, synced to every page). Control refuses while the page's "Read
// only" switch is on (AiManager::readonly). Nothing here writes files the
// receiver runs on or reaches folders / keys / settings resets: those
// commands have no tool.

#include <string>

namespace geo { struct Json; }

namespace ai_mcp {

// Creates the HTTP app on the uWS loop thread (call there, before the main
// app runs) and listens on 127.0.0.1
void attach();
std::string url();     // "" while not listening
std::string token();

// get_screen's answer: the receiver, station and every VFO + the asking
// page's view (a JSON object) - also sent with each question. uWS loop thread
std::string screen_json(const std::string& view_json);

// a parsed JSON value written out again
std::string to_json(const geo::Json& j);

// a page's answer to a map / viewshed command (AI_PAGE_REPLY:{json}, uWS loop)
void page_reply(const std::string& json);

}  // namespace ai_mcp
