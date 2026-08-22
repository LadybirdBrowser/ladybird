/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <RequestServer/AIA.h>
#include <RequestServer/CURL.h>

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/Time.h>
#include <AK/Vector.h>

#include <openssl/pem.h>
#include <openssl/pkcs7.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <openssl/x509v3.h>

namespace RequestServer {

// This max was chosen to align with the (telemetry-tuned) value Chromium uses.
static constexpr size_t max_ca_issuers_urls_per_certificate = 5;

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

// Process-wide cache of intermediate certs fetched via AIA. Each entry owns one reference to the X509. Bounded by
// max_cached_intermediates with FIFO eviction.
static Vector<X509*>& intermediate_cache()
{
    static Vector<X509*> cache;
    return cache;
}

// Process-wide record of caIssuers URLs whose fetch failed, mapped to when they failed — so we don't repeatedly retry a
// dead URL. Entries older than failed_url_retry_interval are ignored (and dropped when next encountered) — so a
// transiently-unreachable AIA server is eventually retried.
static HashMap<ByteString, MonotonicTime>& failed_urls()
{
    static HashMap<ByteString, MonotonicTime> urls;
    return urls;
}

void mark_aia_url_failed(ByteString url)
{
    if (failed_urls().size() >= max_failed_urls)
        failed_urls().clear();
    failed_urls().set(move(url), MonotonicTime::now_coarse());
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

    if (auto* bio = BIO_new_mem_buf(body.data(), static_cast<int>(body.size()))) {
        while (auto* certificate = PEM_read_bio_X509(bio, nullptr, nullptr, nullptr))
            certificates.append(certificate);
        BIO_free(bio);
    }

    return certificates;
}

bool add_fetched_aia_intermediate(ReadonlyBytes body)
{
    auto certificates = parse_certificates(body);
    for (auto* certificate : certificates) {
        if (intermediate_cache().size() >= max_cached_intermediates)
            X509_free(intermediate_cache().take_first());
        intermediate_cache().append(certificate);
    }
    return !certificates.is_empty();
}

// Full certificate-verification callback, installed via SSL_CTX_set_cert_verify_callback. We offer the AIA-fetched
// intermediates to path building as UNTRUSTED certificates — never as trust anchors — so the completed chain must still
// terminate at a locally-trusted root. On a verification failure, we record the failing cert's caIssuers URLs (skipping
// any recently known to be dead) — so the request layer can fetch them and retry.
static int verify_callback(X509_STORE_CTX* context, void* collector_data)
{
    auto* collector = static_cast<AIACollector*>(collector_data);

    // Add the fetched intermediates alongside the certs the server sent, as untrusted path-building material.
    auto* server_untrusted = X509_STORE_CTX_get0_untrusted(context);
    auto* untrusted = server_untrusted ? sk_X509_dup(server_untrusted) : sk_X509_new_null();
    if (untrusted) {
        for (auto* candidate : intermediate_cache())
            sk_X509_push(untrusted, candidate);
        X509_STORE_CTX_set0_untrusted(context, untrusted);
    }

    auto result = X509_verify_cert(context);

    if (untrusted) {
        X509_STORE_CTX_set0_untrusted(context, server_untrusted);
        sk_X509_free(untrusted);
    }

    // AIA can only repair a chain missing an intermediate issuer. For any other verification failure (expired,
    // revoked, hostname mismatch, ...) fetching caIssuers is useless and would burn several 10s retries before the
    // real error surfaces, so we leave the collector's pending URLs untouched.
    auto const verify_error = X509_STORE_CTX_get_error(context);
    auto const issuer_is_missing = verify_error == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT || verify_error == X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY;

    if (result <= 0 && collector && issuer_is_missing) {
        if (auto* current = X509_STORE_CTX_get_current_cert(context)) {
            for (auto& url : ca_issuers_urls(current)) {
                if (collector->attempted_urls.contains(url))
                    continue;
                if (auto failed_at = failed_urls().get(url); failed_at.has_value()) {
                    if (MonotonicTime::now_coarse() - *failed_at < failed_url_retry_interval)
                        continue;
                    failed_urls().remove(url);
                }
                if (!collector->pending_urls.contains_slow(url))
                    collector->pending_urls.append(move(url));
            }
        }
    }

    return result;
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

static CURLcode ssl_context_callback(CURL*, void* ssl_context, void* collector_data)
{
    auto* context = static_cast<SSL_CTX*>(ssl_context);
    auto* collector = static_cast<AIACollector*>(collector_data);
    collector->ref();
    SSL_CTX_set_ex_data(context, collector_ex_data_index(), collector);
    SSL_CTX_set_cert_verify_callback(context, verify_callback, collector);
    return CURLE_OK;
}

void install_aia_verification_hook(CURL* easy_handle, AIACollector& collector)
{
    curl_easy_setopt(easy_handle, CURLOPT_SSL_CTX_FUNCTION, ssl_context_callback);
    curl_easy_setopt(easy_handle, CURLOPT_SSL_CTX_DATA, &collector);
}

}
