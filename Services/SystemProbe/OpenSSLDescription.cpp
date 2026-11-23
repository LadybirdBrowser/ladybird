/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <SystemProbe/OpenSSLDescription.h>
#include <curl/curl.h>
#include <openssl/crypto.h>
#include <string.h>

PlatformFeatures::OpenSSLFacts describe_openssl()
{
    PlatformFeatures::OpenSSLFacts facts;
    facts.linked_version = MUST(String::formatted("{}.{}.{}", OPENSSL_version_major(), OPENSSL_version_minor(), OPENSSL_version_patch()));

    // A MultiSSL curl only settles on its backend once initialized, so it's initialized the way RequestServer does it
    // before being asked which one that is.
    if (auto result = curl_global_init(CURL_GLOBAL_ALL); result != CURLE_OK) {
        warnln("curl_global_init failed: {}", curl_easy_strerror(result));
        return facts;
    }

    auto const* version_info = curl_version_info(CURLVERSION_NOW);
    if (version_info && version_info->ssl_version) {
        if (auto version = PlatformFeatures::OpenSSLFacts::openssl_version_from_curl_ssl_version({ version_info->ssl_version, strlen(version_info->ssl_version) }); version.has_value())
            facts.curl_version = MUST(String::from_utf8(*version));
    }
    curl_global_cleanup();
    return facts;
}
