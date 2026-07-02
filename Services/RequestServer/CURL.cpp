/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <AK/ScopeGuard.h>
#include <LibCore/Environment.h>
#include <LibCore/File.h>
#include <RequestServer/CURL.h>

namespace RequestServer {

static Optional<ByteString> resolve_default_root_certificate_bundle_path()
{
    if (auto value = Core::Environment::get("CURL_CA_BUNDLE"sv); value.has_value() && !value->is_empty())
        return value->to_byte_string();
    if (auto value = Core::Environment::get("SSL_CERT_FILE"sv); value.has_value() && !value->is_empty())
        return value->to_byte_string();

    auto* easy_handle = curl_easy_init();
    if (!easy_handle)
        return {};
    ScopeGuard cleanup = [&] { curl_easy_cleanup(easy_handle); };

    char* ca_info = nullptr;
    if (curl_easy_getinfo(easy_handle, CURLINFO_CAINFO, &ca_info) == CURLE_OK && ca_info && *ca_info)
        return ByteString { ca_info };

    return {};
}

static ErrorOr<ByteBuffer> read_certificate_files(Vector<ByteString> const& paths)
{
    ByteBuffer bundle;

    for (auto const& path : paths) {
        auto file = TRY(Core::File::open(path, Core::File::OpenMode::Read));
        auto contents = TRY(file->read_until_eof());
        TRY(bundle.try_append(contents));

        // Concatenated PEM blocks must be newline-separated to remain individually parseable.
        if (!bundle.is_empty() && bundle[bundle.size() - 1] != '\n')
            TRY(bundle.try_append("\n", 1));
    }

    return bundle;
}

ErrorOr<ByteBuffer> read_default_root_certificate_bundle(Vector<ByteString> const& certificate_overrides)
{
    if (!certificate_overrides.is_empty())
        return read_certificate_files(certificate_overrides);

    auto bundle_path = resolve_default_root_certificate_bundle_path();
    if (!bundle_path.has_value())
        return ByteBuffer {};

    auto file = Core::File::open(*bundle_path, Core::File::OpenMode::Read);
    if (file.is_error()) {
        warnln("RequestServer: Unable to open root certificate bundle '{}': {}", *bundle_path, file.error());
        return ByteBuffer {};
    }

    return TRY(file.value()->read_until_eof());
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
