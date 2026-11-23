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
    // a symbolic link / junction is a reparse point (the path is UTF-8:
    // win/app.manifest sets the UTF-8 code page)
    const DWORD a = GetFileAttributesA(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
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

string lower(string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

string trim(const string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos) return {};
    return s.substr(b, s.find_last_not_of(" \t\r\n") - b + 1);
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

PkeyPtr load_key(const string& path) {
    string pem;
    if (!read_file(path, &pem) || pem.empty()) return nullptr;
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

void san_of(X509* x, vector<string>* dns, vector<string>* ips) {
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
        }
    }
    GENERAL_NAMES_free(g);
}

int64_t unix_time(const ASN1_TIME* t) {
    int days = 0, secs = 0;
    if (!t || ASN1_TIME_diff(&days, &secs, nullptr, t) != 1) return 0;
    return static_cast<int64_t>(std::time(nullptr)) + static_cast<int64_t>(days) * 86400 + secs;
}

// whole days until the certificate expires (negative: expired)
double days_left(X509* x) {
    return static_cast<double>(unix_time(X509_get0_notAfter(x)) - static_cast<int64_t>(std::time(nullptr))) / 86400.0;
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

// PR #5's self-signed one (O=KrakenSDR receiver) or the one before it (exactly CN=krakensdr)
bool old_managed_subject(X509* x) {
    X509_NAME* n = X509_get_subject_name(x);
    if (name_entry(n, NID_organizationName) == tlscert::ORG) return true;
    return X509_NAME_entry_count(n) == 1 && name_entry(n, NID_commonName) == "krakensdr";
}

// a CA certificate: basicConstraints with CA:TRUE (make-cert.sh: "CA:TRUE" in its text)
bool is_ca_cert(X509* x) {
    const uint32_t f = X509_get_extension_flags(x);
    return (f & EXFLAG_BCONS) && (f & EXFLAG_CA);
}

// make-cert.sh's rules: a host name is usable with only a-z 0-9 - in it; a
// DNS name has a-z 0-9 . - , no leading / trailing dot and no empty label
bool usable_host(const string& s) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(), [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

bool valid_dns(const string& s) {
    if (s.empty() || s.front() == '.' || s.back() == '.' || s.find("..") != string::npos) return false;
    return std::all_of(s.begin(), s.end(),
                       [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.'; });
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

// every global IPv4 address: what make-cert.sh reads from `ip -4 addr show
// scope global` (an interface's state doesn't matter there either; loopback
// and link-local by their value - outside the private ranges anyway).
// Windows: the up adapters' (no script to match)
vector<string> global_ipv4() {
    vector<string> v;
#ifdef _WIN32
    win_adapters(&v, nullptr);
#else
    struct ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return v;
    for (struct ifaddrs* i = list; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
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
    std::ifstream f(alt && *alt ? alt : "/etc/resolv.conf");
    string line;
    while (std::getline(f, line)) {
        std::istringstream ls(line);
        string key, d;
        ls >> key;
        if (key != "search" && key != "domain") continue;
        while (ls >> d) v.push_back(d);
    }
#endif
    return v;
}

#ifndef _WIN32
#ifndef __APPLE__
// the name avahi announces: after an mDNS conflict the second "krakensdr"
// becomes "krakensdr-2", which hostname doesn't know
// (as make-cert.sh asks: busctl, gdbus, avahi-resolve - each only if installed)
string avahi_host() {
    string s;
    if (!kproc::find_in_path("busctl").empty()) {
        // still registering (1) or resolving a conflict (3): let it settle (2 = running)
        for (int i = 0; i < 6; i++) {
            s = helper_line({"busctl", "--timeout=2", "call", "org.freedesktop.Avahi", "/",
                             "org.freedesktop.Avahi.Server", "GetState"});
            if (s != "i 1" && s != "i 3") break;
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        s = helper_line({"busctl", "--timeout=2", "call", "org.freedesktop.Avahi", "/", "org.freedesktop.Avahi.Server",
                         "GetHostName"});   // s "krakensdr-2"
        if (s.size() > 4 && s.compare(0, 3, "s \"") == 0 && s.back() == '"') return s.substr(3, s.size() - 4);
    }
    s = helper_line({"gdbus", "call", "--system", "--timeout", "2", "--dest", "org.freedesktop.Avahi", "--object-path", "/",
                     "--method", "org.freedesktop.Avahi.Server.GetHostName"});   // ('krakensdr-2',)
    if (s.size() > 5 && s.compare(0, 2, "('") == 0 && s.compare(s.size() - 3, 3, "',)") == 0)
        return s.substr(2, s.size() - 5);
    // "192.168.1.5	krakensdr-2.local" - an mDNS answer only
    const vector<string> ips = tlscert::private_ipv4();
    if (!ips.empty()) {
        std::istringstream ws(helper_line({"avahi-resolve", "-a", ips[0]}));
        string a, n;
        if (ws >> a >> n) {
            if (!n.empty() && n.back() == '.') n.pop_back();
            if (n.size() > 6 && n.compare(n.size() - 6, 6, ".local") == 0) return n.substr(0, n.size() - 6);
        }
    }
    return {};
}
#endif
#endif

#ifdef _WIN32
// fopen(path, "wb") for a file only this user, SYSTEM and the Administrators
// can open - the Windows form of mode 0600: a protected DACL, nothing
// inherited from the folder (whose ACL may let every local user read it). A
// file that was already there gets that DACL too (CreateFile keeps an
// existing file's). nullptr + errno on failure.
std::FILE* fopen_owner_only(const string& path) {
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
        !ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &sd, nullptr)) {
        errno = EACCES;
        return nullptr;
    }
    SECURITY_ATTRIBUTES sa{sizeof sa, sd, FALSE};
    // the path is UTF-8 like fopen's (win/app.manifest: activeCodePage UTF-8)
    HANDLE h = CreateFileA(path.c_str(), GENERIC_WRITE | WRITE_DAC, 0, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    DWORD e = h == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
    if (h != INVALID_HANDLE_VALUE && GetLastError() == ERROR_ALREADY_EXISTS) {
        PACL dacl = nullptr;
        BOOL present = FALSE, defaulted = FALSE;
        e = GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted)
                ? SetSecurityInfo(h, SE_FILE_OBJECT, DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION,
                                  nullptr, nullptr, dacl, nullptr)
                : GetLastError();
    }
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

// version 3, a random positive 127-bit serial, valid from an hour ago (a
// device clock a little behind) for exactly `days` days
bool cert_basics(X509* x, int days) {
    unsigned char sb[16];
    if (RAND_bytes(sb, sizeof sb) != 1) return false;
    sb[0] &= 0x7F;
    sb[0] |= 0x01;   // never a short (leading-zero) serial
    BIGNUM* bn = BN_bin2bn(sb, sizeof sb, nullptr);
    const bool ok = bn && BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x)) != nullptr;
    BN_free(bn);
    return ok && X509_set_version(x, 2) == 1 && X509_gmtime_adj(X509_getm_notBefore(x), -3600L) != nullptr &&
           X509_gmtime_adj(X509_getm_notAfter(x), static_cast<long>(days) * 86400L - 3600L) != nullptr;
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
X509Ptr make_ca(EVP_PKEY* key, const string& host, const vector<string>& names, bool wide) {
    X509Ptr x(X509_new());
    if (!x || !cert_basics(x.get(), tlscert::CA_DAYS) || X509_set_pubkey(x.get(), key) != 1) return nullptr;
    X509_NAME* n = X509_get_subject_name(x.get());
    if (!add_name(n, "CN", "KrakenSDR " + host + " CA " + today("%Y-%m-%d")) || !add_name(n, "O", tlscert::ORG) ||
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
        !add_ext(x.get(), x.get(), NID_name_constraints, nc) || X509_sign(x.get(), key, EVP_sha256()) <= 0)
        return nullptr;
    return x;
}

// The server certificate: CN=<host>, the names + IPs, signed by the CA
X509Ptr make_server(EVP_PKEY* key, X509* ca, EVP_PKEY* ca_key, const string& host, const vector<string>& names,
                    const vector<string>& ips) {
    X509Ptr x(X509_new());
    if (!x || !cert_basics(x.get(), tlscert::SERVER_DAYS) || X509_set_pubkey(x.get(), key) != 1) return nullptr;
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

}  // namespace

namespace tlscert {

Host host_name() {
    Host h;
    if (const char* e = std::getenv("KRAKEN_CERT_HOST"); e && *e) {
        h.raw = e;   // as given (make-cert.sh: not cut at a dot - a dot makes it unusable)
    } else {
#if defined(__APPLE__)
        h.raw = helper_line({"scutil", "--get", "LocalHostName"});
#elif !defined(_WIN32)
        h.raw = avahi_host();
#endif
        if (h.raw.empty()) {
            char b[256] = {};
            if (gethostname(b, sizeof b - 1) == 0) h.raw = b;
        }
        h.raw = h.raw.substr(0, h.raw.find('.'));   // hostname -s
    }
    const string n = lower(h.raw);
    h.usable = usable_host(n);
    if (h.usable) h.name = n;
    return h;
}

// make-cert.sh's list, in its order (compared as a set): an unusable host
// name still gets localhost and KRAKEN_CERT_NAMES
vector<string> wanted_names(const Host& h) {
    vector<string> v;
    auto add = [&v](const string& s) {
        if (std::find(v.begin(), v.end(), s) == v.end()) v.push_back(s);
    };
    auto one_dot_off = [](string s) {   // ${d%.}
        if (!s.empty() && s.back() == '.') s.pop_back();
        return s;
    };
    if (h.usable) {
        add(h.name);
        add(h.name + ".local");
        for (const string& raw : search_domains()) {
            const string d = one_dot_off(lower(raw));
            if (d != "local" && valid_dns(d)) add(h.name + "." + d);
        }
    }
    add("localhost");
    if (const char* e = std::getenv("KRAKEN_CERT_NAMES")) {
        // split on commas and white space (make-cert.sh: tr ',' ' ', then words)
        string list = e, w;
        std::replace(list.begin(), list.end(), ',', ' ');
        std::istringstream ws(list);
        while (ws >> w) {
            const string n = one_dot_off(lower(w));
            if (!valid_dns(n)) continue;   // not a DNS name
            if (std::all_of(n.begin(), n.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; }))
                continue;                    // an address, not a name
            add(n);
        }
    }
    return v;
}

vector<string> private_ipv4() {
    vector<string> v;
    for (const string& ip : global_ipv4()) {
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

// make-cert.sh's reading: the last line "scope = <letters>" (spaces
// allowed, any case) counts; anything but wide is narrow
bool read_scope_wide(const string& dir) {
    std::ifstream f(join(dir, OPTIONS));
    string line, value;
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.compare(0, 5, "scope") != 0) continue;
        string rest = trim(line.substr(5));
        if (rest.empty() || rest[0] != '=') continue;
        rest = trim(rest.substr(1));
        if (std::all_of(rest.begin(), rest.end(), [](char c) { return std::isalpha(static_cast<unsigned char>(c)) != 0; }))
            value = lower(rest);
    }
    return value == "wide";
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
    const vector<X509Ptr> chain = crt_there ? load_certs(crt_p) : vector<X509Ptr>();
    X509* leaf = chain.empty() ? nullptr : chain[0].get();
    if (crt_there && !leaf) {
        // not something to replace blindly (make-cert.sh fails the same way)
        *err = crt_p + " holds no certificate that can be read - move it away (a new one is made then) or replace it";
        return false;
    }
    const vector<X509Ptr> cas = file_nonempty(cac_p) ? load_certs(cac_p) : vector<X509Ptr>();
    X509* ca = cas.empty() ? nullptr : cas[0].get();
    // managed or not
    if (leaf && !(old_managed_subject(leaf) || verifies(leaf, ca, nullptr, false))) {
        *state = State::NotManaged;   // a certificate of the user's own: left alone
        if (key_there) return true;
        *err = string(SERVER_CRT) + " is not one of the receiver's own and " + SERVER_KEY + " is missing";
        return false;
    }
    // the server key: kept whatever its type; one that can't be read is replaced
    PkeyPtr skey = key_there ? load_key(key_p) : nullptr;
    PkeyPtr cakey = ca && file_nonempty(cak_p) ? load_key(cak_p) : nullptr;

    // what is wanted
    const bool wide = read_scope_wide(dir);
    const Host host = host_name();
    const vector<string> names = wanted_names(host);
    const vector<string> ips = wanted_ips(wide);
    const string cn = host.usable ? host.name : "localhost";

    // a new CA when: none (or no key that fits it, or not a CA); less than a
    // year left; another scope (no name constraints = a mismatch too); a
    // name outside its DNS constraints; asked to
    bool make_new_ca = new_ca || !ca || !cakey || !same_key(X509_get0_pubkey(ca), cakey.get()) || !is_ca_cert(ca) ||
                       days_left(ca) < CA_RENEW_DAYS || ca_scope(ca) != (wide ? "wide" : "narrow");
    if (!make_new_ca) {
        const Constraints c = constraints_of(ca);
        for (const string& n : names) make_new_ca = make_new_ca || !dns_permitted(c, n);
    }
    const bool make_key = !skey;
    // the server certificate is re-issued (same key) when: other names / IPs;
    // less than 60 days left; it doesn't verify against the current CA (a
    // migrated one); server.crt isn't exactly it + the CA; asked to
    bool issue = force || make_new_ca || make_key || !leaf;
    if (!issue) {
        vector<string> dns, sips, wips;
        san_of(leaf, &dns, &sips);
        for (const string& ip : ips) wips.push_back(ip_normal(ip));
        issue = !same_key(X509_get0_pubkey(leaf), skey.get()) || !same_set(dns, names) || !same_set(sips, wips) ||
                days_left(leaf) < SERVER_RENEW_DAYS || !verifies(leaf, ca, nullptr, false) || chain.size() != 2 ||
                X509_cmp(chain[1].get(), ca) != 0;
    }
    if (!issue) {
#ifndef _WIN32
        // keys left readable by others are tightened (make-cert.sh's fix_mode)
        for (const string& k : {key_p, cak_p}) {
            struct stat st;
            if (::lstat(k.c_str(), &st) == 0 && S_ISREG(st.st_mode) && (st.st_mode & 0777) != 0600) ::chmod(k.c_str(), 0600);
        }
#endif
        *state = State::Kept;
        return true;
    }

    // make everything in memory first
    PkeyPtr new_cakey, new_skey;
    X509Ptr new_ca_crt, new_leaf;
    if (make_new_ca) {
        new_cakey.reset(make_rsa_key(3072));
        if (new_cakey) new_ca_crt = make_ca(new_cakey.get(), cn, names, wide);
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
    new_leaf = make_server(use_skey, use_ca, use_cakey, cn, names, ips);
    if (!new_leaf) {
        *err = openssl_error("cannot create the server certificate");
        return false;
    }
    string why;
    if (!verifies(new_leaf.get(), use_ca, &why)) {   // what every browser will check
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
    if (make_new_ca) {
        // the old CA certificate is kept as .retired (the page names it:
        // devices should remove it); its key is replaced - gone
        // (only a readable certificate, not a symlink - make-cert.sh's rule;
        // the date in UTC, -2, -3 ... for more the same day)
        if (ca && !is_symlink(cac_p)) {
            const string stem = join(dir, "krakensdr-ca." + today("%Y%m%d"));
            string retired = stem + ".retired.crt";
            struct stat st;
            for (int i = 2; ::stat(retired.c_str(), &st) == 0 && i < 1000; i++)
                retired = stem + "-" + std::to_string(i) + ".retired.crt";
            if (!move(cac_p, retired)) return false;
        }
        if (!move(cak_p + ".tmp", cak_p) || !move(cac_p + ".tmp", cac_p)) return false;
    }
    if (make_key && !move(key_p + ".tmp", key_p)) return false;
    if (!move(crt_p + ".tmp", crt_p)) return false;
#ifndef _WIN32
    if (!make_key) ::chmod(key_p.c_str(), 0600);
#endif
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
    r.managed = !is_symlink(crt_p) && !is_symlink(join(dir, SERVER_KEY)) && (r.has_ca || old_managed_subject(leaf));
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(dir.empty() ? "." : dir, ec)) {
        const string name = e.path().filename().string();
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
