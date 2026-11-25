/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCrypto/OpenSSL.h>
#include <RequestServer/AIA.h>
#ifdef AK_OS_MACOS
#    include <RequestServer/SystemTrustMacOS.h>
#endif
#include <RequestServer/CertificateExceptions.h>

#include <AK/AnyOf.h>
#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/Time.h>
#include <AK/Vector.h>

#if defined(AK_OS_WINDOWS)
// Included before the OpenSSL headers so the winsock2 struct timeval is renamed and can't collide with AK's.
// Same reason CURL.h includes it before curl.h.
#    include <AK/Windows.h>
#endif

#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

namespace RequestServer {

// This max was chosen to align with the (telemetry-tuned) value Chromium uses.
static constexpr size_t max_ca_issuers_urls_per_certificate = 5;

// On macOS, curl leaves OpenSSL's trust store empty, so OpenSSL cannot identify missing issuers or untrusted roots.
// SecTrust fetches missing intermediates and verifies after the handshake unless OpenSSL already verified.
#ifdef AK_OS_MACOS
static constexpr bool openssl_trust_store_is_authoritative = false;
#else
static constexpr bool openssl_trust_store_is_authoritative = true;
#endif

// Upper bound on the process-wide fetched-intermediate cache — so a long-lived process that visits many misconfigured
// sites can't grow it without limit.
static constexpr size_t max_cached_intermediates = 256;

// How long a caIssuers URL whose fetch failed is remembered, before we're willing to try it again.
static constexpr auto failed_url_retry_interval = AK::Duration::from_seconds(300);

// Upper bound on the failed-URL negative cache, so a page hitting many distinct dead caIssuers URLs can't grow it
// without limit.
static constexpr size_t max_failed_urls = 1024;

// Collect the caIssuers URLs from a cert's AIA extension. Only http URLs are returned; fetching an intermediate over
// https would itself require cert verification — risking infinite recursion.
Vector<ByteString> ca_issuers_urls(X509* certificate)
{
    Vector<ByteString> urls;

    auto* authority_info_access = static_cast<AUTHORITY_INFO_ACCESS*>(X509_get_ext_d2i(certificate, NID_info_access, nullptr, nullptr));
    if (!authority_info_access)
        return urls;

    for (int i = 0; i < sk_ACCESS_DESCRIPTION_num(authority_info_access); ++i) {
        auto* access_description = sk_ACCESS_DESCRIPTION_value(authority_info_access, i);
        if (OBJ_obj2nid(access_description->method) != NID_ad_ca_issuers)
            continue;
        if (access_description->location->type != GEN_URI)
            continue;

        auto const* uri = access_description->location->d.uniformResourceIdentifier;
        auto url = ByteString { reinterpret_cast<char const*>(ASN1_STRING_get0_data(uri)), static_cast<size_t>(ASN1_STRING_length(uri)) };
        if (url.starts_with("http://"sv, CaseSensitivity::CaseInsensitive) && urls.size() < max_ca_issuers_urls_per_certificate)
            urls.append(move(url));
    }

    AUTHORITY_INFO_ACCESS_free(authority_info_access);
    return urls;
}

// Separate normal and private caches to prevent browsing-history leaks. Each entry owns one X509 reference.
static Vector<Crypto::OpenSSL_X509>& intermediate_cache(IsPrivate is_private)
{
    static Vector<Crypto::OpenSSL_X509> caches[2];
    return caches[to_underlying(is_private)];
}

// Cache failed caIssuers fetches to avoid repeated retries, separately for normal and private clients.
static HashMap<ByteString, MonotonicTime>& failed_urls(IsPrivate is_private)
{
    static HashMap<ByteString, MonotonicTime> urls[2];
    return urls[to_underlying(is_private)];
}

void mark_aia_url_failed(IsPrivate is_private, ByteString url)
{
    auto& urls = failed_urls(is_private);
    if (urls.size() >= max_failed_urls)
        urls.clear();
    urls.set(move(url), MonotonicTime::now_coarse());
}

void clear_aia_state(IsPrivate is_private)
{
    intermediate_cache(is_private).clear();
    failed_urls(is_private).clear();
}

// Parse a fetched AIA response body into one or more certs. Real-world CAs serve a bare DER-encoded cert, a PKCS#7/
// CMS "certs-only" bundle (RFC 5280 section 4.2.2.1), or PEM.
Vector<X509*> parse_certificates(ReadonlyBytes body)
{
    Vector<X509*> certificates;

    auto const* der = body.data();
    if (auto* certificate = d2i_X509(nullptr, &der, static_cast<long>(body.size()))) {
        certificates.append(certificate);
        return certificates;
    }
    // Each decoder below is tried in turn — so a failure here is expected rather than exceptional. Drop what it
    // queued — so it can't later be mistaken for the error of an unrelated OpenSSL call on this thread.
    ERR_clear_error();

    auto const* pkcs7_data = body.data();
    if (auto* pkcs7 = d2i_PKCS7(nullptr, &pkcs7_data, static_cast<long>(body.size()))) {
        STACK_OF(X509)* certs = nullptr;
        if (PKCS7_type_is_signed(pkcs7)) {
            if (pkcs7->d.sign)
                certs = pkcs7->d.sign->cert;
        } else if (PKCS7_type_is_signedAndEnveloped(pkcs7)) {
            if (pkcs7->d.signed_and_enveloped)
                certs = pkcs7->d.signed_and_enveloped->cert;
        }
        for (int i = 0; certs && i < sk_X509_num(certs); ++i) {
            auto* certificate = sk_X509_value(certs, i);
            X509_up_ref(certificate);
            certificates.append(certificate);
        }
        PKCS7_free(pkcs7);
        if (!certificates.is_empty())
            return certificates;
    }
    ERR_clear_error();

    if (auto* bio = BIO_new_mem_buf(body.data(), static_cast<int>(body.size()))) {
        while (auto* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr))
            certificates.append(certificate);
        BIO_free(bio);
    }
    // The read loop above always ends on a failed read, queueing an error even when certificates were parsed.
    ERR_clear_error();

    return certificates;
}

static void add_certificate(Vector<Crypto::OpenSSL_X509>& certificates, X509* certificate)
{
    X509_up_ref(certificate);
    auto owned = Crypto::OpenSSL_X509::wrap(certificate);
    if (owned.is_error()) {
        X509_free(certificate);
        return;
    }
    certificates.append(owned.release_value());
}

bool add_fetched_aia_intermediates(ReadonlyBytes body, ReadonlySpan<NonnullRefPtr<AIACollector>> collectors)
{
    auto certificates = parse_certificates(body);
    for (auto* certificate : certificates) {
        for (auto const& collector : collectors)
            add_certificate(collector->fetched_intermediates, certificate);
        X509_free(certificate);
    }
    return !certificates.is_empty();
}

static bool contains_certificate(Vector<Crypto::OpenSSL_X509> const& certificates, X509 const* certificate)
{
    return any_of(certificates, [&](auto const& candidate) { return X509_cmp(candidate.ptr(), certificate) == 0; });
}

static void promote_fetched_intermediates(X509_STORE_CTX* context, AIACollector& collector)
{
    auto* chain = X509_STORE_CTX_get0_chain(context);
    auto& cache = intermediate_cache(collector.is_private);

    for (int i = 0; chain && i < sk_X509_num(chain); ++i) {
        auto* certificate = sk_X509_value(chain, i);
        if (!contains_certificate(collector.fetched_intermediates, certificate) || contains_certificate(cache, certificate))
            continue;

        // take_first() drops the oldest wrapper, whose destructor frees the certificate.
        if (cache.size() >= max_cached_intermediates)
            (void)cache.take_first();
        add_certificate(cache, certificate);
    }
}

static bool certificate_matches_host(X509* certificate, ByteString const& host)
{
    auto address = host.view();
    if (address.starts_with('[') && address.ends_with(']'))
        address = address.substring_view(1, address.length() - 2);

    // Like curl, match an IP literal only against iPAddress names, and refuse partial wildcards such as w*.example.com.
    // X509_check_ip_asc() returns -2 when the input is not an IP address.
    auto address_string = ByteString { address };
    if (auto result = X509_check_ip_asc(certificate, address_string.characters(), 0); result != -2)
        return result == 1;

    // Like curl, match a fully qualified name such as example.com. without its trailing dot.
    auto name = host.view();
    if (name.ends_with('.'))
        name = name.substring_view(0, name.length() - 1);
    return X509_check_host(certificate, name.characters_without_null_termination(), name.length(), X509_CHECK_FLAG_NEVER_CHECK_SUBJECT | X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS, nullptr) == 1;
}

struct VerificationFailure {
    Requests::CertificateError errors { Requests::CertificateError::None };
    // Descriptions of errors without a dedicated CertificateError flag.
    ByteString details;
    // macOS decides issuer trust; curl doesn't report its reasons for rejection.
    bool is_decided_by_system_verifier { false };

    void add(int verify_error)
    {
        switch (verify_error) {
        case X509_V_OK:
            return;
        case X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT:
            errors |= Requests::CertificateError::SelfSigned;
            return;
        case X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT:
        case X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY:
        case X509_V_ERR_UNABLE_TO_VERIFY_LEAF_SIGNATURE:
        case X509_V_ERR_CERT_UNTRUSTED:
            if (!openssl_trust_store_is_authoritative)
                is_decided_by_system_verifier = true;
            else
                errors |= verify_error == X509_V_ERR_SELF_SIGNED_CERT_IN_CHAIN ? Requests::CertificateError::SelfSigned : Requests::CertificateError::UnknownIssuer;
            return;
        case X509_V_ERR_CERT_HAS_EXPIRED:
            errors |= Requests::CertificateError::Expired;
            return;
        case X509_V_ERR_CERT_NOT_YET_VALID:
            errors |= Requests::CertificateError::NotYetValid;
            return;
        case X509_V_ERR_HOSTNAME_MISMATCH:
            errors |= Requests::CertificateError::NameMismatch;
            return;
        default:
            break;
        }

        add_unnamed_error(X509_verify_cert_error_string(verify_error));
    }

    void add_unnamed_error(ByteString description)
    {
        errors |= Requests::CertificateError::Other;
        if (description.is_empty() || details.view().split_view("; "sv).contains_slow(description.view()))
            return;
        details = details.is_empty() ? move(description) : ByteString::formatted("{}; {}", details, description);
    }

#ifdef AK_OS_MACOS
    void add_system_verdict(X509* leaf, ReadonlySpan<X509*> intermediates)
    {
        auto verdict = system_trust_verdict(leaf, intermediates);
        errors |= verdict.errors & ~Requests::CertificateError::Other;
        if (has_flag(verdict.errors, Requests::CertificateError::Other))
            add_unnamed_error(move(verdict.details));
        is_decided_by_system_verifier = false;
    }
#endif
};

static int record_verification_error(int, X509_STORE_CTX* context)
{
    static_cast<VerificationFailure*>(X509_STORE_CTX_get_app_data(context))->add(X509_STORE_CTX_get_error(context));
    // Continue verification to collect later errors.
    return 1;
}

// Verify past OpenSSL's first error so the user can review all failures, including checks an untrusted chain
// would skip, such as certificate purpose.
static VerificationFailure verification_failure(X509_STORE_CTX* context, int result, X509* leaf, ByteString const& host)
{
    VerificationFailure failure;

    if (result <= 0) {
        auto* full_verification = X509_STORE_CTX_new();
        if (full_verification && X509_STORE_CTX_init(full_verification, X509_STORE_CTX_get0_store(context), leaf, X509_STORE_CTX_get0_untrusted(context)) == 1
            && X509_VERIFY_PARAM_set1(X509_STORE_CTX_get0_param(full_verification), X509_STORE_CTX_get0_param(context)) == 1) {
            X509_STORE_CTX_set_verify_cb(full_verification, record_verification_error);
            X509_STORE_CTX_set_app_data(full_verification, &failure);
            (void)X509_verify_cert(full_verification);
        }
        X509_STORE_CTX_free(full_verification);
        ERR_clear_error();

        // Preserve the original failure if the second pass recorded none.
        if (failure.errors == Requests::CertificateError::None && !failure.is_decided_by_system_verifier)
            failure.add(X509_STORE_CTX_get_error(context) == X509_V_OK ? X509_V_ERR_UNSPECIFIED : X509_STORE_CTX_get_error(context));
    }

    // curl only checks the name once the handshake is done, and doesn't say that it was the name that failed.
    if (!host.is_empty() && !certificate_matches_host(leaf, host))
        failure.errors |= Requests::CertificateError::NameMismatch;

    return failure;
}

#ifdef AK_OS_MACOS
static Vector<X509*> intermediates_of(X509_STORE_CTX* context)
{
    Vector<X509*> intermediates;
    auto* untrusted = X509_STORE_CTX_get0_untrusted(context);
    for (int i = 0; untrusted && i < sk_X509_num(untrusted); ++i)
        intermediates.append(sk_X509_value(untrusted, i));
    return intermediates;
}
#endif

// Return whether an exception covers all failures; otherwise record the rejected certificate and errors for review.
static bool apply_certificate_exception(X509_STORE_CTX* context, int result, AIACollector& collector)
{
    collector.rejected_certificate.clear();
    collector.chain_for_system_verifier.clear();

    auto* leaf = X509_STORE_CTX_get0_cert(context);
    if (!leaf)
        return false;

    auto failure = verification_failure(context, result, leaf, collector.host);
    if (result > 0 && failure.errors == Requests::CertificateError::None)
        return false;

    auto fingerprint = certificate_fingerprint(leaf);
    if (fingerprint.is_empty())
        return false;

    if (failure.is_decided_by_system_verifier) {
#ifdef AK_OS_MACOS
        // An exception bypasses curl's system verification, so ask here first. Otherwise, curl asks after the
        // handshake and we collect the reasons only if it fails.
        if (collector.checks_host_name) {
            failure.add_system_verdict(leaf, intermediates_of(context));
            if (failure.errors == Requests::CertificateError::None)
                return false;
        } else {
            AIACollector::ChainForSystemVerifier chain { { move(fingerprint), failure.errors, move(failure.details) }, {} };
            add_certificate(chain.certificates, leaf);
            for (auto* intermediate : intermediates_of(context))
                add_certificate(chain.certificates, intermediate);
            collector.chain_for_system_verifier = move(chain);
            return false;
        }
#else
        return false;
#endif
    }

    // NB: curl still checks the name itself unless an exception existed when the connection was set up.
    auto may_have_another_name = collector.checks_host_name || !has_flag(failure.errors, Requests::CertificateError::NameMismatch);
    if (may_have_another_name && has_certificate_exception(collector.host, collector.port, fingerprint, failure.errors, failure.details)) {
        // curl checks SSL_get_verify_result() after the handshake, which reports the error left on the context.
        X509_STORE_CTX_set_error(context, X509_V_OK);
        return true;
    }

    collector.rejected_certificate = AIACollector::RejectedCertificate { move(fingerprint), failure.errors, move(failure.details) };
    return false;
}

void explain_system_verifier_refusal([[maybe_unused]] AIACollector& collector)
{
#ifdef AK_OS_MACOS
    auto chain = exchange(collector.chain_for_system_verifier, {});
    if (!chain.has_value() || chain->certificates.is_empty())
        return;

    VerificationFailure failure { chain->rejection.errors, move(chain->rejection.details), true };
    Vector<X509*> intermediates;
    for (size_t i = 1; i < chain->certificates.size(); ++i)
        intermediates.append(chain->certificates[i].ptr());
    failure.add_system_verdict(chain->certificates.first().ptr(), intermediates);
    if (failure.errors == Requests::CertificateError::None)
        return;

    collector.rejected_certificate = AIACollector::RejectedCertificate { move(chain->rejection.fingerprint), failure.errors, move(failure.details) };
#endif
}

// Installed via SSL_CTX_set_cert_verify_callback. On failure, we record the failing cert's caIssuers URLs for the
// request layer to fetch and retry.
static int verify_callback(X509_STORE_CTX* context, void* collector_data)
{
    auto* collector = static_cast<AIACollector*>(collector_data);
    auto is_private = collector ? collector->is_private : IsPrivate::No;

    // Add the fetched intermediates alongside the certs the server sent, as untrusted path-building material.
    auto* server_untrusted = X509_STORE_CTX_get0_untrusted(context);
    auto* untrusted = server_untrusted ? sk_X509_dup(server_untrusted) : sk_X509_new_null();
    if (untrusted) {
        for (auto& candidate : intermediate_cache(is_private))
            sk_X509_push(untrusted, candidate.ptr());
        if (collector) {
            for (auto& candidate : collector->fetched_intermediates)
                sk_X509_push(untrusted, candidate.ptr());
        }
        X509_STORE_CTX_set0_untrusted(context, untrusted);
    }

    auto result = X509_verify_cert(context);

    // Reverification needs the fetched intermediates before they are removed from the context.
    auto accepted_by_exception = collector && apply_certificate_exception(context, result, *collector);

    if (untrusted) {
        X509_STORE_CTX_set0_untrusted(context, server_untrusted);
        sk_X509_free(untrusted);
    }
    if (accepted_by_exception)
        result = 1;

    // Only the certificate the user trusted may name another host. Fail the handshake for any other, as on macOS curl
    // would otherwise go on to verify the chain with SecTrust, which then doesn't check the name either.
    if (collector && collector->checks_host_name && !accepted_by_exception) {
        auto* leaf = X509_STORE_CTX_get0_cert(context);
        if (!leaf || !certificate_matches_host(leaf, collector->host)) {
            if (result > 0)
                X509_STORE_CTX_set_error(context, X509_V_ERR_HOSTNAME_MISMATCH);
            return 0;
        }
    }

    if (result > 0 && collector && !collector->fetched_intermediates.is_empty())
        promote_fetched_intermediates(context, *collector);

    // AIA can only repair a chain missing an intermediate issuer. For any other verification failure, fetching
    // caIssuers is useless — so we leave the collector's pending URLs untouched.
    auto const verify_error = X509_STORE_CTX_get_error(context);
    auto const issuer_is_missing = verify_error == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT || verify_error == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY;

    if (result <= 0 && collector && issuer_is_missing && openssl_trust_store_is_authoritative) {
        if (auto* current = X509_STORE_CTX_get_current_cert(context)) {
            for (auto& url : ca_issuers_urls(current)) {
                if (collector->attempted_urls.contains(url))
                    continue;
                if (auto failed_at = failed_urls(is_private).get(url); failed_at.has_value()) {
                    if (MonotonicTime::now_coarse() - *failed_at < failed_url_retry_interval)
                        continue;
                    failed_urls(is_private).remove(url);
                }
                if (!collector->pending_urls.contains_slow(url))
                    collector->pending_urls.append(move(url));
            }
        }
    }

    // X509_verify_cert() returns -1 in exceptional cases, and a negative return from a cert_verify_callback tells
    // OpenSSL to suspend the handshake and retry it later. Collapse it to a plain pass/fail.
    // NB: With SSL_VERIFY_PEER (see apply_aia_verification()), let the handshake continue for these failures;
    //     curl reads the verification error from the context.
    if (collector && collector->checks_host_name)
        return 1;
    return result > 0 ? 1 : 0;
}

// The SSL_CTX (per-connection, possibly pooled beyond the originating request) holds a strong reference to the
// collector; this releases it when the SSL_CTX is destroyed.
static void collector_ex_data_free(void*, void* ptr, CRYPTO_EX_DATA*, int, long, void*)
{
    if (ptr)
        static_cast<AIACollector*>(ptr)->unref();
}

static int collector_ex_data_index()
{
    static int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr, collector_ex_data_free);
    return index;
}

void apply_aia_verification(SSL_CTX* ssl_context, AIACollector& collector)
{
    // The ex-data slot owns the reference taken here, and collector_ex_data_free() releases it when the SSL_CTX
    // goes away. On a failed set, the pointer isn't stored — so nothing would ever release it. Hand it back here.
    collector.ref();
    if (SSL_CTX_set_ex_data(ssl_context, collector_ex_data_index(), &collector) != 1) {
        collector.unref();
        return;
    }
    SSL_CTX_set_cert_verify_callback(ssl_context, verify_callback, &collector);

    // curl sets SSL_VERIFY_NONE and looks at the verification result once the handshake is done. Then a host name
    // mismatch found during verification has to fail the handshake.
    if (collector.checks_host_name)
        SSL_CTX_set_verify(ssl_context, SSL_VERIFY_PEER, SSL_CTX_get_verify_callback(ssl_context));
}

}
