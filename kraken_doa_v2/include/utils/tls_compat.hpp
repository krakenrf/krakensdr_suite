#pragma once
// TLS (OpenSSL) set-up that differs between Linux and Windows: where a TLS
// client finds the CA certificates it verifies servers against, and the
// certificate of the 8080 web UI (a CA per receiver - the in-process side of
// docs/dev/https-certificate.md). On Linux tls_load_system_roots does what
// the code did before it existed.
#include <openssl/ssl.h>

#include <cstdint>
#include <string>
#include <vector>

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

// ---- the web UI's certificate (docs/dev/https-certificate.md) -----------------------
// scripts/make-cert.sh does this with the openssl CLI on Linux / macOS /
// Docker (run.sh, install.sh, the container's entrypoint); kraken_doa does it
// in-process on Windows at every start and elsewhere when server.crt /
// server.key are missing. Both follow the contract in that file: the same
// files, names, scope, triggers and managed check.
namespace tlscert {

inline constexpr const char* CA_KEY = "krakensdr-ca.key";     // RSA 3072, 0600
inline constexpr const char* CA_CRT = "krakensdr-ca.crt";     // what users install, 0644
inline constexpr const char* SERVER_KEY = "server.key";       // RSA 2048, kept for every re-issue, 0600
inline constexpr const char* SERVER_CRT = "server.crt";       // server certificate + the CA certificate, 0644
inline constexpr const char* OPTIONS = "cert_options";        // "scope=narrow" | "scope=wide"; missing = narrow
inline constexpr int SERVER_DAYS = 824;                       // one under the 825 Apple accepts (fine rule 33)
inline constexpr int CA_DAYS = 3650;
inline constexpr int SERVER_RENEW_DAYS = 60;                  // re-issued with fewer left
inline constexpr int CA_RENEW_DAYS = 365;                     // a new CA with fewer left
inline constexpr const char* ORG = "KrakenSDR receiver";

// <host>: KRAKEN_CERT_HOST trimmed (not cut at a dot), else (up to the first
// dot) the name avahi announces (Linux: busctl / gdbus / avahi-resolve, each
// only if installed, 2 s timeouts), macOS `scutil --get LocalHostName`,
// gethostname(). Lowercased; usable = one DNS label: 1-63 of a-z 0-9 -, not
// starting or ending with - (the "Fine rules" of
// docs/dev/https-certificate.md, which these functions and make-cert.sh
// follow to the letter: the two must agree, or each would re-issue what the
// other made). May start helper programs: never call it on the uWS loop.
struct Host {
    std::string raw;    // as found (for the page: why it can't be used)
    std::string name;   // lowercased; "" when not usable
    bool usable = false;
    // the name avahi announces when it is another (an mDNS conflict:
    // krakensdr-2), else "": an EXTRA name, never one of the own names (fine rule 24)
    std::string avahi;
};
Host host_name();

// The DNS names the server certificate carries (and a narrow CA permits):
// <host>, <host>.local, <host>.<d> for each search / domain entry of
// /etc/resolv.conf (Windows: each adapter's DNS suffix) except "local",
// localhost, then KRAKEN_CERT_NAMES (split on commas and white space; an
// address or a word that is no DNS name is left out, with a line in *notes)
// - the last two also with an unusable host. No duplicates.
std::vector<std::string> wanted_names(const Host& h, std::vector<std::string>* notes = nullptr);
// The host's OWN names - <host>, <host>.local (none for an unusable host): one
// of them outside the CA is a rename and makes a new CA; any other wanted
// name outside it is left out of the server certificate (fine rule 18)
std::vector<std::string> own_names(const Host& h);
// KRAKEN_CERT_NAMES' names alone (as wanted_names takes them)
std::vector<std::string> env_names(std::vector<std::string>* notes = nullptr);
// Is `name` inside these permitted DNS subtrees (a CA's; empty = every name)?
bool name_permitted(const std::vector<std::string>& permitted_dns, const std::string& name);

// The IPv4 addresses of the interfaces that are up (not the loopback
// interface, 127/8, 169.254/16; as make-cert.sh's `ip -4 -o addr show up`)
// inside the wide scope's private ranges (10/8, 172.16/12, 192.168/16,
// 100.64/10): a name outside the CA's constraints fails the whole
// certificate, so a public address is never put in. getifaddrs /
// GetAdaptersAddresses.
std::vector<std::string> private_ipv4();

// The server certificate's IPs: 127.0.0.1, ::1 (+ private_ipv4() when wide)
std::vector<std::string> wanted_ips(bool wide);

// cert_options in `dir` ("" = the working directory): true = wide (the last
// "scope = VALUE" line, any case, says "wide")
bool read_scope_wide(const std::string& dir);
// ... a new CA asked for (the last "new_ca = VALUE" line says 1; fine rule 29)
bool read_new_ca(const std::string& dir);
// Writes "scope=narrow|wide" (+ "new_ca=1") atomically, mode 0644
bool write_options(const std::string& dir, bool wide, bool new_ca, std::string* err);
// Writes it atomically (temp file + fs_replace), mode 0644
bool write_scope(const std::string& dir, bool wide, std::string* err);

// What the generator did - make-cert.sh's last line
enum class State { Kept, Issued, CaCreated, NotManaged };
const char* state_name(State s);   // "kept" | "issued" | "ca-created" | "not-managed"

// The generator. true = a usable pair is there (*state says what happened);
// false + *err: nothing was changed. force: re-issue the server certificate;
// new_ca: a new CA (as make-cert.sh --force / --new-ca). Takes make-cert.sh's
// lock (<dir>/.make-cert.lock; waits up to 10 s for another maker) and
// writes notes (a name left out, an unusable key moved aside, a migrated
// server.crt kept) to stderr.
bool ensure(const std::string& dir, bool force, bool new_ca, State* state, std::string* err);

// ---- reading what is there (the page's certificate section) ----
struct CertInfo {
    bool ok = false;
    std::string cn, org;                 // subject
    std::vector<std::string> dns, ips;   // subjectAltName
    int64_t not_after = 0;               // Unix seconds
    std::string sha256, sha1;            // fingerprints of the DER, "AB:CD:..." (as openssl x509 -fingerprint)
    std::string pem;                     // the certificate alone, re-encoded from the parsed one (never a file's bytes)
    std::string scope;                   // a CA: "wide" (permits 10.0.0.0/8), "narrow", "none" (no name constraints)
    std::vector<std::string> permitted_dns;   // a CA: its permitted DNS subtrees
    bool has_ip_subtree = false;              // a CA: a permitted IP subtree
    bool server_auth_only = false;            // extendedKeyUsage exactly serverAuth
    int64_t not_before = 0;                   // Unix seconds
};
struct Inspect {
    bool present = false;          // server.crt there and readable
    bool managed = false;          // (docs/dev/https-certificate.md "Managed or not")
    CertInfo server;               // server.crt's first certificate
    bool has_ca = false;           // krakensdr-ca.crt there and server.crt verifies against it
    CertInfo ca;
    struct Retired {
        std::string date;          // YYYY-MM-DD from the file name
        CertInfo cert;
    };
    std::vector<Retired> retired;  // krakensdr-ca.*.retired.crt, newest first
};
Inspect inspect(const std::string& dir);

}  // namespace tlscert
