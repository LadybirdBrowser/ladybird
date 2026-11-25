/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <AK/ScopeGuard.h>
#include <AK/Vector.h>
#include <RequestServer/SystemTrustMacOS.h>

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>
#include <openssl/x509.h>

namespace RequestServer {

static SecCertificateRef create_certificate(X509* certificate)
{
    unsigned char* der = nullptr;
    auto length = i2d_X509(certificate, &der);
    if (length <= 0)
        return nullptr;
    ScopeGuard free_der = [&] { OPENSSL_free(der); };

    auto* data = CFDataCreate(nullptr, der, length);
    if (!data)
        return nullptr;
    ScopeGuard release_data = [&] { CFRelease(data); };
    return SecCertificateCreateWithData(nullptr, data);
}

static ByteString byte_string_from(CFStringRef string)
{
    if (!string)
        return {};
    auto length = CFStringGetMaximumSizeForEncoding(CFStringGetLength(string), kCFStringEncodingUTF8) + 1;
    Vector<char> buffer;
    buffer.resize(length);
    if (!CFStringGetCString(string, buffer.data(), length, kCFStringEncodingUTF8))
        return {};
    return ByteString { buffer.data() };
}

// Recheck dates: the system may add certificates OpenSSL never saw, such as keychain intermediates.
static void add_validity_period_error(SecCertificateRef certificate, SystemTrustVerdict& verdict, Vector<ByteString>& unnamed_reasons)
{
    X509* parsed = nullptr;
    if (auto* data = SecCertificateCopyData(certificate)) {
        auto const* bytes = CFDataGetBytePtr(data);
        parsed = d2i_X509(nullptr, &bytes, CFDataGetLength(data));
        CFRelease(data);
    }
    ScopeGuard free_parsed = [&] { X509_free(parsed); };

    if (parsed && X509_cmp_current_time(X509_get0_notAfter(parsed)) < 0)
        verdict.errors |= Requests::CertificateError::Expired;
    else if (parsed && X509_cmp_current_time(X509_get0_notBefore(parsed)) > 0)
        verdict.errors |= Requests::CertificateError::NotYetValid;
    else
        unnamed_reasons.append("TemporalValidity"sv);
}

static void add_reasons(CFDictionaryRef certificate_details, SecCertificateRef certificate, SystemTrustVerdict& verdict, Vector<ByteString>& unnamed_reasons)
{
    auto count = CFDictionaryGetCount(certificate_details);
    Vector<void const*> keys;
    keys.resize(count);
    CFDictionaryGetKeysAndValues(certificate_details, keys.data(), nullptr);

    for (auto const* key : keys) {
        if (CFGetTypeID(key) != CFStringGetTypeID())
            continue;
        auto name = byte_string_from(static_cast<CFStringRef>(key));
        // The chain ends at a root that isn't trusted.
        if (name == "AnchorTrusted"sv)
            verdict.errors |= Requests::CertificateError::SelfSigned;
        else if (name == "MissingIntermediate"sv)
            verdict.errors |= Requests::CertificateError::UnknownIssuer;
        else if (name == "TemporalValidity"sv && certificate)
            add_validity_period_error(certificate, verdict, unnamed_reasons);
        // The caller checks host names; StatusCodes lists codes for the other errors.
        else if (!name.is_one_of("SSLHostname"sv, "StatusCodes"sv) && !unnamed_reasons.contains_slow(name))
            unnamed_reasons.append(move(name));
    }
}

SystemTrustVerdict system_trust_verdict(X509* leaf, ReadonlySpan<X509*> intermediates)
{
    SystemTrustVerdict verdict;

    auto* certificates = CFArrayCreateMutable(nullptr, 0, &kCFTypeArrayCallBacks);
    ScopeGuard release_certificates = [&] { CFRelease(certificates); };
    Vector<X509*> chain { leaf };
    for (auto* intermediate : intermediates) {
        if (X509_cmp(intermediate, leaf) != 0)
            chain.append(intermediate);
    }
    for (auto* certificate : chain) {
        auto* system_certificate = create_certificate(certificate);
        if (!system_certificate)
            continue;
        CFArrayAppendValue(certificates, system_certificate);
        CFRelease(system_certificate);
    }

    // The caller checks host names using curl's matching rules.
    auto* policy = SecPolicyCreateSSL(true, nullptr);
    ScopeGuard release_policy = [&] { CFRelease(policy); };

    SecTrustRef trust = nullptr;
    if (CFArrayGetCount(certificates) == 0 || SecTrustCreateWithCertificates(certificates, policy, &trust) != errSecSuccess) {
        verdict.errors |= Requests::CertificateError::Other;
        return verdict;
    }
    ScopeGuard release_trust = [&] { CFRelease(trust); };

    CFErrorRef error = nullptr;
    if (SecTrustEvaluateWithError(trust, &error))
        return verdict;
    ScopeGuard release_error = [&] {
        if (error)
            CFRelease(error);
    };

    // Undocumented per-certificate details distinguish rejection reasons. Without them, use the system's description.
    Vector<ByteString> unnamed_reasons;
    if (auto* result = SecTrustCopyResult(trust)) {
        ScopeGuard release_result = [&] { CFRelease(result); };
        // Details follow the system's certificate chain order.
        auto* chain = SecTrustCopyCertificateChain(trust);
        ScopeGuard release_chain = [&] {
            if (chain)
                CFRelease(chain);
        };
        auto const* details = CFDictionaryGetValue(result, CFSTR("TrustResultDetails"));
        if (details && CFGetTypeID(details) == CFArrayGetTypeID()) {
            auto* array = static_cast<CFArrayRef>(details);
            for (CFIndex i = 0; i < CFArrayGetCount(array); ++i) {
                auto const* certificate_details = CFArrayGetValueAtIndex(array, i);
                if (CFGetTypeID(certificate_details) != CFDictionaryGetTypeID())
                    continue;
                auto certificate = chain && i < CFArrayGetCount(chain) ? static_cast<SecCertificateRef>(const_cast<void*>(CFArrayGetValueAtIndex(chain, i))) : nullptr;
                add_reasons(static_cast<CFDictionaryRef>(certificate_details), certificate, verdict, unnamed_reasons);
            }
        }
    }

    if (unnamed_reasons.is_empty() && verdict.errors != Requests::CertificateError::None)
        return verdict;

    // The description names only the most serious reason. Include all unnamed reasons so exceptions cannot
    // cover reasons the user didn't review.
    verdict.errors |= Requests::CertificateError::Other;
    ByteString description;
    if (error) {
        auto* error_description = CFErrorCopyDescription(error);
        description = byte_string_from(error_description);
        if (error_description)
            CFRelease(error_description);
    }
    quick_sort(unnamed_reasons);
    if (unnamed_reasons.is_empty())
        verdict.details = move(description);
    else if (description.is_empty())
        verdict.details = ByteString::join(", "sv, unnamed_reasons);
    else
        verdict.details = ByteString::formatted("{} ({})", description, ByteString::join(", "sv, unnamed_reasons));
    return verdict;
}

}
