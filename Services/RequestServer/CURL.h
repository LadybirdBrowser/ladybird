/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Include this header instead of <curl/curl.h>. Only include this header from .cpp files.

#include <AK/ByteString.h>
#include <AK/Platform.h>
#include <AK/StringView.h>
#include <LibDNS/Resolver.h>
#include <LibRequests/NetworkError.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h> // Needed because curl.h includes winsock2.h
#endif

#include <array>
#include <curl/curl.h>

namespace RequestServer {

ByteString build_curl_resolve_list(DNS::LookupResult const& dns_result, StringView host, u16 port);

struct ErrorMapping {
    int curl_code;
    Requests::NetworkError network_error;
};

inline constexpr Requests::NetworkError curl_code_to_network_error(int code)
{
    constexpr std::array<ErrorMapping, 10> error_map { {
        { CURLE_COULDNT_RESOLVE_HOST, Requests::NetworkError::UnableToResolveHost },
        { CURLE_COULDNT_RESOLVE_PROXY, Requests::NetworkError::UnableToResolveProxy },
        { CURLE_COULDNT_CONNECT, Requests::NetworkError::UnableToConnect },
        { CURLE_OPERATION_TIMEDOUT, Requests::NetworkError::TimeoutReached },
        { CURLE_TOO_MANY_REDIRECTS, Requests::NetworkError::TooManyRedirects },
        { CURLE_SSL_CONNECT_ERROR, Requests::NetworkError::SSLHandshakeFailed },
        { CURLE_PEER_FAILED_VERIFICATION, Requests::NetworkError::SSLVerificationFailed },
        { CURLE_URL_MALFORMAT, Requests::NetworkError::MalformedUrl },
        { CURLE_PARTIAL_FILE, Requests::NetworkError::IncompleteContent },
        { CURLE_BAD_CONTENT_ENCODING, Requests::NetworkError::InvalidContentEncoding },
    } };

    for (auto const& entry : error_map) {
        if (entry.curl_code == code)
            return entry.network_error;
    }

    return Requests::NetworkError::Unknown;
}

}
