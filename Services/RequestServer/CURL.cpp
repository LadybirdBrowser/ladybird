/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/Format.h>
#include <RequestServer/AIA.h>
#include <RequestServer/CURL.h>
#include <RequestServer/Resolver.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <string.h>

namespace RequestServer {

ErrorOr<void> initialize_libcurl()
{
    auto result = curl_global_init(CURL_GLOBAL_ALL);
    if (result != CURLE_OK) {
        warnln("curl_global_init failed: {}", curl_easy_strerror(result));
        return Error::from_string_literal("Failed to initialize libcurl");
    }
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

CURLcode configure_ssl_context(CURL*, [[maybe_unused]] void* ssl_context, [[maybe_unused]] void* aia_collector)
{
    // Everything below casts ssl_context to an SSL_CTX, which only holds when curl is built against OpenSSL.
    [[maybe_unused]] static auto const curl_uses_openssl = [] {
        auto const* version_info = curl_version_info(CURLVERSION_NOW);
        VERIFY(version_info);
        return version_info->ssl_version && StringView { version_info->ssl_version, strlen(version_info->ssl_version) }.starts_with("OpenSSL/"sv);
    }();

#if defined(SSL_OP_IGNORE_UNEXPECTED_EOF)
    // Some HTTPS servers, including the Python server used by WPT, close the connection without sending the
    // mandatory TLS close-notify alert. Other browsers treat such a post-handshake transport EOF as a clean TLS EOF
    // for compatibility. Do the same while leaving curl responsible for detecting incomplete HTTP framing.
    if (curl_uses_openssl)
        SSL_CTX_set_options(static_cast<SSL_CTX*>(ssl_context), SSL_OP_IGNORE_UNEXPECTED_EOF);
#endif

    // Keep curl's verify mode: it sets SSL_VERIFY_NONE and checks the verification result once the handshake is done.
    if (curl_uses_openssl) {
        auto* context = static_cast<SSL_CTX*>(ssl_context);
        SSL_CTX_set_verify(context, SSL_CTX_get_verify_mode(context), verify_server_certificate_has_subject_alt_name);
    }

    // Offer only modern key exchange groups, with key shares for the two X25519 based groups. Some CDNs reject
    // requests that have a browser User-Agent and the TLS fingerprint of a default OpenSSL client. OpenSSL older than
    // 3.5 does not understand this syntax. It rejects the list and keeps its default groups.
    if (curl_uses_openssl) {
        char groups[] = "*X25519MLKEM768:*X25519:P-256:P-384";
        if (SSL_CTX_set1_groups_list(static_cast<SSL_CTX*>(ssl_context), groups) != 1)
            ERR_clear_error();
    }

#ifdef AK_OS_MACOS
    if (curl_uses_openssl)
        share_certificate_store(static_cast<SSL_CTX*>(ssl_context));
#else
    // curl keeps one SSL context callback per handle, and AIA has to run against the same SSL_CTX configured above. So
    // both are applied here. The collector arrives as CURLOPT_SSL_CTX_DATA and is null on handles lacking AIA state.
    if (curl_uses_openssl && aia_collector)
        apply_aia_verification(static_cast<SSL_CTX*>(ssl_context), *static_cast<AIACollector*>(aia_collector));
#endif

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
