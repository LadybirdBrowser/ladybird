/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <RequestServer/CURL.h>

namespace RequestServer {

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
