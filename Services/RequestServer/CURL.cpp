/*
 * Copyright (c) 2024, Andrew Kaster <andrew@ladybird.org>
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <RequestServer/CURL.h>
#include <array>
#include <utility>

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

}
