/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteString.h>
#include <AK/Enumerate.h>
#include <AK/Format.h>
#include <AK/ScopeGuard.h>
#include <LibPlatformFeatures/SystemFacts.h>
#include <RequestServer/AIA.h>
#include <RequestServer/CURL.h>
#include <RequestServer/CertificateExceptions.h>
#include <RequestServer/Resolver.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <string.h>

namespace RequestServer {

// The cipher suites that browsers offer, in their order of preference.
static constexpr Array s_tls13_cipher_suites = {
    "TLS_AES_128_GCM_SHA256"sv,
    "TLS_AES_256_GCM_SHA384"sv,
    "TLS_CHACHA20_POLY1305_SHA256"sv,
};

static constexpr Array s_tls12_cipher_suites = {
    "ECDHE-ECDSA-AES128-GCM-SHA256"sv,
    "ECDHE-RSA-AES128-GCM-SHA256"sv,
    "ECDHE-ECDSA-AES256-GCM-SHA384"sv,
    "ECDHE-RSA-AES256-GCM-SHA384"sv,
    "ECDHE-ECDSA-CHACHA20-POLY1305"sv,
    "ECDHE-RSA-CHACHA20-POLY1305"sv,
    // Servers that support none of the suites above need one of these.
    "ECDHE-RSA-AES128-SHA"sv,
    "ECDHE-RSA-AES256-SHA"sv,
    "AES128-GCM-SHA256"sv,
    "AES256-GCM-SHA384"sv,
    "AES128-SHA"sv,
    "AES256-SHA"sv,
};

struct CipherSuiteLists {
    ByteString tls13;
    ByteString tls12;
};

// The host's OpenSSL configuration can disable cipher suites, and we leave those out of what we offer.
static CipherSuiteLists const& enabled_cipher_suites()
{
    static CipherSuiteLists const lists = [] {
        auto* context = SSL_CTX_new(TLS_client_method());
        ScopeGuard free_context = [&] { SSL_CTX_free(context); };
        auto* enabled = context ? SSL_CTX_get_ciphers(context) : nullptr;

        Vector<StringView> enabled_names;
        for (int i = 0; i < sk_SSL_CIPHER_num(enabled); ++i) {
            auto const* name = SSL_CIPHER_get_name(sk_SSL_CIPHER_value(enabled, i));
            enabled_names.append({ name, strlen(name) });
        }

        auto join_enabled = [&](ReadonlySpan<StringView> cipher_suites) {
            Vector<StringView> names;
            for (auto cipher_suite : cipher_suites) {
                if (enabled_names.contains_slow(cipher_suite))
                    names.append(cipher_suite);
            }
            return ByteString::join(':', names);
        };
        return CipherSuiteLists { join_enabled(s_tls13_cipher_suites), join_enabled(s_tls12_cipher_suites) };
    }();
    return lists;
}

ErrorOr<void> initialize_libcurl()
{
    auto result = curl_global_init(CURL_GLOBAL_ALL);
    if (result != CURLE_OK) {
        warnln("curl_global_init failed: {}", curl_easy_strerror(result));
        return Error::from_string_literal("Failed to initialize libcurl");
    }

    // OpenSSL reads its configuration for this, which the sandbox may not allow later.
    (void)enabled_cipher_suites();
    return {};
}

#ifdef AK_OS_MACOS
// curl parses the system's root certificates for every connection on macOS, because it keeps its store cache off when
// verifying through the system trust APIs, and the parsed store costs about 1 MiB for as long as the connection lives.
// So all connections share one parsed store, replaced as often as curl would replace a store it caches.
static void share_certificate_store(SSL_CTX* ssl_context)
{
    // NB: A custom certificate is only trusted by the handles that set it, so those handles can't share a store.
    if (!default_certificate_path().is_empty())
        return;

    static constexpr auto s_shared_store_lifetime = AK::Duration::from_seconds(24 * 60 * 60);
    static X509_STORE* s_shared_store = nullptr;
    static MonotonicTime s_shared_store_expiry = MonotonicTime::now_coarse();

    auto now = MonotonicTime::now_coarse();
    if (!s_shared_store || now >= s_shared_store_expiry) {
        // NB: curl ignores errors when it loads the store. An empty store stays with this connection only, and the
        //     next connection tries again.
        auto* store = SSL_CTX_get_cert_store(ssl_context);
        if (!store || sk_X509_OBJECT_num(X509_STORE_get0_objects(store)) == 0 || !X509_STORE_up_ref(store))
            return;

        X509_STORE_free(s_shared_store);
        s_shared_store = store;
        s_shared_store_expiry = now + s_shared_store_lifetime;
        return;
    }

    // NB: The SSL_CTX takes over the reference, and frees the store curl just parsed for it.
    if (X509_STORE_up_ref(s_shared_store))
        SSL_CTX_set_cert_store(ssl_context, s_shared_store);
}
#endif

// curl matches the server's certificate against its Common Name when the certificate has no DNS or IP subject
// alternative name. Fail verification of such certificates, so that only subject alternative names identify a server.
static int verify_server_certificate_has_subject_alt_name(int preverify_ok, X509_STORE_CTX* context)
{
    if (!preverify_ok || X509_STORE_CTX_get_error_depth(context) != 0)
        return preverify_ok;

    auto* names = static_cast<GENERAL_NAMES*>(X509_get_ext_d2i(X509_STORE_CTX_get_current_cert(context), NID_subject_alt_name, nullptr, nullptr));
    auto has_host_name = false;
    for (int i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
        auto type = sk_GENERAL_NAME_value(names, i)->type;
        if (type == GEN_DNS || type == GEN_IPADD) {
            has_host_name = true;
            break;
        }
    }
    GENERAL_NAMES_free(names);

    if (has_host_name)
        return 1;
    X509_STORE_CTX_set_error(context, X509_V_ERR_HOSTNAME_MISMATCH);
    return 0;
}

static Optional<StringView> openssl_version_used_by_curl()
{
    auto const* version_info = curl_version_info(CURLVERSION_NOW);
    VERIFY(version_info);
    if (!version_info->ssl_version)
        return {};
    return PlatformFeatures::OpenSSLFacts::openssl_version_from_curl_ssl_version({ version_info->ssl_version, strlen(version_info->ssl_version) });
}

// Our SSL context callbacks require OpenSSL's SSL_CTX.
static bool curl_uses_openssl()
{
    // Those callbacks change curl's SSL_CTX through the OpenSSL we link, which only holds when curl uses that same
    // OpenSSL. A system curl can load another one, for instance once the system has moved on to a newer OpenSSL than
    // the one Ladybird was built against. Both are then loaded, and the structures of one handed to the other corrupt
    // memory, so leave curl's SSL_CTX alone in that case.
    static auto const uses_openssl = [] {
        auto curl_openssl_version = openssl_version_used_by_curl();
        if (!curl_openssl_version.has_value())
            return false;

        // The Browser tells the user about this as well, from what the system probe found.
        auto our_openssl_version = ByteString::formatted("{}.{}.{}", OPENSSL_version_major(), OPENSSL_version_minor(), OPENSSL_version_patch());
        if (*curl_openssl_version != our_openssl_version) {
            warnln("RequestServer: curl uses OpenSSL {}, but Ladybird uses OpenSSL {}. Ladybird has to be rebuilt against the OpenSSL that curl uses, until then it leaves curl's TLS configuration as it is.",
                *curl_openssl_version, our_openssl_version);
            return false;
        }
        return true;
    }();
    return uses_openssl;
}

CURLcode configure_tls_handshake(CURL* handle)
{
    // Other TLS backends read these options in a syntax of their own.
    static auto const tls_backend_is_openssl = openssl_version_used_by_curl().has_value();
    if (!tls_backend_is_openssl)
        return CURLE_OK;

    static auto const supports_https_proxies = (curl_version_info(CURLVERSION_NOW)->features & CURL_VERSION_HTTPS_PROXY) != 0;
    auto set_cipher_suites = [&](CURLoption option, CURLoption proxy_option, ByteString const& cipher_suites) {
        // The host's configuration enables none of our cipher suites, so its own list stays.
        if (cipher_suites.is_empty())
            return CURLE_OK;
        if (auto result = curl_easy_setopt(handle, option, cipher_suites.characters()); result != CURLE_OK)
            return result;
        if (!supports_https_proxies)
            return CURLE_OK;
        return curl_easy_setopt(handle, proxy_option, cipher_suites.characters());
    };

    auto const& cipher_suites = enabled_cipher_suites();
    if (auto result = set_cipher_suites(CURLOPT_TLS13_CIPHERS, CURLOPT_PROXY_TLS13_CIPHERS, cipher_suites.tls13); result != CURLE_OK)
        return result;
    return set_cipher_suites(CURLOPT_SSL_CIPHER_LIST, CURLOPT_PROXY_SSL_CIPHER_LIST, cipher_suites.tls12);
}

CURLcode configure_certificate_verification(CURL* handle, AIACollector& collector)
{
    if (auto result = configure_tls_handshake(handle); result != CURLE_OK)
        return result;

    // Exceptions may allow another host name, so our OpenSSL verifier must check names instead of curl.
    // Other backends keep curl's host name checks.
    collector.checks_host_name = curl_uses_openssl() && has_any_certificate_exception(collector.host, collector.port);
    if (auto result = curl_easy_setopt(handle, CURLOPT_SSL_CTX_FUNCTION, configure_ssl_context); result != CURLE_OK)
        return result;
    if (auto result = curl_easy_setopt(handle, CURLOPT_SSL_CTX_DATA, &collector); result != CURLE_OK)
        return result;
    return curl_easy_setopt(handle, CURLOPT_SSL_VERIFYHOST, collector.checks_host_name ? 0L : 2L);
}

CURLcode configure_ssl_context(CURL*, [[maybe_unused]] void* ssl_context, [[maybe_unused]] void* aia_collector)
{
    [[maybe_unused]] auto const uses_openssl = curl_uses_openssl();

#if defined(SSL_OP_IGNORE_UNEXPECTED_EOF)
    // Some HTTPS servers, including the Python server used by WPT, close the connection without sending the
    // mandatory TLS close-notify alert. Other browsers treat such a post-handshake transport EOF as a clean TLS EOF
    // for compatibility. Do the same while leaving curl responsible for detecting incomplete HTTP framing.
    if (uses_openssl)
        SSL_CTX_set_options(static_cast<SSL_CTX*>(ssl_context), SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

    // Keep curl's verify mode: it sets SSL_VERIFY_NONE and checks the verification result once the handshake is done.
    if (uses_openssl) {
        auto* context = static_cast<SSL_CTX*>(ssl_context);
        SSL_CTX_set_verify(context, SSL_CTX_get_verify_mode(context), verify_server_certificate_has_subject_alt_name);
    }

    // Offer only modern key exchange groups, with key shares for the two X25519 based groups. Some CDNs reject
    // requests that have a browser User-Agent and the TLS fingerprint of a default OpenSSL client. OpenSSL older than
    // 3.5 does not understand this syntax. It rejects the list and keeps its default groups.
    if (uses_openssl) {
        char groups[] = "*X25519MLKEM768:*X25519:P-256:P-384";
        if (SSL_CTX_set1_groups_list(static_cast<SSL_CTX*>(ssl_context), groups) != 1)
            ERR_clear_error();
    }

#ifdef AK_OS_MACOS
    if (uses_openssl)
        share_certificate_store(static_cast<SSL_CTX*>(ssl_context));
#endif
    // curl keeps one SSL context callback per handle, and AIA has to run against the same SSL_CTX configured above. So
    // both are applied here. The collector arrives as CURLOPT_SSL_CTX_DATA and is null on handles lacking AIA state.
    // NB: On macOS, curl uses SecTrust only if OpenSSL has not already verified the certificate.
    if (uses_openssl && aia_collector)
        apply_aia_verification(static_cast<SSL_CTX*>(ssl_context), *static_cast<AIACollector*>(aia_collector));

    return CURLE_OK;
}

bool can_pin_host_in_curl_resolve_list(StringView host)
{
    return !host.is_empty() && host[0] != '-' && host[0] != '+';
}

ByteString build_curl_resolve_list(DNS::LookupResult const& dns_result, StringView host, u16 port)
{
    StringBuilder resolve_opt_builder;
    resolve_opt_builder.appendff("{}:{}:", host, port);

    for (auto const& [i, addr] : enumerate(dns_result.cached_addresses())) {
        auto formatted_address = addr.visit(
            [&](IPv4Address const& ipv4) { return ipv4.to_byte_string(); },
            [&](IPv6Address const& ipv6) { return MUST(ipv6.to_string()).to_byte_string(); });

        if (i > 0)
            resolve_opt_builder.append(',');
        resolve_opt_builder.append(formatted_address);
    }

    dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Resolve list: {}", resolve_opt_builder.string_view());
    return resolve_opt_builder.to_byte_string();
}

Optional<ByteString> build_curl_connect_to_entry(DNS::LookupResult const& dns_result, StringView host, u16 port, u32 address_index)
{
    auto addresses = dns_result.cached_addresses();
    if (addresses.size() < 2)
        return {};

    auto formatted_address = addresses[address_index % addresses.size()].visit(
        [&](IPv4Address const& ipv4) { return ipv4.to_byte_string(); },
        // CURLOPT_CONNECT_TO, unlike CURLOPT_RESOLVE, parses its host field, so an IPv6 address has to be bracketed
        // for its colons not to read as a port separator.
        [&](IPv6Address const& ipv6) { return ByteString::formatted("[{}]", MUST(ipv6.to_string())); });

    auto entry = ByteString::formatted("{}:{}:{}:{}", host, port, formatted_address, port);
    dbgln_if(REQUESTSERVER_DEBUG, "RequestServer: Connect-to entry: {}", entry);

    return entry;
}

Requests::NetworkError curl_code_to_network_error(int code)
{
    switch (code) {
    case CURLE_COULDNT_RESOLVE_HOST:
        return Requests::NetworkError::UnableToResolveHost;
    case CURLE_COULDNT_RESOLVE_PROXY:
        return Requests::NetworkError::UnableToResolveProxy;
    case CURLE_COULDNT_CONNECT:
        return Requests::NetworkError::UnableToConnect;
    case CURLE_OPERATION_TIMEDOUT:
        return Requests::NetworkError::TimeoutReached;
    case CURLE_TOO_MANY_REDIRECTS:
        return Requests::NetworkError::TooManyRedirects;
    case CURLE_SSL_CONNECT_ERROR:
        return Requests::NetworkError::SSLHandshakeFailed;
    case CURLE_PEER_FAILED_VERIFICATION:
        return Requests::NetworkError::SSLVerificationFailed;
    case CURLE_URL_MALFORMAT:
        return Requests::NetworkError::MalformedUrl;
    case CURLE_PARTIAL_FILE:
        return Requests::NetworkError::IncompleteContent;
    case CURLE_BAD_CONTENT_ENCODING:
        return Requests::NetworkError::InvalidContentEncoding;
    default:
        return Requests::NetworkError::Unknown;
    }
}

}
