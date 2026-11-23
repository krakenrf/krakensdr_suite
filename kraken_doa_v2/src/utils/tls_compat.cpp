// See tls_compat.hpp.
#ifdef _WIN32
// Before any OpenSSL header: wincrypt.h defines X509_NAME, OCSP_REQUEST ...
// as macros, which OpenSSL's headers #undef - only for a wincrypt.h included
// earlier. winsock2.h before windows.h (GetAdaptersAddresses, inet_ntop).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <wincrypt.h>
#include <aclapi.h>
#include <iphlpapi.h>
#include <sddl.h>
#endif

#include "utils/tls_compat.hpp"
#include "utils/fs_compat.hpp"
#include "utils/process.hpp"

#include <openssl/bio.h>
#include <openssl/bn.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <sys/stat.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>

#include <cwchar>
#include <mutex>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <ifaddrs.h>
#ifndef __APPLE__
#include <sys/timex.h>
#endif
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>
#endif

// --- CA certificates for TLS clients -------------------------------------------------

#ifdef _WIN32
namespace {

X509_STORE* g_roots = nullptr;   // built once, shared by every client context
int g_root_count = 0;
std::once_flag g_roots_once;

// Does Windows trust the certificate for server authentication? True when
// it carries no usage restriction at all, or one that lists serverAuth; a
// root an administrator limited to other purposes - or disabled ("all
// purposes" off: an empty list) - stays out, as for Windows' own TLS.
bool server_auth_ok(PCCERT_CONTEXT c) {
    DWORD size = 0;
    SetLastError(0);
    if (!CertGetEnhancedKeyUsage(c, 0, nullptr, &size)) return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
    std::vector<unsigned char> buf(size);
    auto* eku = reinterpret_cast<PCERT_ENHKEY_USAGE>(buf.data());
    SetLastError(0);
    if (!CertGetEnhancedKeyUsage(c, 0, eku, &size)) return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
    // an empty list: valid for every purpose (CRYPT_E_NOT_FOUND) or none
    if (eku->cUsageIdentifier == 0) return GetLastError() == static_cast<DWORD>(CRYPT_E_NOT_FOUND);
    for (DWORD i = 0; i < eku->cUsageIdentifier; i++)
        if (std::strcmp(eku->rgpszUsageIdentifier[i], szOID_PKIX_KP_SERVER_AUTH) == 0) return true;
    return false;
}

// UTF-8 path of a file next to the running exe
std::string beside_exe(const wchar_t* name) {
    std::wstring w(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, w.data(), static_cast<DWORD>(w.size()));
    if (n == 0 || n >= w.size()) return {};
    w.resize(n);
    const size_t sl = w.find_last_of(L"\\/");
    w = (sl == std::wstring::npos ? std::wstring() : w.substr(0, sl + 1)) + name;
    if (GetFileAttributesW(w.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string s(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), len, nullptr, nullptr);
    return s;
}

void build_roots() {
    X509_STORE* st = X509_STORE_new();
    if (!st) return;
    // OpenSSL's own defaults first: SSL_CERT_FILE / SSL_CERT_DIR, or the
    // bundle of an MSYS2 install the exe runs from
    X509_STORE_set_default_paths(st);
    // a CA bundle shipped with the exe (curl's / Mozilla's cacert.pem):
    // roots this Windows hasn't downloaded into its store yet
    const std::string bundle = beside_exe(L"cacert.pem");
    if (!bundle.empty()) X509_STORE_load_file(st, bundle.c_str());
    // the Windows stores: ROOT = the trusted roots (the machine's are part
    // of the user's view), CA = intermediates, which only help to build a
    // chain - the chain must still end at a root
    for (const wchar_t* name : {L"ROOT", L"CA"}) {
        HCERTSTORE h = CertOpenSystemStoreW(0, name);
        if (!h) continue;
        const bool intermediates = std::wcscmp(name, L"CA") == 0;
        for (PCCERT_CONTEXT c = nullptr; (c = CertEnumCertificatesInStore(h, c)) != nullptr;) {
            if (!server_auth_ok(c)) continue;
            const unsigned char* p = c->pbCertEncoded;
            if (X509* x = d2i_X509(nullptr, &p, static_cast<long>(c->cbCertEncoded))) {
                // a self-signed one in CA is no root for Windows, but in an
                // X509_STORE it would be a trust anchor
                if (!(intermediates && (X509_get_extension_flags(x) & EXFLAG_SS)))
                    X509_STORE_add_cert(st, x);   // a duplicate (in both stores, in the bundle) is refused - fine
                X509_free(x);
            }
        }
        CertCloseStore(h, 0);
    }
    ERR_clear_error();   // missing default files, duplicates
    g_root_count = sk_X509_OBJECT_num(X509_STORE_get0_objects(st));
    g_roots = st;
}

// Windows' verdict on the chain a server sent: CryptoAPI builds it (the sent
// certificates in a memory store) for server authentication and checks it
// with the SSL policy - host = the name it must carry, nullptr = no name
// check here. true = trusted; *out = the chain, leaf first, root last.
bool windows_chain(X509* leaf, STACK_OF(X509)* sent, const char* host, std::vector<X509*>* out) {
    HCERTSTORE mem = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (!mem) return false;
    auto add = [mem](X509* x, PCCERT_CONTEXT* added) {
        unsigned char* der = nullptr;
        const int n = i2d_X509(x, &der);
        if (n <= 0) return false;
        const BOOL r = CertAddEncodedCertificateToStore(mem, X509_ASN_ENCODING, der, static_cast<DWORD>(n),
                                                        CERT_STORE_ADD_USE_EXISTING, added);
        OPENSSL_free(der);
        return r != FALSE;
    };
    PCCERT_CONTEXT lc = nullptr;
    PCCERT_CHAIN_CONTEXT chain = nullptr;
    bool ok = add(leaf, &lc);
    for (int i = 0; ok && i < sk_X509_num(sent); i++) add(sk_X509_value(sent, i), nullptr);
    if (ok) {
        LPSTR usage[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
        CERT_CHAIN_PARA cp{};
        cp.cbSize = sizeof cp;
        cp.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
        cp.RequestedUsage.Usage.cUsageIdentifier = 1;
        cp.RequestedUsage.Usage.rgpszUsageIdentifier = usage;
        ok = CertGetCertificateChain(nullptr, lc, nullptr, mem, &cp, 0, nullptr, &chain) && chain->cChain > 0 &&
             chain->rgpChain[0]->cElement > 0;
    }
    if (ok) {
        std::wstring wh;
        if (host) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, host, -1, nullptr, 0);
            wh.assign(n > 0 ? static_cast<size_t>(n) : 1, L'\0');
            if (n > 0) MultiByteToWideChar(CP_UTF8, 0, host, -1, wh.data(), n);
        }
        SSL_EXTRA_CERT_CHAIN_POLICY_PARA sp{};
        sp.cbSize = sizeof sp;
        sp.dwAuthType = AUTHTYPE_SERVER;
        sp.pwszServerName = host ? wh.data() : nullptr;
        CERT_CHAIN_POLICY_PARA pp{};
        pp.cbSize = sizeof pp;
        pp.pvExtraPolicyPara = &sp;
        CERT_CHAIN_POLICY_STATUS ps{};
        ps.cbSize = sizeof ps;
        ok = CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &pp, &ps) && ps.dwError == 0;
    }
    if (ok) {
        const CERT_SIMPLE_CHAIN* s = chain->rgpChain[0];
        for (DWORD i = 0; ok && i < s->cElement; i++) {
            const CERT_CONTEXT* c = s->rgpElement[i]->pCertContext;
            const unsigned char* p = c->pbCertEncoded;
            X509* x = d2i_X509(nullptr, &p, static_cast<long>(c->cbCertEncoded));
            if (x) out->push_back(x);
            else ok = false;
        }
    }
    if (chain) CertFreeCertificateChain(chain);
    if (lc) CertFreeCertificateContext(lc);
    CertCloseStore(mem, 0);
    return ok;
}

// The clients' chain verification (SSL_CTX_set_cert_verify_callback).
// OpenSSL's own first; when that fails only because the chain doesn't reach
// a root of g_roots, Windows decides. Most of the roots Windows trusts arrive
// on demand (Automatic Root Certificates Update, when CryptoAPI first needs
// one), which OpenSSL never triggers - so the store above lacks them - and
// CryptoAPI also fetches an intermediate a server left out. A chain Windows
// accepts is verified by OpenSSL once more, with the root Windows ended at
// as the only anchor: host name / IP, validity, purpose and key sizes are
// checked exactly as for every other chain.
int verify_chain(X509_STORE_CTX* sc, void*) {
    const int ok = X509_verify_cert(sc);
    const int e = X509_STORE_CTX_get_error(sc);
    if (ok == 1 || (e != X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY && e != X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT &&
                    e != X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN))
        return ok;
    X509* leaf = X509_STORE_CTX_get0_cert(sc);
    STACK_OF(X509)* sent = X509_STORE_CTX_get0_untrusted(sc);
    X509_VERIFY_PARAM* param = X509_STORE_CTX_get0_param(sc);
    // Windows checks the host name too when there is exactly one (an IP
    // literal has none: only OpenSSL's check below)
    const char* host = X509_VERIFY_PARAM_get0_host(param, 1) ? nullptr : X509_VERIFY_PARAM_get0_host(param, 0);
    std::vector<X509*> win;
    int r = ok;
    if (leaf && windows_chain(leaf, sent, host, &win)) {
        X509_STORE* anchor = X509_STORE_new();
        STACK_OF(X509)* untrusted = sk_X509_new_null();
        X509_STORE_CTX* v = X509_STORE_CTX_new();
        bool ready = anchor && untrusted && v && X509_STORE_add_cert(anchor, win.back()) == 1;
        // the sent certificates + those Windows found (a fetched intermediate)
        for (int i = 0; ready && i < sk_X509_num(sent); i++)
            ready = X509_add_cert(untrusted, sk_X509_value(sent, i), X509_ADD_FLAG_UP_REF) == 1;
        for (size_t i = 1; ready && i + 1 < win.size(); i++)
            ready = X509_add_cert(untrusted, win[i], X509_ADD_FLAG_UP_REF) == 1;
        const int idx = SSL_get_ex_data_X509_STORE_CTX_idx();
        ready = ready && X509_STORE_CTX_init(v, anchor, leaf, untrusted) == 1 &&
                X509_VERIFY_PARAM_set1(X509_STORE_CTX_get0_param(v), param) == 1 &&
                X509_STORE_CTX_set_ex_data(v, idx, X509_STORE_CTX_get_ex_data(sc, idx)) == 1;
        if (ready) {
            X509_STORE_CTX_set_verify_cb(v, X509_STORE_CTX_get_verify_cb(sc));
            r = X509_verify_cert(v);
            // what libssl reads back from sc: the result, the chain, the
            // matched peer name
            X509_STORE_CTX_set_error(sc, X509_STORE_CTX_get_error(v));
            X509_STORE_CTX_set_error_depth(sc, X509_STORE_CTX_get_error_depth(v));
            X509_STORE_CTX_set0_verified_chain(sc, X509_STORE_CTX_get1_chain(v));
            X509_VERIFY_PARAM_move_peername(param, X509_STORE_CTX_get0_param(v));
        }
        X509_STORE_CTX_free(v);
        sk_X509_pop_free(untrusted, X509_free);
        X509_STORE_free(anchor);
    }
    for (X509* x : win) X509_free(x);
    return r;
}

}  // namespace

int tls_load_system_roots(SSL_CTX* ctx) {
    std::call_once(g_roots_once, build_roots);
    if (!g_roots || g_root_count <= 0) return 0;
    SSL_CTX_set1_cert_store(ctx, g_roots);   // the context takes a reference
    SSL_CTX_set_cert_verify_callback(ctx, verify_chain, nullptr);
    return 1;
}

#else

int tls_load_system_roots(SSL_CTX* ctx) { return SSL_CTX_set_default_verify_paths(ctx); }

#endif

// --- the web UI's certificate (docs/dev/https-certificate.md) ------------------------

namespace {

using std::string;
using std::vector;

struct X509Del {
    void operator()(X509* x) const { X509_free(x); }
};
struct PkeyDel {
    void operator()(EVP_PKEY* k) const { EVP_PKEY_free(k); }
};
using X509Ptr = std::unique_ptr<X509, X509Del>;
using PkeyPtr = std::unique_ptr<EVP_PKEY, PkeyDel>;

string join(const string& dir, const string& name) {
    if (dir.empty() || dir == ".") return name;
    const char last = dir.back();
    return last == '/' || last == '\\' ? dir + name : dir + "/" + name;
}

bool file_nonempty(const string& path) {
    struct stat st;
    return ::stat(path.c_str(), &st) == 0 && st.st_size > 0;
}

bool is_symlink(const string& path) {
#ifdef _WIN32
    // a symbolic link / junction is a reparse point - but so is a OneDrive
    // placeholder (Desktop, Documents): only the reparse tags of a link count
    // (the path is UTF-8: win/app.manifest sets the UTF-8 code page).
    // UNTESTED: no MinGW on the development machine
    WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA(path.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    FindClose(h);
    return (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 &&
           (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK || fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
#else
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
#endif
}

bool read_file(const string& path, string* out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return true;
}

// ASCII only, whatever the locale (make-cert.sh: tr under LC_ALL=C)
string lower(string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// white space as make-cert.sh's [[:space:]] (LC_ALL=C)
constexpr const char* SPACES = " \t\r\n\v\f";

string trim(const string& s) {
    const size_t b = s.find_first_not_of(SPACES);
    if (b == string::npos) return {};
    return s.substr(b, s.find_last_not_of(SPACES) - b + 1);
}

// every trailing dot off (make-cert.sh's nodots)
string no_trailing_dots(string s) {
    while (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}

string openssl_error(const char* what) {
    string s = what;
    if (const unsigned long e = ERR_get_error()) {
        char b[256];
        ERR_error_string_n(e, b, sizeof b);
        s += string(": ") + b;
    }
    ERR_clear_error();
    return s;
}

// every certificate of a PEM file, in order (server.crt: the server's, then the chain)
vector<X509Ptr> load_certs(const string& path) {
    vector<X509Ptr> v;
    string pem;
    if (!read_file(path, &pem) || pem.empty()) return v;
    BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    while (b) {
        X509* x = PEM_read_bio_X509(b, nullptr, nullptr, nullptr);
        if (!x) break;
        v.emplace_back(x);
    }
    BIO_free(b);
    ERR_clear_error();   // the end of the file reads as an error
    return v;
}

// never asks for a passphrase on the terminal: an encrypted key just doesn't load
int no_passphrase(char*, int, int, void*) { return 0; }

// fine rule 27: a key uSockets can load - PEM (PEM_read_* reads nothing
// else), unencrypted (make-cert.sh's key_pub: the same markers)
PkeyPtr load_key(const string& path) {
    string pem;
    if (!read_file(path, &pem) || pem.empty()) return nullptr;
    if (pem.find("ENCRYPTED PRIVATE KEY") != string::npos || pem.find("4,ENCRYPTED") != string::npos) return nullptr;
    BIO* b = BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size()));
    EVP_PKEY* k = b ? PEM_read_bio_PrivateKey(b, nullptr, no_passphrase, nullptr) : nullptr;
    BIO_free(b);
    ERR_clear_error();
    return PkeyPtr(k);
}

bool same_key(const EVP_PKEY* a, const EVP_PKEY* b) {
    if (!a || !b) return false;
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    return EVP_PKEY_eq(a, b) == 1;
#else
    return EVP_PKEY_cmp(a, b) == 1;
#endif
}

EVP_PKEY* make_rsa_key(int bits) {
    EVP_PKEY* key = nullptr;
    EVP_PKEY_CTX* kc = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (kc && EVP_PKEY_keygen_init(kc) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(kc, bits) > 0)
        EVP_PKEY_keygen(kc, &key);
    EVP_PKEY_CTX_free(kc);
    return key;
}

// the PEM text written by `write` into a memory BIO
template <typename F>
bool pem_text(F write, string* out) {
    BIO* b = BIO_new(BIO_s_mem());
    if (!b) return false;
    bool ok = write(b) == 1;
    char* p = nullptr;
    const long n = BIO_get_mem_data(b, &p);
    if (ok && n > 0) out->assign(p, static_cast<size_t>(n));
    BIO_free(b);
    return ok && n > 0;
}

string cert_pem(X509* x) {
    string s;
    pem_text([&](BIO* b) { return PEM_write_bio_X509(b, x); }, &s);
    return s;
}

// the first entry of that kind in a name (UTF-8), "" if none
string name_entry(X509_NAME* n, int nid) {   // (not const: OpenSSL 1.1.1)
    const int i = n ? X509_NAME_get_index_by_NID(n, nid, -1) : -1;
    if (i < 0) return {};
    unsigned char* u = nullptr;
    const int len = ASN1_STRING_to_UTF8(&u, X509_NAME_ENTRY_get_data(X509_NAME_get_entry(n, i)));
    if (len < 0) return {};
    string s(reinterpret_cast<char*>(u), static_cast<size_t>(len));
    OPENSSL_free(u);
    return s;
}

string ip_text(const unsigned char* p, int len) {
    char b[64] = {};
    if (len == 4) return inet_ntop(AF_INET, p, b, sizeof b) ? string(b) : string();
    if (len == 16) return inet_ntop(AF_INET6, p, b, sizeof b) ? string(b) : string();
    return {};
}

// a textual IP in the form ip_text gives (::1, not 0:0:...:1); "" if none
string ip_normal(const string& s) {
    unsigned char b[16];
    if (inet_pton(AF_INET, s.c_str(), b) == 1) return ip_text(b, 4);
    if (inet_pton(AF_INET6, s.c_str(), b) == 1) return ip_text(b, 16);
    return {};
}

// subjectAltName: the DNS names (lowercased: compared case-insensitively, as
// make-cert.sh does) and IPs; *other = an entry of another kind (or an IP of
// another length) - such a set never equals the wanted one
void san_of(X509* x, vector<string>* dns, vector<string>* ips, bool* other = nullptr) {
    auto* g = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(x, NID_subject_alt_name, nullptr, nullptr));
    if (!g) return;
    for (int i = 0; i < sk_GENERAL_NAME_num(g); i++) {
        const GENERAL_NAME* n = sk_GENERAL_NAME_value(g, i);
        if (n->type == GEN_DNS) {
            const ASN1_STRING* s = n->d.dNSName;
            dns->push_back(lower(string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(s)),
                                        static_cast<size_t>(ASN1_STRING_length(s)))));
        } else if (n->type == GEN_IPADD) {
            const ASN1_OCTET_STRING* s = n->d.iPAddress;
            const string t = ip_text(ASN1_STRING_get0_data(s), ASN1_STRING_length(s));
            if (!t.empty()) ips->push_back(t);
            else if (other) *other = true;
        } else if (other) {
            *other = true;
        }
    }
    GENERAL_NAMES_free(g);
}

int64_t unix_time(const ASN1_TIME* t) {
    int days = 0, secs = 0;
    if (!t || ASN1_TIME_diff(&days, &secs, nullptr, t) != 1) return 0;
    return static_cast<int64_t>(std::time(nullptr)) + static_cast<int64_t>(days) * 86400 + secs;
}

string fingerprint(X509* x, const EVP_MD* md) {
    unsigned char d[EVP_MAX_MD_SIZE];
    unsigned int n = 0;
    if (X509_digest(x, md, d, &n) != 1) return {};
    static const char* hex = "0123456789ABCDEF";
    string s;
    for (unsigned int i = 0; i < n; i++) {
        if (i) s += ':';
        s += hex[d[i] >> 4];
        s += hex[d[i] & 15];
    }
    return s;
}

// A CA's name constraints (RFC 5280 4.2.1.10): a type without a permitted
// subtree is unconstrained - why the narrow scope names 127.0.0.1/32 and ::1
struct Constraints {
    bool present = false;
    vector<string> dns;   // permitted DNS subtrees
    vector<string> ip;    // permitted iPAddress subtrees: address + mask bytes (8 or 32)
};

Constraints constraints_of(X509* ca) {
    Constraints c;
    auto* nc = static_cast<NAME_CONSTRAINTS*>(X509_get_ext_d2i(ca, NID_name_constraints, nullptr, nullptr));
    if (!nc) return c;
    c.present = true;
    STACK_OF(GENERAL_SUBTREE)* st = nc->permittedSubtrees;
    for (int i = 0; st && i < sk_GENERAL_SUBTREE_num(st); i++) {
        const GENERAL_NAME* b = sk_GENERAL_SUBTREE_value(st, i)->base;
        if (b->type == GEN_DNS)
            c.dns.push_back(lower(string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(b->d.dNSName)),
                                         static_cast<size_t>(ASN1_STRING_length(b->d.dNSName)))));
        else if (b->type == GEN_IPADD)
            c.ip.push_back(string(reinterpret_cast<const char*>(ASN1_STRING_get0_data(b->d.iPAddress)),
                                  static_cast<size_t>(ASN1_STRING_length(b->d.iPAddress))));
    }
    NAME_CONSTRAINTS_free(nc);
    return c;
}

// OpenSSL's rule (nc_dns), make-cert.sh's dns_permitted: the name itself,
// or any name under it (".x": only under it)
bool dns_under(const string& name, const string& base) {
    if (base.empty()) return true;
    if (name.size() < base.size()) return false;
    if (name.size() > base.size() && base[0] != '.' && name[name.size() - base.size() - 1] != '.') return false;
    return name.compare(name.size() - base.size(), base.size(), base) == 0;
}

// Would a new CA be needed for this name? As make-cert.sh decides it: the
// permitted DNS subtrees only (no DNS subtree at all = every name). The IP
// subtrees follow from the scope, which is compared on its own.
bool dns_permitted(const Constraints& c, const string& name) {
    if (c.dns.empty()) return true;
    return std::any_of(c.dns.begin(), c.dns.end(), [&](const string& b) { return dns_under(name, b); });
}

// The CA's scope: "wide" when it permits 10.0.0.0/8, "narrow" otherwise,
// "none" without a Name Constraints extension (never one of ours: it gets a
// new CA, like any other scope mismatch) - make-cert.sh's rule
string ca_scope(X509* ca) {
    const Constraints c = constraints_of(ca);
    if (!c.present) return "none";
    static const string ten("\x0a\x00\x00\x00\xff\x00\x00\x00", 8);
    return std::find(c.ip.begin(), c.ip.end(), ten) != c.ip.end() ? "wide" : "narrow";
}

// What a browser checks: issued by this CA (its name), the chain, name
// constraints, purpose - and with check_time the validity too. Without it
// (the managed check, as make-cert.sh's verify -no_check_time) a clock
// behind or an expired certificate doesn't make ours look foreign.
bool verifies(X509* leaf, X509* ca, string* why = nullptr, bool check_time = true) {
    if (!leaf || !ca || X509_NAME_cmp(X509_get_issuer_name(leaf), X509_get_subject_name(ca)) != 0) return false;
    X509_STORE* st = X509_STORE_new();
    X509_STORE_CTX* c = X509_STORE_CTX_new();
    bool ok = st && c && X509_STORE_add_cert(st, ca) == 1 && X509_STORE_CTX_init(c, st, leaf, nullptr) == 1 &&
              X509_STORE_CTX_set_purpose(c, X509_PURPOSE_SSL_SERVER) == 1;
    if (ok) {
        if (!check_time) X509_STORE_CTX_set_flags(c, X509_V_FLAG_NO_CHECK_TIME);
        ok = X509_verify_cert(c) == 1;
        if (!ok && why) *why = X509_verify_cert_error_string(X509_STORE_CTX_get_error(c));
    }
    X509_STORE_CTX_free(c);
    X509_STORE_free(st);
    ERR_clear_error();
    return ok;
}

// PR #5's self-signed one (O=KrakenSDR receiver) or the one before it
// (exactly CN=krakensdr) - managed only when self_signed() too (fine rule 15)
bool old_managed_subject(X509* x) {
    X509_NAME* n = X509_get_subject_name(x);
    if (name_entry(n, NID_organizationName) == tlscert::ORG) return true;
    return X509_NAME_entry_count(n) == 1 && name_entry(n, NID_commonName) == "krakensdr";
}

// Fine rule 15: self-issued, and its signature verifies with its own key -
// for the signatures make-cert.sh's self_signed checks (RSA PKCS#1 and ECDSA
// with SHA-1 / SHA-2), so both decide alike
bool self_signed(X509* x) {
    if (X509_NAME_cmp(X509_get_issuer_name(x), X509_get_subject_name(x)) != 0) return false;
    switch (X509_get_signature_nid(x)) {
        case NID_sha1WithRSAEncryption:
        case NID_sha256WithRSAEncryption:
        case NID_sha384WithRSAEncryption:
        case NID_sha512WithRSAEncryption:
        case NID_ecdsa_with_SHA1:
        case NID_ecdsa_with_SHA256:
        case NID_ecdsa_with_SHA384:
        case NID_ecdsa_with_SHA512: break;
        default: return false;
    }
    EVP_PKEY* k = X509_get0_pubkey(x);
    const bool ok = k && X509_verify(x, k) == 1;
    ERR_clear_error();
    return ok;
}

// Fine rule 16: limited to TLS servers - extendedKeyUsage exactly serverAuth
// (make-cert.sh: the line "TLS Web Server Authentication")
bool server_auth_only(X509* x) {
    auto* eku = static_cast<EXTENDED_KEY_USAGE*>(X509_get_ext_d2i(x, NID_ext_key_usage, nullptr, nullptr));
    if (!eku) return false;
    const bool ok = sk_ASN1_OBJECT_num(eku) == 1 && OBJ_obj2nid(sk_ASN1_OBJECT_value(eku, 0)) == NID_server_auth;
    EXTENDED_KEY_USAGE_free(eku);
    return ok;
}

// a CA certificate: basicConstraints with CA:TRUE (make-cert.sh: "CA:TRUE" in its text)
bool is_ca_cert(X509* x) {
    const uint32_t f = X509_get_extension_flags(x);
    return (f & EXFLAG_BCONS) && (f & EXFLAG_CA);
}

// A DNS label (fine rule 6, make-cert.sh's valid_label): 1-63 of a-z 0-9 -,
// not starting or ending with - (lowercased before)
bool valid_label(const string& s) {
    return !s.empty() && s.size() <= 63 && s.front() != '-' && s.back() != '-' &&
           std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

// A DNS name (make-cert.sh's valid_dns): such labels joined by dots, 253
// characters at most
bool valid_dns(const string& s) {
    if (s.empty() || s.size() > 253) return false;
    for (size_t b = 0;;) {
        const size_t e = s.find('.', b);
        if (!valid_label(s.substr(b, e == string::npos ? string::npos : e - b))) return false;
        if (e == string::npos) return true;
        b = e + 1;
    }
}

// An IPv4 address as inet_pton (glibc) parses it - exactly four decimal
// parts 0-255, no leading zero - written out here so that every platform
// (and make-cert.sh's is_ipv4) decides alike
bool is_ipv4(const string& s) {
    int parts = 0;
    for (size_t i = 0;;) {
        size_t j = i;
        while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
        const size_t len = j - i;
        if (len == 0 || len > 3 || (len > 1 && s[i] == '0') || std::stoi(s.substr(i, len)) > 255) return false;
        parts++;
        if (j == s.size()) return parts == 4;
        if (s[j] != '.' || parts == 4) return false;
        i = j + 1;
    }
}

bool is_ipv6(const string& s) {
    unsigned char b[16];
    return s.find(':') != string::npos && inet_pton(AF_INET6, s.c_str(), b) == 1;
}

#ifndef _WIN32
// the output of a helper program (first line, trimmed); "" when it isn't installed or fails
string helper_line(const vector<string>& argv) {
    if (kproc::find_in_path(argv[0]).empty()) return {};
    string out;
    if (!kproc::run_capture(argv, 3000, &out, nullptr, 4096, true)) return {};
    return trim(out.substr(0, out.find('\n')));
}
#endif

#ifdef _WIN32
// The up adapters' IPv4 addresses (not loopback / link-local) and DNS
// suffixes (Windows' counterpart of resolv.conf's search domains).
// UNTESTED: no MinGW on the development machine.
void win_adapters(vector<string>* ipv4, vector<string>* suffixes) {
    ULONG size = 16 * 1024;
    vector<unsigned char> buf;
    ULONG r = ERROR_BUFFER_OVERFLOW;
    for (int tries = 0; tries < 3 && r == ERROR_BUFFER_OVERFLOW; tries++) {
        buf.assign(size, 0);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER,
                                 nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()), &size);
    }
    if (r != NO_ERROR) return;
    for (auto* a = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data()); a; a = a->Next) {
        if (a->OperStatus != IfOperStatusUp || a->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        if (ipv4) {
            for (auto* u = a->FirstUnicastAddress; u; u = u->Next) {
                const sockaddr* sa = u->Address.lpSockaddr;
                if (!sa || sa->sa_family != AF_INET) continue;
                const auto* p = reinterpret_cast<const unsigned char*>(&reinterpret_cast<const sockaddr_in*>(sa)->sin_addr);
                if (p[0] == 127 || (p[0] == 169 && p[1] == 254)) continue;
                const string t = ip_text(p, 4);
                if (!t.empty()) ipv4->push_back(t);
            }
        }
        if (suffixes && a->DnsSuffix && a->DnsSuffix[0]) {
            const int n = WideCharToMultiByte(CP_UTF8, 0, a->DnsSuffix, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                string s(static_cast<size_t>(n - 1), '\0');
                WideCharToMultiByte(CP_UTF8, 0, a->DnsSuffix, -1, s.data(), n, nullptr, nullptr);
                suffixes->push_back(s);
            }
        }
    }
}
#endif

// The IPv4 addresses of the interfaces that are up, without the loopback
// interface, 127/8 and link-local 169.254/16 (fine rule 8): what make-cert.sh
// reads from `ip -4 -o addr show up` without `lo` (macOS: `ifconfig -u`
// without loopback interfaces). getifaddrs names an alias address by its
// label ("lo:1"): the part before ':' is the interface. Windows: the up
// adapters' (no script to match)
vector<string> up_ipv4() {
    vector<string> v;
#ifdef _WIN32
    win_adapters(&v, nullptr);
#else
    struct ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return v;
    for (struct ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
        if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
        if (i->ifa_name && string(i->ifa_name).substr(0, string(i->ifa_name).find(':')) == "lo") continue;
        const auto* p = reinterpret_cast<const unsigned char*>(&reinterpret_cast<const sockaddr_in*>(i->ifa_addr)->sin_addr);
        if (p[0] == 127 || (p[0] == 169 && p[1] == 254)) continue;
        const string t = ip_text(p, 4);
        if (!t.empty()) v.push_back(t);
    }
    freeifaddrs(list);
#endif
    return v;
}

// search / domain entries of resolv.conf (Windows: the adapters' DNS suffixes)
vector<string> search_domains() {
    vector<string> v;
#ifdef _WIN32
    win_adapters(nullptr, &v);
#else
    // KRAKEN_CERT_RESOLV_CONF: tests only, as in make-cert.sh
    const char* alt = std::getenv("KRAKEN_CERT_RESOLV_CONF");
    // fine rule 30: the LAST search line (what glibc uses) and the last
    // domain line
    std::ifstream f(alt && *alt ? alt : "/etc/resolv.conf");
    string line;
    vector<string> search, domain;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        string key, d;
        ls >> key;
        if (key != "search" && key != "domain") continue;
        vector<string>& to = key == "search" ? search : domain;
        to.clear();
        while (ls >> d) to.push_back(d);
    }
    v = search;
    v.insert(v.end(), domain.begin(), domain.end());
#endif
    return v;
}

#ifndef _WIN32
#ifndef __APPLE__
// the name avahi announces: after an mDNS conflict the second "krakensdr"
// becomes "krakensdr-2", which hostname doesn't know (fine rule 7, as
// make-cert.sh asks: busctl, then gdbus - 2 s timeouts each, once avahi's
// GetState has settled, waited for at most 3 s - then avahi-resolve; each
// only if installed)
string avahi_host() {
    string s;
    const bool busctl = !kproc::find_in_path("busctl").empty();
    // still registering (1) or resolving a conflict (3): let it settle (2 = running)
    for (int i = 0; i < 6; i++) {
        s = busctl ? helper_line({"busctl", "--timeout=2", "call", "org.freedesktop.Avahi", "/",
                                  "org.freedesktop.Avahi.Server", "GetState"})   // i 2
                   : helper_line({"gdbus", "call", "--system", "--timeout", "2", "--dest", "org.freedesktop.Avahi",
                                  "--object-path", "/", "--method", "org.freedesktop.Avahi.Server.GetState"});   // (2,)
        if (s != "i 1" && s != "i 3" && s != "(1,)" && s != "(3,)") break;
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    if (busctl) {
        s = helper_line({"busctl", "--timeout=2", "call", "org.freedesktop.Avahi", "/", "org.freedesktop.Avahi.Server",
                         "GetHostName"});   // s "krakensdr-2"
        if (s.size() > 4 && s.compare(0, 3, "s \"") == 0 && s.back() == '"') return s.substr(3, s.size() - 4);
    }
    s = helper_line({"gdbus", "call", "--system", "--timeout", "2", "--dest", "org.freedesktop.Avahi", "--object-path", "/",
                     "--method", "org.freedesktop.Avahi.Server.GetHostName"});   // ('krakensdr-2',)
    if (s.size() > 5 && s.compare(0, 2, "('") == 0 && s.compare(s.size() - 3, 3, "',)") == 0)
        return s.substr(2, s.size() - 5);
    // "192.168.1.5	krakensdr-2.local" for a private IPv4 address - an mDNS answer only
    const vector<string> ips = tlscert::private_ipv4();
    if (!ips.empty()) {
        std::istringstream ws(helper_line({"avahi-resolve", "-a", ips[0]}));
        string a, n;
        if (ws >> a >> n) {
            n = no_trailing_dots(n);
            if (n.size() > 6 && n.compare(n.size() - 6, 6, ".local") == 0) return n.substr(0, n.size() - 6);
        }
    }
    return {};
}
#endif
#endif

// Fine rule 23: is the clock known to be synchronized? Linux: what
// `timedatectl show -p NTPSynchronized --value` says (as make-cert.sh), and
// only without an answer from it the kernel's adjtimex state; macOS /
// Windows: yes
bool clock_synced() {
#if defined(_WIN32) || defined(__APPLE__)
    return true;
#else
    const string v = helper_line({"timedatectl", "show", "-p", "NTPSynchronized", "--value"});
    if (v == "yes" || v == "no") return v == "yes";
    struct timex tx {};
    const int r = adjtimex(&tx);
    return r != -1 && r != TIME_ERROR && !(tx.status & STA_UNSYNC);
#endif
}

// Fine rule 25: a file's CERTIFICATE blocks only, line by line (make-cert.sh's
// cert_blocks: the same lines); has_private_key: a key in it
string cert_blocks(const string& text) {
    string out;
    bool in = false;
    for (size_t b = 0; b < text.size();) {
        size_t e = text.find('\n', b);
        const string line = text.substr(b, e == string::npos ? string::npos : e - b);
        if (line.find("-----BEGIN CERTIFICATE-----") != string::npos) in = true;
        if (in) out += line + "\n";
        if (line.find("-----END CERTIFICATE-----") != string::npos) in = false;
        if (e == string::npos) break;
        b = e + 1;
    }
    return out;
}
bool has_private_key(const string& text) { return text.find("PRIVATE KEY-----") != string::npos; }

#ifdef _WIN32
// A security descriptor whose DACL lets only this user, SYSTEM and the
// Administrators open the file - the Windows form of mode 0600: protected,
// nothing inherited from the folder (whose ACL may let every local user read
// it). LocalFree it; nullptr on failure.
PSECURITY_DESCRIPTOR owner_only_sd() {
    std::wstring sddl;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD n = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
        vector<unsigned char> tu(n);
        LPWSTR sid = nullptr;
        if (n > 0 && GetTokenInformation(tok, TokenUser, tu.data(), n, &n) &&
            ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(tu.data())->User.Sid, &sid)) {
            sddl = std::wstring(L"D:P(A;;FA;;;") + sid + L")(A;;FA;;;SY)(A;;FA;;;BA)";
            LocalFree(sid);
        }
        CloseHandle(tok);
    }
    PSECURITY_DESCRIPTOR sd = nullptr;
    if (sddl.empty() ||
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr))
        return nullptr;
    return sd;
}

// sd's DACL on an open file (opened with WRITE_DAC); ERROR_SUCCESS or the error
DWORD set_owner_only_dacl(HANDLE h, PSECURITY_DESCRIPTOR sd) {
    PACL dacl = nullptr;
    BOOL present = FALSE, defaulted = FALSE;
    return GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted)
               ? SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                 nullptr, nullptr, dacl, nullptr)
               : GetLastError();
}

// fine rule 13 on Windows: a key file that is there gets the owner-only DACL
// again (also on "kept"; e.g. one copied in from another receiver). UNTESTED:
// no MinGW on the development machine.
void restrict_existing(const string& path) {
    PSECURITY_DESCRIPTOR sd = owner_only_sd();
    if (!sd) return;
    HANDLE h = CreateFileA(path.c_str(), WRITE_DAC, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        set_owner_only_dacl(h, sd);
        CloseHandle(h);
    }
    LocalFree(sd);
}

// fopen(path, "wb") for a file with that DACL. A file that was already there
// gets it too (CreateFile keeps an existing file's). nullptr + errno on
// failure.
std::FILE* fopen_owner_only(const string& path) {
    PSECURITY_DESCRIPTOR sd = owner_only_sd();
    if (!sd) {
        errno = EACCES;
        return nullptr;
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    // the path is UTF-8 like fopen's (win/app.manifest: activeCodePage UTF-8)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE | WRITE_DAC, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD e = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (h != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS) e = set_owner_only_dacl(h, sd);
    LocalFree(sd);
    if (e != ERROR_SUCCESS) {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        errno = e == ERROR_ACCESS_DENIED || e == ERROR_SHARING_VIOLATION ? EACCES
                : e == ERROR_PATH_NOT_FOUND                               ? ENOENT
                                                                          : EIO;
        return nullptr;
    }
    const int fd = _open_osfhandle(reinterpret_cast<intptr_t>(h), _O_WRONLY | _O_BINARY);
    if (fd < 0) {
        CloseHandle(h);
        errno = EIO;
        return nullptr;
    }
    std::FILE* f = _fdopen(fd, "wb");
    if (!f) _close(fd);
    return f;
}
#endif

// whole file, flushed to the disk; owner_only: mode 0600 (Windows: a DACL of
// this user, SYSTEM and the Administrators only), else 0644
bool write_file(const string& path, const string& data, bool owner_only, string* err) {
#ifdef _WIN32
    std::FILE* f = owner_only ? fopen_owner_only(path) : std::fopen(path.c_str(), "wb");
#else
    ::unlink(path.c_str());   // a leftover temp file keeps its old mode otherwise
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, owner_only ? 0600 : 0644);
    if (fd >= 0) ::fchmod(fd, owner_only ? 0600 : 0644);   // whatever the umask
    std::FILE* f = fd >= 0 ? fdopen(fd, "wb") : nullptr;
    if (!f && fd >= 0) ::close(fd);
#endif
    if (!f) {
        *err = "cannot write " + path + " (" + std::strerror(errno) + ")";
        return false;
    }
    bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size() && std::fflush(f) == 0 &&
              fsync(fileno(f)) == 0;
    if (std::fclose(f) != 0) ok = false;
    if (!ok) {
        *err = "cannot write " + path + " (" + std::strerror(errno) + ")";
        std::remove(path.c_str());
    }
    return ok;
}

bool add_ext(X509* x, X509* issuer, int nid, const string& value) {
    X509V3_CTX v3{};
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, issuer, x, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, nid, value.c_str());
    if (!ext) return false;
    const bool ok = X509_add_ext(x, ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
}

// version 3, a random positive 127-bit serial, valid from `start` (fine rule
// 9: now - 1 h, a device clock a little behind) for exactly `days` days.
// ASN1_TIME_set writes UTCTime before 2050, as make-cert.sh's asn1_time
bool cert_basics(X509* x, std::time_t start, int days) {
    unsigned char sb[16];
    if (RAND_bytes(sb, sizeof sb) != 1) return false;
    sb[0] &= 0x7F;
    sb[0] |= 0x01;   // never a short (leading-zero) serial
    BIGNUM* bn = BN_bin2bn(sb, sizeof sb, nullptr);
    const bool ok = bn && BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x)) != nullptr;
    BN_free(bn);
    return ok && X509_set_version(x, 2) == 1 && ASN1_TIME_set(X509_getm_notBefore(x), start) != nullptr &&
           ASN1_TIME_set(X509_getm_notAfter(x), start + static_cast<std::time_t>(days) * 86400) != nullptr;
}

// Expires within `days` days (or has expired) - exactly `openssl x509
// -checkend`'s test, which make-cert.sh uses (fine rule 9)
bool expires_within(X509* x, int days) {
    std::time_t t = std::time(nullptr) + static_cast<std::time_t>(days) * 86400;
    return X509_cmp_time(X509_get0_notAfter(x), &t) < 0;
}

// notBefore more than `secs` after the clock (fine rule 23)
bool starts_after(X509* x, long secs) {
    std::time_t t = std::time(nullptr) + secs;
    return X509_cmp_time(X509_get0_notBefore(x), &t) > 0;
}

// a name taken by anything (also a dangling symlink): the retired / moved-aside names
bool name_taken(const string& path) {
    struct stat st;
    return ::lstat(path.c_str(), &st) == 0;
}

// <stem>-YYYYMMDD<post>, then -2, -3 ... by which names are taken (UTC: fine rules 2, 12)
string free_dated_name(const string& stem, const string& date, const string& post) {
    string n = stem + date + post;
    for (int i = 2; name_taken(n) && i < 10000; i++) n = stem + date + "-" + std::to_string(i) + post;
    return n;
}

// fine rule 13: a key file left readable by others is set to 0600 again (a
// symlink - not ours - is left alone); Windows: the owner-only DACL
void tighten_key(const string& path) {
    if (is_symlink(path)) return;
    struct stat st;
    if (::lstat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) return;
#ifdef _WIN32
    restrict_existing(path);
#else
    if ((st.st_mode & 0777) != 0600) ::chmod(path.c_str(), 0600);
#endif
}

// <dir>/.make-cert.lock (fine rule 14): a folder holding the time it was
// taken (Unix seconds in "since"), exactly as make-cert.sh's take_lock - so
// the script (run.sh, install.sh, a manual run) and this generator never
// write at the same time. Waited for up to 10 s. A dead run's lock - older
// than 120 s, dated more than 60 s ahead (the clock was set back: a Pi
// without a clock after a power cut), or without its time once the wait is
// over - is taken over by renaming it away first: of two waiters only one
// rename succeeds, and one that renamed a NEW lock (made meanwhile) puts it
// back. Removed when the guard goes. A folder that can't be made at all (a
// read-only <dir>): no lock - nothing can be written there either.
class DirLock {
public:
    DirLock(const string& dir, string path) : dir_(dir), path_(std::move(path)) {}
    DirLock(const DirLock&) = delete;
    DirLock& operator=(const DirLock&) = delete;
    bool held() const { return held_; }
    ~DirLock() {
        if (!held_) return;
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }
    bool take(string* err) {
        namespace fs = std::filesystem;
        const auto t0 = std::chrono::steady_clock::now();
        unsigned char r[4] = {};
        RAND_bytes(r, sizeof r);
        char tag[16];
        std::snprintf(tag, sizeof tag, "%02x%02x%02x%02x", r[0], r[1], r[2], r[3]);
        const string stale = join(dir_, string(".make-cert.stale.") + tag);
        for (;;) {
            std::error_code ec;
            if (fs::create_directory(path_, ec)) {
                held_ = true;
                std::ofstream(join(path_, "since")) << static_cast<long long>(std::time(nullptr)) << '\n';
                return true;
            }
            if (!fs::is_directory(path_, ec)) return true;   // can't be made (read-only <dir>)
            const string since = since_of(path_);
            const long long now = static_cast<long long>(std::time(nullptr));
            const auto waited = std::chrono::steady_clock::now() - t0;
            const bool dead = since.empty() ? waited >= std::chrono::seconds(10)
                                            : now - std::stoll(since) > 120 || std::stoll(since) - now > 60;
            if (dead) {
                fs::remove_all(stale, ec);
                fs::rename(path_, stale, ec);
                if (!ec) {
                    // the one judged dead? else a live one made meanwhile: back
                    if (since_of(stale) == since) {
                        fs::remove_all(stale, ec);
                    } else {
                        fs::rename(stale, path_, ec);
                        if (ec) fs::remove_all(stale, ec);
                    }
                    continue;
                }
                // can't be renamed while it is still the dead one: a read-only <dir>
                if (fs::is_directory(path_, ec) && since_of(path_) == since) return true;
                continue;
            }
            if (waited >= std::chrono::seconds(10)) {
                *err = "another make-cert.sh or kraken_doa is writing the certificate (" + path_ +
                       " for 10 s) - nothing was changed";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

private:
    // a lock's time: its digits, "" when it has none (make-cert.sh's lock_since)
    static string since_of(const string& lock) {
        string v;
        read_file(join(lock, "since"), &v);
        v = trim(v);
        if (v.size() > 18 || !std::all_of(v.begin(), v.end(), [](char c) { return c >= '0' && c <= '9'; })) v.clear();
        return v;
    }
    string dir_, path_;
    bool held_ = false;
};

// fine rule 12: a file this user may not read (another user's - made by
// `sudo ./run.sh`?) - open fails with EACCES / EPERM (make-cert.sh: test -r)
bool permission_denied(const string& path) {
    if (!name_taken(path)) return false;
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f) {
        std::fclose(f);
        return false;
    }
    return errno == EACCES || errno == EPERM;
}

bool add_name(X509_NAME* n, const char* field, const string& value) {
    return X509_NAME_add_entry_by_txt(n, field, MBSTRING_UTF8, reinterpret_cast<const unsigned char*>(value.c_str()),
                                      -1, -1, 0) == 1;
}

// the date in UTC (make-cert.sh: date -u), for the CA's name and the retired files
string today(const char* fmt) {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&t, &tm);   // Windows: MinGW's (win/posix_compat.h)
    char b[16];
    std::strftime(b, sizeof b, fmt, &tm);
    return b;
}

// The CA: CN=KrakenSDR <host> CA <date>, constraints per scope
X509Ptr make_ca(EVP_PKEY* key, const string& host, const vector<string>& names, bool wide, std::time_t start) {
    X509Ptr x(X509_new());
    if (!x || !cert_basics(x.get(), start, tlscert::CA_DAYS) || X509_set_pubkey(x.get(), key) != 1) return nullptr;
    X509_NAME* n = X509_get_subject_name(x.get());
    // the host part at most 40 characters: a CN is 64 at most (fine rule 17)
    if (!add_name(n, "CN", "KrakenSDR " + host.substr(0, 40) + " CA " + today("%Y-%m-%d")) ||
        !add_name(n, "O", tlscert::ORG) ||
        X509_set_issuer_name(x.get(), n) != 1)
        return nullptr;
    string nc = "critical";
    for (const string& d : names) nc += ",permitted;DNS:" + d;
    if (wide) {
        nc += ",permitted;DNS:local,permitted;DNS:lan";
        for (const char* ip : {"10.0.0.0/255.0.0.0", "172.16.0.0/255.240.0.0", "192.168.0.0/255.255.0.0",
                               "100.64.0.0/255.192.0.0", "127.0.0.0/255.0.0.0", "fc00::/fe00::"})
            nc += string(",permitted;IP:") + ip;
    } else {
        nc += ",permitted;IP:127.0.0.1/255.255.255.255";
    }
    nc += ",permitted;IP:::1/ffff:ffff:ffff:ffff:ffff:ffff:ffff:ffff";
    if (!add_ext(x.get(), x.get(), NID_basic_constraints, "critical,CA:TRUE,pathlen:0") ||
        !add_ext(x.get(), x.get(), NID_key_usage, "critical,keyCertSign,cRLSign") ||
        !add_ext(x.get(), x.get(), NID_subject_key_identifier, "hash") ||
        !add_ext(x.get(), x.get(), NID_ext_key_usage, "serverAuth") ||
        !add_ext(x.get(), x.get(), NID_name_constraints, nc) || X509_sign(x.get(), key, EVP_sha256()) <= 0)
        return nullptr;
    return x;
}

// The server certificate: CN=<host>, the names + IPs, signed by the CA
X509Ptr make_server(EVP_PKEY* key, X509* ca, EVP_PKEY* ca_key, const string& host, const vector<string>& names,
                    const vector<string>& ips, std::time_t start) {
    X509Ptr x(X509_new());
    if (!x || !cert_basics(x.get(), start, tlscert::SERVER_DAYS) || X509_set_pubkey(x.get(), key) != 1) return nullptr;
    X509_NAME* n = X509_get_subject_name(x.get());
    if (!add_name(n, "CN", host) || !add_name(n, "O", tlscert::ORG) ||
        X509_set_issuer_name(x.get(), X509_get_subject_name(ca)) != 1)
        return nullptr;
    string san;
    for (const string& d : names) san += (san.empty() ? "DNS:" : ",DNS:") + d;
    for (const string& ip : ips) san += (san.empty() ? "IP:" : ",IP:") + ip;
    if (!add_ext(x.get(), ca, NID_basic_constraints, "critical,CA:FALSE") ||
        !add_ext(x.get(), ca, NID_key_usage, "critical,digitalSignature,keyEncipherment") ||
        !add_ext(x.get(), ca, NID_ext_key_usage, "serverAuth") ||
        !add_ext(x.get(), ca, NID_subject_key_identifier, "hash") ||
        !add_ext(x.get(), ca, NID_authority_key_identifier, "keyid:always") ||
        !add_ext(x.get(), ca, NID_subject_alt_name, san) || X509_sign(x.get(), ca_key, EVP_sha256()) <= 0)
        return nullptr;
    return x;
}

bool same_set(vector<string> a, vector<string> b) {
    std::sort(a.begin(), a.end());
    std::sort(b.begin(), b.end());
    a.erase(std::unique(a.begin(), a.end()), a.end());
    b.erase(std::unique(b.begin(), b.end()), b.end());
    return a == b;
}

tlscert::CertInfo info_of(X509* x) {
    tlscert::CertInfo c;
    if (!x) return c;
    c.ok = true;
    c.cn = name_entry(X509_get_subject_name(x), NID_commonName);
    c.org = name_entry(X509_get_subject_name(x), NID_organizationName);
    san_of(x, &c.dns, &c.ips);
    c.not_after = unix_time(X509_get0_notAfter(x));
    c.sha256 = fingerprint(x, EVP_sha256());
    c.sha1 = fingerprint(x, EVP_sha1());
    c.pem = cert_pem(x);
    c.scope = ca_scope(x);
    const Constraints nc = constraints_of(x);
    c.permitted_dns = nc.dns;
    c.has_ip_subtree = !nc.ip.empty();
    c.server_auth_only = server_auth_only(x);
    c.not_before = unix_time(X509_get0_notBefore(x));
    return c;
}

// krakensdr-ca.YYYYMMDD.retired.crt (-2, -3 ... after the first of a day):
// the date, "" for other names
string retired_date(const string& name) {
    static const string pre = "krakensdr-ca.", post = ".retired.crt";
    if (name.size() < pre.size() + 8 + post.size() || name.compare(0, pre.size(), pre) != 0 ||
        name.compare(name.size() - post.size(), post.size(), post) != 0)
        return {};
    const string mid = name.substr(pre.size(), name.size() - pre.size() - post.size());
    if (mid.size() < 8 || !std::all_of(mid.begin(), mid.begin() + 8, [](char c) { return c >= '0' && c <= '9'; }))
        return {};
    if (mid.size() > 8 && (mid[8] != '-' || mid.size() == 9 ||
                           !std::all_of(mid.begin() + 9, mid.end(), [](char c) { return c >= '0' && c <= '9'; })))
        return {};
    return mid.substr(0, 4) + "-" + mid.substr(4, 2) + "-" + mid.substr(6, 2);
}

// The names in a folder (UTF-8). Nothing here may throw (into
// certstatus::init: a crash at every start): the iterator is advanced with an
// error_code, and a name MinGW's path can't give as UTF-8 is skipped
vector<string> folder_names(const string& dir) {
    vector<string> v;
    try {
        std::error_code ec;
        std::filesystem::directory_iterator it(dir.empty() ? "." : dir, ec), end;
        for (; !ec && it != end; it.increment(ec)) {
            try {
                const auto u = it->path().filename().u8string();
                v.emplace_back(reinterpret_cast<const char*>(u.data()), u.size());
            } catch (...) {
            }
        }
    } catch (...) {
    }
    return v;
}

// Fine rule 34: a dead run's stage / stale-lock folders (.make-cert.*, not
// the lock) older than an hour - under the lock (make-cert.sh: find -mmin +60)
void sweep_stages(const string& dir) {
    const std::time_t now = std::time(nullptr);
    for (const string& name : folder_names(dir)) {
        if (name.compare(0, 11, ".make-cert.") != 0 || name == ".make-cert.lock") continue;
        struct stat st;
        const string p = join(dir, name);
        if (::lstat(p.c_str(), &st) != 0 || !S_ISDIR(st.st_mode) || now - st.st_mtime <= 3600) continue;
        std::error_code ec;
        std::filesystem::remove_all(p, ec);
    }
}

// Fine rule 22: a name of the folder's krakensdr-ca.*.retired.crt (make-cert.sh's glob)
bool retired_name(const string& name) {
    static const string pre = "krakensdr-ca.", post = ".retired.crt";
    return name.size() >= pre.size() + post.size() && name.compare(0, pre.size(), pre) == 0 &&
           name.compare(name.size() - post.size(), post.size(), post) == 0;
}

// any entry of that kind in the name for which f(its UTF-8 value) holds
template <typename F>
bool any_entry(X509_NAME* n, int nid, F f) {
    for (int i = n ? X509_NAME_get_index_by_NID(n, nid, -1) : -1; i >= 0; i = X509_NAME_get_index_by_NID(n, nid, i)) {
        unsigned char* u = nullptr;
        const int len = ASN1_STRING_to_UTF8(&u, X509_NAME_ENTRY_get_data(X509_NAME_get_entry(n, i)));
        if (len < 0) continue;
        const string v(reinterpret_cast<char*>(u), static_cast<size_t>(len));
        OPENSSL_free(u);
        if (f(v)) return true;
    }
    return false;
}

// Fine rule 22: ours, from a CA of ours that was deleted or replaced (by a
// copied one - sharing a CA): subject and issuer both with O=KrakenSDR
// receiver, the issuer's CN starting "KrakenSDR " and containing " CA " (as
// make-cert.sh's kraken_issued); or issued (no time check) by one of the
// folder's retired CAs
bool prior_ours(X509* leaf, const string& dir) {
    auto org = [](const string& v) { return v == tlscert::ORG; };
    if (any_entry(X509_get_subject_name(leaf), NID_organizationName, org) &&
        any_entry(X509_get_issuer_name(leaf), NID_organizationName, org) &&
        any_entry(X509_get_issuer_name(leaf), NID_commonName,
                  [](const string& v) { return v.compare(0, 10, "KrakenSDR ") == 0 && v.find(" CA ") != string::npos; }))
        return true;
    for (const string& name : folder_names(dir)) {
        if (!retired_name(name)) continue;
        const vector<X509Ptr> rc = load_certs(join(dir, name));
        if (!rc.empty() && verifies(leaf, rc[0].get(), nullptr, false)) return true;
    }
    return false;
}

}  // namespace

namespace tlscert {

Host host_name() {
    Host h;
    string avahi;
    if (const char* e = std::getenv("KRAKEN_CERT_HOST"); e && *e) {
        // trimmed, not cut at a dot - a dot makes it unusable (fine rule 6)
        h.raw = trim(e);
    } else {
        // fine rule 24: the system's name; the one avahi announces (another
        // after an mDNS conflict: krakensdr-2) is an EXTRA name
#if defined(__APPLE__)
        h.raw = helper_line({"scutil", "--get", "LocalHostName"});
#elif !defined(_WIN32)
        avahi = avahi_host();
        avahi = lower(trim(avahi.substr(0, avahi.find('.'))));
#endif
        if (h.raw.empty()) {
            char b[256] = {};
            if (gethostname(b, sizeof b - 1) == 0) h.raw = b;
        }
        h.raw = trim(h.raw.substr(0, h.raw.find('.')));   // hostname -s
    }
    const string n = lower(h.raw);
    h.usable = valid_label(n);
    if (h.usable) h.name = n;
    if (valid_label(avahi) && avahi != n) h.avahi = avahi;
    return h;
}

vector<string> own_names(const Host& h) {
    if (!h.usable) return {};
    return {h.name, h.name + ".local"};
}

vector<string> env_names(vector<string>* notes) {
    vector<string> v;
    const char* e = std::getenv("KRAKEN_CERT_NAMES");
    if (!e) return v;
    // fine rule 4: split on commas and white space, lowercased, trailing dots
    // off; an address (browsers never match one against a DNS name) and what
    // is no DNS name are left out, each with a note
    string list = e;
    for (char& c : list)
        if (c == ',' || std::strchr(SPACES, c)) c = ' ';
    std::istringstream ws(list);
    string w;
    while (ws >> w) {
        const string n = no_trailing_dots(lower(w));
        if (n.empty()) continue;
        if (is_ipv4(n) || is_ipv6(n)) {
            if (notes)
                notes->push_back("KRAKEN_CERT_NAMES: " + n +
                                 " is an address - left out (browsers never match an address against a DNS name; "
                                 "wide scope names the receiver's private addresses)");
        } else if (!valid_dns(n)) {
            if (notes) notes->push_back("KRAKEN_CERT_NAMES: '" + n + "' is not a DNS name - left out");
        } else if (std::find(v.begin(), v.end(), n) == v.end()) {
            v.push_back(n);
        }
    }
    return v;
}

// make-cert.sh's list, in its order (compared as a set): an unusable host
// name still gets localhost and KRAKEN_CERT_NAMES
vector<string> wanted_names(const Host& h, vector<string>* notes) {
    vector<string> v;
    auto add = [&v](const string& s) {
        if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
    };
    for (const string& n : own_names(h)) add(n);
    if (!h.avahi.empty()) {
        add(h.avahi);
        add(h.avahi + ".local");
    }
    if (h.usable) {
        // fine rule 30: of the search / domain words (lowercased, trailing dots
        // off) only the suffixes the Host allow-list trusts; any other domain
        // only through KRAKEN_CERT_NAMES
        for (const string& raw : search_domains()) {
            const string d = no_trailing_dots(lower(raw));
            if (d == "lan" || d == "home" || d == "localdomain" || d == "home.arpa") add(h.name + "." + d);
        }
    }
    add("localhost");
    for (const string& n : env_names(notes)) add(n);
    return v;
}

bool name_permitted(const vector<string>& permitted_dns, const string& name) {
    Constraints c;
    c.dns = permitted_dns;
    return dns_permitted(c, name);
}

vector<string> private_ipv4() {
    vector<string> v;
    for (const string& ip : up_ipv4()) {
        unsigned char b[4];
        if (inet_pton(AF_INET, ip.c_str(), b) != 1) continue;
        const bool priv = b[0] == 10 || (b[0] == 172 && (b[1] & 0xF0) == 16) || (b[0] == 192 && b[1] == 168) ||
                          (b[0] == 100 && (b[1] & 0xC0) == 64);
        if (priv && std::find(v.begin(), v.end(), ip) == v.end()) v.push_back(ip);
    }
    return v;
}

vector<string> wanted_ips(bool wide) {
    vector<string> v{"127.0.0.1", "::1"};
    if (wide)
        for (const string& ip : private_ipv4()) v.push_back(ip);
    return v;
}

// Fine rule 5 (as make-cert.sh reads it): lines "scope = VALUE", spaces
// around '=' allowed, key and value in any case; the last such line wins;
// "wide" is wide, anything else (also no such line) narrow
bool read_scope_wide(const string& dir) {
    std::ifstream f(join(dir, OPTIONS));
    string line, value;
    while (std::getline(f, line)) {
        line = trim(line);
        if (lower(line.substr(0, 5)) != "scope") continue;
        const string rest = trim(line.substr(5));
        if (rest.empty() || rest[0] != '=') continue;
        value = lower(trim(rest.substr(1)));
    }
    return value == "wide";
}

// Fine rule 29: "new_ca = 1" - key in any case, spaces around '=', the last
// such line wins
bool read_new_ca(const string& dir) {
    std::ifstream f(join(dir, OPTIONS));
    string line, value;
    while (std::getline(f, line)) {
        line = trim(line);
        if (lower(line.substr(0, 6)) != "new_ca") continue;
        const string rest = trim(line.substr(6));
        if (rest.empty() || rest[0] != '=') continue;
        value = trim(rest.substr(1));
    }
    return value == "1";
}

bool write_options(const string& dir, bool wide, bool new_ca, string* err) {
    const string path = join(dir, OPTIONS), tmp = path + ".tmp";
    if (!write_file(tmp, string(wide ? "scope=wide\n" : "scope=narrow\n") + (new_ca ? "new_ca=1\n" : ""), false, err))
        return false;
    if (fs_replace(tmp, path) != 0) {
        *err = "cannot replace " + path + " (" + std::strerror(errno) + ")";
        std::remove(tmp.c_str());
        return false;
    }
    fs_fsync_dir(dir.empty() ? "." : dir);
    return true;
}

bool write_scope(const string& dir, bool wide, string* err) {
    const string path = join(dir, OPTIONS), tmp = path + ".tmp";
    if (!write_file(tmp, wide ? "scope=wide\n" : "scope=narrow\n", false, err)) return false;
    if (fs_replace(tmp, path) != 0) {
        *err = "cannot replace " + path + " (" + std::strerror(errno) + ")";
        std::remove(tmp.c_str());
        return false;
    }
    fs_fsync_dir(dir.empty() ? "." : dir);
    return true;
}

const char* state_name(State s) {
    switch (s) {
        case State::Kept: return "kept";
        case State::Issued: return "issued";
        case State::CaCreated: return "ca-created";
        case State::NotManaged: return "not-managed";
    }
    return "kept";
}

bool ensure(const string& dir, bool force, bool new_ca, State* state, string* err) {
    *state = State::Kept;
    // one maker at a time in <dir>: make-cert.sh or this (fine rule 14)
    DirLock lock(dir, join(dir, ".make-cert.lock"));
    if (!lock.take(err)) return false;
    if (lock.held()) sweep_stages(dir);
    const string crt_p = join(dir, SERVER_CRT), key_p = join(dir, SERVER_KEY);
    const string cac_p = join(dir, CA_CRT), cak_p = join(dir, CA_KEY);
    const bool crt_there = file_nonempty(crt_p), key_there = file_nonempty(key_p);

    // a symlink is the user's: never touched
    if (is_symlink(crt_p) || is_symlink(key_p)) {
        *state = State::NotManaged;
        if (crt_there && key_there) return true;
        *err = SERVER_CRT + string(" / ") + SERVER_KEY + " is a symbolic link and the pair is incomplete";
        return false;
    }
    // fine rule 12: a file this user may not read is a failure, never a
    // reason for a new key or CA
    for (const string& f : {crt_p, key_p, cac_p, cak_p}) {
        if (permission_denied(f)) {
            *err = f + " cannot be read by this user - check its owner - nothing was changed";
            return false;
        }
    }
    const vector<X509Ptr> chain = crt_there ? load_certs(crt_p) : vector<X509Ptr>();
    X509* leaf = chain.empty() ? nullptr : chain[0].get();
    if (crt_there && !leaf) {
        // fine rule 11: not something to replace blindly (make-cert.sh fails the same way)
        *err = crt_p + " holds no certificate that can be read - nothing was changed (move it away: a new one is "
                       "made then; or replace it)";
        return false;
    }
    const vector<X509Ptr> cas = file_nonempty(cac_p) ? load_certs(cac_p) : vector<X509Ptr>();
    X509* ca = cas.empty() ? nullptr : cas[0].get();
    // managed or not: issued by our CA (chain and signature, no time check:
    // fine rule 10), or a legacy subject that is self-signed (fine rule 15)
    const bool ours = leaf && verifies(leaf, ca, nullptr, false);
    const bool legacy = leaf && !ours && old_managed_subject(leaf) && self_signed(leaf);
    // fine rule 22: ours from a CA of ours that was replaced or deleted -
    // re-issued from the current CA (or a new one), its key kept
    const bool prior = leaf && !ours && !legacy && prior_ours(leaf, dir);
    if (leaf && !ours && !legacy && !prior) {
        *state = State::NotManaged;   // a certificate of the user's own: left alone
        if (key_there) return true;
        *err = string(SERVER_CRT) + " is not one of the receiver's own and " + SERVER_KEY + " is missing";
        return false;
    }
    // the keys: kept whatever their type. One that can be read but not used
    // (a passphrase, garbage, DER: fine rules 12, 27) is never deleted or
    // overwritten: it is moved aside and a new key (a new CA) is made
    PkeyPtr skey = key_there ? load_key(key_p) : nullptr;
    const bool key_aside = key_there && !skey;
    const bool cakey_there = file_nonempty(cak_p);
    PkeyPtr cakey = cakey_there ? load_key(cak_p) : nullptr;
    const bool cakey_aside = cakey_there && !cakey;
    // fine rule 31: no CA key beside a valid CA certificate (taken offline):
    // kept while nothing needs signing
    const bool key_away = !cakey_there;

    // what is wanted
    vector<string> notes;
    const bool wide = read_scope_wide(dir);
    const bool new_ca_req = read_new_ca(dir);   // fine rule 29
    const Host host = host_name();
    if (!host.usable)
        notes.push_back("the host name '" + host.raw + "' is not usable in DNS - the certificate names only localhost");
    const vector<string> names = wanted_names(host, &notes);
    const vector<string> own = own_names(host);
    const vector<string> ips = wanted_ips(wide);
    const string cn = host.usable ? host.name : "localhost";

    // a new CA when: asked to (--new-ca, new_ca=1); the CA key unusable; none
    // (or a key that doesn't fit it, or not a CA); less than a year left;
    // another scope (no name constraints - scope "none" - never matches: fine
    // rule 3); no DNS or no IP subtree; not limited to TLS servers (fine rule
    // 16); starting more than a day after a synchronized clock (fine rule 23);
    // one of the host's own names outside its DNS constraints - a rename (fine
    // rule 18)
    bool make_new_ca = new_ca || new_ca_req || cakey_aside || !ca || (!cakey && !key_away) ||
                       (cakey && !same_key(X509_get0_pubkey(ca), cakey.get())) || !is_ca_cert(ca) ||
                       expires_within(ca, CA_RENEW_DAYS) || ca_scope(ca) != (wide ? "wide" : "narrow");
    vector<string> not_covered, ca_exact;
    if (!make_new_ca) {
        const Constraints c = constraints_of(ca);
        // fine rule 35: the CA's EXACT permitted names (not the wide scope's
        // subtrees `local` / `lan`) stay in the server certificate
        for (const string& d : c.dns)
            if (d != "local" && d != "lan" && valid_dns(d) && std::find(ca_exact.begin(), ca_exact.end(), d) == ca_exact.end())
                ca_exact.push_back(d);
        make_new_ca = c.dns.empty() || c.ip.empty() || !server_auth_only(ca);
        if (!make_new_ca && starts_after(ca, 86400)) {
            if (clock_synced()) make_new_ca = true;
            else
                notes.push_back("the CA starts more than a day after this clock, which is not known to be "
                                "synchronized - kept (clock_suspect)");
        }
        for (size_t i = 0; !make_new_ca && i < names.size(); i++) {
            if (dns_permitted(c, names[i])) continue;
            if (std::find(own.begin(), own.end(), names[i]) != own.end()) make_new_ca = true;
            else not_covered.push_back(names[i]);   // left out of the server certificate
        }
    }

    const bool make_key = !skey;
    // the server certificate is re-issued (same key) when: other names / IPs;
    // less than 60 days left; it isn't issued by the current CA (a migrated
    // one); server.crt isn't exactly it + the CA; it doesn't match
    // server.key; asked to; and - only when nothing else is due - it starts
    // more than a day after a synchronized clock (fine rule 23)
    // its names: the wanted ones the CA permits (fine rule 18) and the CA's
    // exact names (fine rule 35); a new CA takes every name in
    if (make_new_ca) {
        not_covered.clear();
        ca_exact.clear();
    }
    vector<string> leaf_names;
    for (const string& n : names)
        if (std::find(not_covered.begin(), not_covered.end(), n) == not_covered.end()) leaf_names.push_back(n);
    for (const string& n : ca_exact)
        if (std::find(leaf_names.begin(), leaf_names.end(), n) == leaf_names.end()) leaf_names.push_back(n);
    bool issue = force || make_new_ca || make_key || !leaf || !ours;
    if (!issue) {
        vector<string> dns, sips, wips;
        bool other = false;
        san_of(leaf, &dns, &sips, &other);
        for (const string& ip : ips) wips.push_back(ip_normal(ip));
        // (the CN too: with the CA's names staying in - rule 35 - a rename
        // inside the CA or an unusable host name can leave the names as they were)
        issue = !same_key(X509_get0_pubkey(leaf), skey.get()) || other || !same_set(dns, leaf_names) ||
                !same_set(sips, wips) || name_entry(X509_get_subject_name(leaf), NID_commonName) != cn ||
                expires_within(leaf, SERVER_RENEW_DAYS) || chain.size() != 2 || X509_cmp(chain[1].get(), ca) != 0;
        // fine rule 23: dated from a clock that was ahead - re-issued only when
        // this clock is known to be right (else the new one would be dated from
        // a wrong time), and never a reason for a new CA (with the CA key away
        // too: kept)
        if (!issue && starts_after(leaf, 86400)) {
            if (!clock_synced())
                notes.push_back(string(SERVER_CRT) + " starts more than a day after this clock, which is not known to "
                                "be synchronized - kept");
            else if (key_away)
                notes.push_back(string(SERVER_CRT) + " starts more than a day after the clock - kept: " + CA_KEY +
                                " is missing (a clock is never a reason for a new CA)");
            else
                issue = true;
        }
    }
    // fine rule 31: something to sign and the CA key away - a new CA after all
    if (issue && !make_new_ca && key_away) {
        make_new_ca = true;
        not_covered.clear();
        leaf_names = names;
    }
    if (!not_covered.empty()) {
        string l;
        for (const string& n : not_covered) l += " " + n;
        notes.push_back("outside this receiver's CA, left out of the certificate:" + l +
                        " (a new CA takes them in - the page's New certificate authority, or make-cert.sh --new-ca; "
                        "every device then installs it)");
    }
    for (const string& n : notes) std::cerr << "HTTPS certificate: " << n << std::endl;
    if (!issue) {
        tighten_key(key_p);   // fine rule 13, also on kept
        tighten_key(cak_p);
        *state = State::Kept;
        return true;
    }

    // make everything in memory first; one start for both (fine rule 9)
    const std::time_t start = std::time(nullptr) - 3600;
    PkeyPtr new_cakey, new_skey;
    X509Ptr new_ca_crt, new_leaf;
    if (make_new_ca) {
        new_cakey.reset(make_rsa_key(3072));
        if (new_cakey) new_ca_crt = make_ca(new_cakey.get(), cn, names, wide, start);
        if (!new_ca_crt) {
            *err = openssl_error("cannot create the CA");
            return false;
        }
    }
    X509* use_ca = make_new_ca ? new_ca_crt.get() : ca;
    EVP_PKEY* use_cakey = make_new_ca ? new_cakey.get() : cakey.get();
    if (make_key) {
        new_skey.reset(make_rsa_key(2048));
        if (!new_skey) {
            *err = openssl_error("cannot create the server key");
            return false;
        }
    }
    EVP_PKEY* use_skey = make_key ? new_skey.get() : skey.get();
    new_leaf = make_server(use_skey, use_ca, use_cakey, cn, leaf_names, ips, start);
    if (!new_leaf) {
        *err = openssl_error("cannot create the server certificate");
        return false;
    }
    // what every browser will check - but not the time: the dates are ours
    // (fine rule 23; a CA starting after a clock that is behind never fails a run)
    string why;
    if (!verifies(new_leaf.get(), use_ca, &why, false)) {
        *err = "the new server certificate does not verify against its CA (" + why + ")";
        return false;
    }
    string cak_pem, cac_pem = cert_pem(use_ca), sk_pem, crt_pem = cert_pem(new_leaf.get()) + cac_pem;
    if (make_new_ca &&
        !pem_text([&](BIO* b) { return PEM_write_bio_PrivateKey(b, use_cakey, nullptr, nullptr, 0, nullptr, nullptr); },
                  &cak_pem)) {
        *err = openssl_error("cannot write the CA key");
        return false;
    }
    if (make_key &&
        !pem_text([&](BIO* b) { return PEM_write_bio_PrivateKey(b, use_skey, nullptr, nullptr, 0, nullptr, nullptr); },
                  &sk_pem)) {
        *err = openssl_error("cannot write the server key");
        return false;
    }

    // temp files next to the real ones: a failure here leaves every file as it was
    vector<string> temps;
    auto put = [&](const string& path, const string& data, bool owner_only) {
        if (!write_file(path + ".tmp", data, owner_only, err)) return false;
        temps.push_back(path + ".tmp");
        return true;
    };
    if ((make_new_ca && (!put(cak_p, cak_pem, true) || !put(cac_p, cac_pem, false))) ||
        (make_key && !put(key_p, sk_pem, true)) || !put(crt_p, crt_pem, false)) {
        for (const string& t : temps) std::remove(t.c_str());
        return false;
    }
    auto move = [&](const string& from, const string& to) {
        if (fs_replace(from, to) == 0) return true;
        *err = "cannot replace " + to + " (" + std::strerror(errno) + ")";
        for (const string& t : temps) std::remove(t.c_str());
        return false;
    };
    // keep(file, name): its certificate blocks as `name` (0644) - and the file
    // itself, holding a key, aside as <file>.unreadable-YYYYMMDD (0600) (fine
    // rule 25: a retired / saved copy never holds a key)
    auto keep = [&](const string& file, const string& name) {
        string text;
        if (!read_file(file, &text) || !write_file(name, cert_blocks(text), false, err)) {
            if (err->empty()) *err = "cannot read " + file;
            for (const string& t : temps) std::remove(t.c_str());
            return false;
        }
        if (has_private_key(text)) {
            const string base = file.substr(file.find_last_of("/\\") == string::npos ? 0 : file.find_last_of("/\\") + 1);
            const string aside = free_dated_name(join(dir, base + ".unreadable-"), today("%Y%m%d"), "");
            if (!move(file, aside)) return false;
            tighten_key(aside);
            std::cerr << "HTTPS certificate: " << file << " held a private key - moved aside to " << aside << " (0600); "
                      << name << " holds its certificates only" << std::endl;
        }
        return true;
    };
    // a migrated (self-signed) server.crt: a copy first (fine rule 15)
    if (legacy) {
        const string saved = free_dated_name(join(dir, string(SERVER_CRT) + ".replaced-"), today("%Y%m%d"), "");
        if (!keep(crt_p, saved)) return false;
        std::cerr << "HTTPS certificate: the old " << crt_p << " is kept as " << saved << std::endl;
    }
    // into place, in make-cert.sh's order (an interruption leaves a state the
    // next run repairs): old CA certificate -> retired, unusable CA key ->
    // aside, new CA key, new CA certificate, unusable server key -> aside,
    // new server key, server.crt
    if (make_new_ca) {
        // the old CA certificate is kept as .retired (the page names it:
        // devices should remove it); its key is replaced - gone, unless the CA
        // isn't one of a KrakenSDR receiver (a user's own CA put at our names:
        // its key is kept as krakensdr-ca.key.retired-YYYYMMDD, fine rule 32).
        // Only a readable certificate, not a symlink; the date in UTC, -2, -3
        // ... by which names exist (fine rules 1, 2)
        if (ca && !is_symlink(cac_p)) {
            const string retired = free_dated_name(join(dir, "krakensdr-ca."), today("%Y%m%d"), ".retired.crt");
            if (!keep(cac_p, retired)) return false;
        }
        // fine rule 32 whatever the certificate file is (a symlink too: its
        // CA's key is still in krakensdr-ca.key, which the new one replaces)
        if (ca && cakey_there && !cakey_aside &&
            !any_entry(X509_get_subject_name(ca), NID_organizationName, [](const string& v) { return v == tlscert::ORG; })) {
            const string kept = free_dated_name(join(dir, string(CA_KEY) + ".retired-"), today("%Y%m%d"), "");
            if (!move(cak_p, kept)) return false;
            tighten_key(kept);
            std::cerr << "HTTPS certificate: the replaced CA is not one of a KrakenSDR receiver - its key kept as " << kept
                      << std::endl;
        }
        if (cakey_aside) {
            const string aside = free_dated_name(join(dir, string(CA_KEY) + ".unreadable-"), today("%Y%m%d"), "");
            if (!move(cak_p, aside)) return false;
            tighten_key(aside);
            std::cerr << "HTTPS certificate: " << cak_p << " could not be read - moved aside to " << aside
                      << ", a new CA is made" << std::endl;
        }
        if (!move(cak_p + ".tmp", cak_p) || !move(cac_p + ".tmp", cac_p)) return false;
    }
    if (key_aside) {
        const string aside = free_dated_name(join(dir, string(SERVER_KEY) + ".unreadable-"), today("%Y%m%d"), "");
        if (!move(key_p, aside)) return false;
        tighten_key(aside);
        std::cerr << "HTTPS certificate: " << key_p << " could not be read - moved aside to " << aside
                  << ", a new key is made" << std::endl;
    }
    if (make_key && !move(key_p + ".tmp", key_p)) return false;
    if (!move(crt_p + ".tmp", crt_p)) return false;
    tighten_key(key_p);   // fine rule 13
    tighten_key(cak_p);
    // fine rule 29: the request is done - its line out of cert_options
    if (new_ca_req && make_new_ca) {
        string text, kept;
        read_file(join(dir, OPTIONS), &text);
        std::istringstream ls(text);
        for (string line; std::getline(ls, line);) {
            const string t = trim(line);
            const size_t eq = t.find('=');
            if (eq != string::npos && lower(trim(t.substr(0, eq))) == "new_ca") continue;
            kept += line + "\n";
        }
        string e2;
        const string tmp = join(dir, string(OPTIONS) + ".tmp");
        if (!write_file(tmp, kept, false, &e2) || fs_replace(tmp, join(dir, OPTIONS)) != 0) {
            std::remove(tmp.c_str());
            std::cerr << "HTTPS certificate: cannot take new_ca=1 out of " << OPTIONS << std::endl;
        }
    }
    fs_fsync_dir(dir.empty() ? "." : dir);
    *state = make_new_ca ? State::CaCreated : State::Issued;
    return true;
}

Inspect inspect(const string& dir) {
    Inspect r;
    const string crt_p = join(dir, SERVER_CRT);
    const vector<X509Ptr> chain = load_certs(crt_p);
    if (chain.empty()) return r;
    r.present = true;
    X509* leaf = chain[0].get();
    r.server = info_of(leaf);
    const vector<X509Ptr> cas = load_certs(join(dir, CA_CRT));
    if (!cas.empty() && verifies(leaf, cas[0].get(), nullptr, false)) {
        r.has_ca = true;
        r.ca = info_of(cas[0].get());
    }
    r.managed = !is_symlink(crt_p) && !is_symlink(join(dir, SERVER_KEY)) &&
                (r.has_ca || (old_managed_subject(leaf) && self_signed(leaf)) || prior_ours(leaf, dir));
    // the retired CAs (folder_names: no exceptions into certstatus::init)
    for (const string& name : folder_names(dir)) {
        const string date = retired_date(name);
        if (date.empty()) continue;
        const vector<X509Ptr> rc = load_certs(join(dir, name));
        if (rc.empty()) continue;
        Inspect::Retired ret;
        ret.date = date;
        ret.cert = info_of(rc[0].get());
        ret.cert.pem.clear();
        r.retired.push_back(std::move(ret));
    }
    std::sort(r.retired.begin(), r.retired.end(), [](const Inspect::Retired& a, const Inspect::Retired& b) {
        return a.date != b.date ? a.date > b.date : a.cert.not_after > b.cert.not_after;
    });
    return r;
}

}  // namespace tlscert
