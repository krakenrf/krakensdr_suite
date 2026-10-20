// See tls_compat.hpp.
#ifdef _WIN32
// Before any OpenSSL header: wincrypt.h defines X509_NAME, OCSP_REQUEST ...
// as macros, which OpenSSL's headers #undef - only for a wincrypt.h included
// earlier.
#include <windows.h>
#include <wincrypt.h>
#include <aclapi.h>
#include <sddl.h>
#endif

#include "utils/tls_compat.hpp"
#include "utils/fs_compat.hpp"

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

#include <cerrno>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>

#include <cwchar>
#include <mutex>
#include <vector>
#else
#include <fcntl.h>
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

// --- the web UI's self-signed certificate --------------------------------------------

namespace {

bool file_nonempty(const char* path) {
    struct stat st;
    return ::stat(path, &st) == 0 && st.st_size > 0;
}

std::string openssl_error(const char* what) {
    std::string s = what;
    if (const unsigned long e = ERR_get_error()) {
        char b[256];
        ERR_error_string_n(e, b, sizeof b);
        s += std::string(": ") + b;
    }
    ERR_clear_error();
    return s;
}

EVP_PKEY* make_rsa_key(int bits) {
    EVP_PKEY* key = nullptr;
    EVP_PKEY_CTX* kc = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    if (kc && EVP_PKEY_keygen_init(kc) > 0 && EVP_PKEY_CTX_set_rsa_keygen_bits(kc, bits) > 0)
        EVP_PKEY_keygen(kc, &key);
    EVP_PKEY_CTX_free(kc);
    return key;
}

bool add_ext(X509* x, int nid, const char* value) {
    X509V3_CTX v3{};
    X509V3_set_ctx_nodb(&v3);
    X509V3_set_ctx(&v3, x, x, nullptr, nullptr, 0);
    X509_EXTENSION* ext = X509V3_EXT_conf_nid(nullptr, &v3, nid, value);
    if (!ext) return false;
    const bool ok = X509_add_ext(x, ext, -1) == 1;
    X509_EXTENSION_free(ext);
    return ok;
}

// the PEM text written by `write` into a memory BIO
template <typename F>
bool pem_text(F write, std::string* out) {
    BIO* b = BIO_new(BIO_s_mem());
    if (!b) return false;
    bool ok = write(b) == 1;
    char* p = nullptr;
    const long n = BIO_get_mem_data(b, &p);
    if (ok && n > 0) out->assign(p, static_cast<size_t>(n));
    BIO_free(b);
    return ok && n > 0;
}

#ifdef _WIN32
// fopen(path, "wb") for a file only this user, SYSTEM and the Administrators
// can open - the Windows form of mode 0600: a protected DACL, nothing
// inherited from the folder (whose ACL may let every local user read it). A
// file that was already there gets that DACL too (CreateFile keeps an
// existing file's). nullptr + errno on failure.
std::FILE* fopen_owner_only(const std::string& path) {
    std::wstring sddl;
    HANDLE tok = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        DWORD n = 0;
        GetTokenInformation(tok, TokenUser, nullptr, 0, &n);
        std::vector<unsigned char> tu(n);
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
// this user, SYSTEM and the Administrators only)
bool write_file(const std::string& path, const std::string& data, bool owner_only, std::string* err) {
#ifdef _WIN32
    std::FILE* f = owner_only ? fopen_owner_only(path) : std::fopen(path.c_str(), "wb");
#else
    const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, owner_only ? 0600 : 0644);
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

}  // namespace

bool tls_ensure_self_signed_cert(const char* cert_path, const char* key_path, bool* created, std::string* err) {
    if (created) *created = false;
    if (file_nonempty(cert_path) && file_nonempty(key_path)) return true;   // never replace a pair

    EVP_PKEY* key = make_rsa_key(2048);
    X509* x = X509_new();
    std::string key_pem, cert_pem;
    bool ok = key && x;
    if (!ok) *err = openssl_error("cannot create an RSA key");
    if (ok) {
        // random positive 127-bit serial, valid from an hour ago (a clock a
        // little behind) for 10 years
        unsigned char sb[16];
        BIGNUM* bn = nullptr;
        ok = RAND_bytes(sb, sizeof sb) == 1;
        if (ok) {
            sb[0] &= 0x7F;
            bn = BN_bin2bn(sb, sizeof sb, nullptr);
        }
        ok = ok && bn && BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x)) != nullptr;
        BN_free(bn);
        ok = ok && X509_set_version(x, 2) == 1 &&   // v3
             X509_gmtime_adj(X509_getm_notBefore(x), -3600L) != nullptr &&
             X509_gmtime_adj(X509_getm_notAfter(x), 3650L * 24 * 3600) != nullptr && X509_set_pubkey(x, key) == 1;
        X509_NAME* name = X509_get_subject_name(x);
        ok = ok && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                                              reinterpret_cast<const unsigned char*>("krakensdr"), -1, -1, 0) == 1 &&
             X509_set_issuer_name(x, name) == 1;
        // a server (end-entity) certificate: browsers want the names in the
        // SAN; Firefox refuses a CA certificate as a server's one
        ok = ok && add_ext(x, NID_basic_constraints, "critical,CA:FALSE") &&
             add_ext(x, NID_key_usage, "critical,digitalSignature,keyEncipherment") &&
             add_ext(x, NID_ext_key_usage, "serverAuth") && add_ext(x, NID_subject_key_identifier, "hash") &&
             add_ext(x, NID_subject_alt_name, "DNS:krakensdr,DNS:localhost,IP:127.0.0.1,IP:::1");
        ok = ok && X509_sign(x, key, EVP_sha256()) > 0;
        ok = ok && pem_text([&](BIO* b) { return PEM_write_bio_PrivateKey(b, key, nullptr, nullptr, 0, nullptr, nullptr); },
                            &key_pem) &&
             pem_text([&](BIO* b) { return PEM_write_bio_X509(b, x); }, &cert_pem);
        if (!ok) *err = openssl_error("cannot create the certificate");
    }
    X509_free(x);
    EVP_PKEY_free(key);
    if (!ok) return false;

    // The certificate's presence marks a complete pair: the old one (left
    // without its key) goes first and the new one comes last, so an
    // interrupted run leaves no certificate - and the next start makes a
    // pair again - rather than a certificate that doesn't match its key.
    const std::string kt = std::string(key_path) + ".tmp", ct = std::string(cert_path) + ".tmp";
    if (!write_file(kt, key_pem, true, err)) return false;
    if (!write_file(ct, cert_pem, false, err)) {
        std::remove(kt.c_str());
        return false;
    }
    std::remove(cert_path);
    if (fs_replace(kt, key_path) != 0 || fs_replace(ct, cert_path) != 0) {
        *err = std::string("cannot replace ") + cert_path + " / " + key_path + " (" + std::strerror(errno) + ")";
        std::remove(kt.c_str());
        std::remove(ct.c_str());
        return false;
    }
    if (created) *created = true;
    return true;
}
