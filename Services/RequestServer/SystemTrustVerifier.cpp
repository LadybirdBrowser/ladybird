/*
 * Copyright (c) 2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <RequestServer/CURL.h>
#include <RequestServer/SystemTrustVerifier.h>

#ifdef AK_OS_MACOS
#    include <Security/Security.h>
#    include <openssl/ssl.h>
#    include <openssl/x509.h>
#    include <openssl/x509_vfy.h>
#endif

namespace RequestServer {

#ifdef AK_OS_MACOS
static Optional<ByteString> copy_cf_string(CFStringRef string)
{
    auto length = CFStringGetLength(string);
    auto maximum_size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    auto buffer = MUST(ByteBuffer::create_uninitialized(maximum_size));
    if (!CFStringGetCString(string, reinterpret_cast<char*>(buffer.data()), maximum_size, kCFStringEncodingUTF8))
        return {};

    return ByteString { reinterpret_cast<char const*>(buffer.data()) };
}

static Optional<Vector<SecCertificateRef>> create_sec_certificate_chain(X509_STORE_CTX& store_context)
{
    auto* leaf_certificate = X509_STORE_CTX_get0_cert(&store_context);
    if (!leaf_certificate)
        return {};

    auto certificates = Vector<SecCertificateRef> {};
    ScopeGuard cleanup = [&] {
        for (auto* cert : certificates)
            CFRelease(cert);
    };

    auto append_certificate = [&](X509& certificate) -> bool {
        auto encoded_size = i2d_X509(&certificate, nullptr);
        if (encoded_size <= 0)
            return false;

        auto encoded_certificate = MUST(ByteBuffer::create_uninitialized(encoded_size));
        auto* encoded_data = encoded_certificate.data();
        if (i2d_X509(&certificate, &encoded_data) != encoded_size)
            return false;

        auto const* data = CFDataCreate(kCFAllocatorDefault, encoded_certificate.data(), encoded_size);
        if (!data)
            return false;
        ScopeGuard release_data = [data] { CFRelease(data); };

        auto* sec_certificate = SecCertificateCreateWithData(kCFAllocatorDefault, data);
        if (!sec_certificate)
            return false;

        certificates.append(sec_certificate);
        return true;
    };

    if (!append_certificate(*leaf_certificate))
        return {};

    if (auto* untrusted_certificates = X509_STORE_CTX_get0_untrusted(&store_context)) {
        for (auto index = 0; index < sk_X509_num(untrusted_certificates); ++index) {
            auto* certificate = sk_X509_value(untrusted_certificates, index);
            if (!certificate || certificate == leaf_certificate)
                continue;
            if (!append_certificate(*certificate))
                return {};
        }
    }

    return certificates;
}

static int sec_trust_verify_callback(X509_STORE_CTX* store_context, void* argument)
{
    auto const* context = static_cast<TLSVerificationContext const*>(argument);
    VERIFY(context);
    auto hostname = context->url.serialized_host().to_byte_string();
    VERIFY(!hostname.is_empty());

    auto* ssl = static_cast<SSL*>(X509_STORE_CTX_get_ex_data(store_context, SSL_get_ex_data_X509_STORE_CTX_idx()));
    if (!ssl) {
        X509_STORE_CTX_set_error(store_context, X509_V_ERR_APPLICATION_VERIFICATION);
        return 0;
    }

    auto certificate_chain = create_sec_certificate_chain(*store_context);
    if (!certificate_chain.has_value()) {
        dbgln("RequestServer: Failed to convert OpenSSL peer certificates into Security.framework certificates for {}", hostname);
        X509_STORE_CTX_set_error(store_context, X509_V_ERR_APPLICATION_VERIFICATION);
        return 0;
    }

    auto* certificate_array = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
    if (!certificate_array) {
        X509_STORE_CTX_set_error(store_context, X509_V_ERR_APPLICATION_VERIFICATION);
        return 0;
    }
    ScopeGuard release_certificate_array = [certificate_array] { CFRelease(certificate_array); };
    ScopeGuard release_certificates = [&certificate_chain] {
        for (auto const* certificate : *certificate_chain)
            CFRelease(certificate);
    };

    for (auto const* certificate : *certificate_chain)
        CFArrayAppendValue(certificate_array, certificate);

    auto const* hostname_string = CFStringCreateWithCString(kCFAllocatorDefault, hostname.characters(), kCFStringEncodingUTF8);
    if (!hostname_string) {
        X509_STORE_CTX_set_error(store_context, X509_V_ERR_APPLICATION_VERIFICATION);
        return 0;
    }
    ScopeGuard release_hostname = [hostname_string] { CFRelease(hostname_string); };

    auto const* policy = SecPolicyCreateSSL(true, hostname_string);
    if (!policy) {
        X509_STORE_CTX_set_error(store_context, X509_V_ERR_APPLICATION_VERIFICATION);
        return 0;
    }
    ScopeGuard release_policy = [policy] { CFRelease(policy); };

    SecTrustRef trust = nullptr;
    if (SecTrustCreateWithCertificates(certificate_array, policy, &trust) != errSecSuccess || !trust) {
        X509_STORE_CTX_set_error(store_context, X509_V_ERR_APPLICATION_VERIFICATION);
        return 0;
    }
    ScopeGuard release_trust = [trust] { CFRelease(trust); };

    SecTrustSetNetworkFetchAllowed(trust, true);

    CFErrorRef error = nullptr;
    auto is_trusted = SecTrustEvaluateWithError(trust, &error);
    ScopeGuard release_error = [&] {
        if (error)
            CFRelease(error);
    };

    if (is_trusted)
        return 1;

    Optional<ByteString> error_description;
    if (error) {
        if (auto const* description = CFErrorCopyDescription(error)) {
            error_description = copy_cf_string(description);
            CFRelease(description);
        }
    }
    if (error_description.has_value())
        dbgln("RequestServer: Security.framework rejected TLS certificate for {}: {}", hostname, *error_description);
    else
        dbgln("RequestServer: Security.framework rejected TLS certificate for {}", hostname);

    X509_STORE_CTX_set_error(store_context, X509_V_ERR_APPLICATION_VERIFICATION);
    return 0;
}
#endif

int setup_system_trust_verifier(void*, void* ssl_ctx, void* user_data)
{
#ifdef AK_OS_MACOS
    SSL_CTX_set_cert_verify_callback(static_cast<SSL_CTX*>(ssl_ctx), sec_trust_verify_callback, user_data);
    return CURLE_OK;
#else
    (void)ssl_ctx;
    (void)user_data;
    return CURLE_NOT_BUILT_IN;
#endif
}

}
