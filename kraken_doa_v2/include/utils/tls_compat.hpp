#pragma once
// TLS (OpenSSL) set-up that differs between Linux and Windows: where a TLS
// client finds the CA certificates it verifies servers against, and the
// self-signed certificate of the 8080 web UI. On Linux each function does
// what the code did before it existed.
#include <openssl/ssl.h>

#include <string>

// Trust anchors for a TLS client context that verifies its server (the
// KrakenPro cloud web mapper, the Nominatim address search). Returns 1 on
// success like SSL_CTX_set_default_verify_paths, 0 when there are none.
//  Linux:   SSL_CTX_set_default_verify_paths(ctx) - the distro's CA bundle.
//  Windows: MSYS2's OpenSSL looks for its bundle relative to its own install
//           (<prefix>/etc/ssl), which a copied exe doesn't have: the default
//           paths "succeed" with nothing loaded and every server is
//           rejected (unable to get local issuer certificate). So the
//           certificates of the Windows "ROOT" and "CA" system stores (those
//           valid for server authentication; a self-signed one in CA is no
//           anchor), a cacert.pem next to the exe (if there is one) and
//           OpenSSL's default paths (SSL_CERT_FILE ...) go into one store,
//           built on the first call and shared by every context
//           (SSL_CTX_set1_cert_store). Windows downloads most of the roots it
//           trusts only when its own chain building first needs one, so a
//           chain that doesn't reach a root of that store goes to CryptoAPI
//           (SSL_CTX_set_cert_verify_callback: chain + SSL policy with the
//           host name); one Windows accepts is verified by OpenSSL again with
//           Windows' root as the only anchor. Link with -lcrypt32.
int tls_load_system_roots(SSL_CTX* ctx);

// The web UI's certificate: when cert_path or key_path is missing (or
// empty), a new pair is made in-process and written to both - RSA-2048, a
// self-signed SHA-256 certificate, CN=krakensdr, SAN krakensdr / localhost /
// 127.0.0.1 / ::1, valid 10 years (what install.sh makes with the openssl
// CLI on Linux; Windows has no install step and no openssl.exe). The key is
// written owner-only (a protected DACL: this user, SYSTEM, Administrators).
// An existing pair is never touched. true = the pair is there (*created: made now);
// false + *err when it couldn't be made. Only the Windows build calls it.
bool tls_ensure_self_signed_cert(const char* cert_path, const char* key_path, bool* created, std::string* err);
