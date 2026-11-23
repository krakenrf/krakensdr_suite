#pragma once
// The web UI's certificate as the page shows it (docs/dev/https-certificate.md):
// system_status.cert, the CA download (/krakensdr-ca.crt) and CERT_SCOPE:.
// The certificate itself is made by scripts/make-cert.sh or in-process
// (tlscert::ensure, utils/tls_compat.hpp) before the HTTPS listener starts.
#include <string>
#include <string_view>

namespace certstatus {

// Once, after the certificate files are final and before the listener:
// reads server.crt, krakensdr-ca.crt and the retired CAs - what this run
// serves - adds the server certificate's DNS names to the host allow-list
// (host_add_allowed_names, utils/host_check.hpp) and starts a worker thread
// that re-checks the host name, the addresses and cert_options every 60 s
// (it may run avahi's tools: never on the uWS loop).
void init();

// The "cert" object of system_status (cached text, cheap to copy); "" before
// init(). {managed, scope, scope_next, host, host_ok, ca: {name, sha256,
// sha1, not_after} | null, server: {name, names, ips, not_after, days_left},
// ips_now, retired: [{name, sha256, date}], notices: [expires_soon,
// name_changed, addresses_changed, restart_pending], scope_error, scope_seq}
std::string json();

// GET /krakensdr-ca.crt: the CA certificate as read at startup (PEM, the
// file's bytes); with no CA (a certificate of the user's own) the server
// certificate. false = nothing to offer.
bool download(std::string* pem);

// CERT_SCOPE:narrow|wide - writes cert_options (applied at the next start).
// allowed: from a page's own socket (not the settings replay, the AI
// assistant, the web mapper, the scanners). Refused during a playback and
// for a certificate of the user's own. "" = written (*applied = the
// normalised command to echo), else the reason (also in the status:
// scope_error, scope_seq).
std::string set_scope(std::string_view value, bool allowed, std::string* applied);

}  // namespace certstatus
