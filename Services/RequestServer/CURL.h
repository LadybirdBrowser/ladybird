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

#include <curl/curl.h>

namespace RequestServer {

ByteString build_curl_resolve_list(DNS::LookupResult const& dns_result, StringView host, u16 port);
Requests::NetworkError curl_code_to_network_error(int code);

}
