/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

// Include this header instead of <curl/curl.h>. Only include this header from .cpp files.

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibDNS/Resolver.h>
#include <LibRequests/NetworkError.h>

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h> // Needed because curl.h includes winsock2.h
#endif

#include <curl/curl.h>

namespace RequestServer {

ByteString build_curl_resolve_list(DNS::LookupResult const& dns_result, StringView host, u16 port);

// A CURLOPT_CONNECT_TO entry pinning one request to a single address out of the resolved pool, so that several
// requests to the same host can be spread across the addresses it resolved to instead of all piling onto whichever
// one libcurl would pick. Empty when there is only one address, i.e. nothing to spread over.
Optional<ByteString> build_curl_connect_to_entry(DNS::LookupResult const& dns_result, StringView host, u16 port, u32 address_index);
Requests::NetworkError curl_code_to_network_error(int code);
ErrorOr<void> initialize_libcurl();

ErrorOr<ByteBuffer> read_default_root_certificate_bundle(Vector<ByteString> const& certificate_overrides);

}
