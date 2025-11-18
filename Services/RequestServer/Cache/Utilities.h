/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/LexicalPath.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibHTTP/HeaderMap.h>
#include <LibURL/Forward.h>
#include <RequestServer/Forward.h>

namespace RequestServer {

constexpr inline auto TEST_CACHE_ENABLED_HEADER = "X-Ladybird-Enable-Disk-Cache"sv;
constexpr inline auto TEST_CACHE_STATUS_HEADER = "X-Ladybird-Disk-Cache-Status"sv;
constexpr inline auto TEST_CACHE_REQUEST_TIME_OFFSET = "X-Ladybird-Request-Time-Offset"sv;

String serialize_url_for_cache_storage(URL::URL const&);
u64 create_cache_key(StringView url, StringView method);
LexicalPath path_for_cache_key(LexicalPath const& cache_directory, u64 cache_key);

bool is_cacheable(StringView method);
bool is_cacheable(u32 status_code, HTTP::HeaderMap const&);
bool is_header_exempted_from_storage(StringView name);

AK::Duration calculate_freshness_lifetime(u32 status_code, HTTP::HeaderMap const&, AK::Duration current_time_offset_for_testing);
AK::Duration calculate_age(HTTP::HeaderMap const&, UnixDateTime request_time, UnixDateTime response_time, AK::Duration current_time_offset_for_testing);

enum class CacheLifetimeStatus {
    Fresh,
    Expired,
    MustRevalidate,
};
CacheLifetimeStatus cache_lifetime_status(HTTP::HeaderMap const&, AK::Duration freshness_lifetime, AK::Duration current_age);

struct RevalidationAttributes {
    static RevalidationAttributes create(HTTP::HeaderMap const&);

    Optional<ByteString> etag;
    Optional<UnixDateTime> last_modified;
};

void update_header_fields(HTTP::HeaderMap&, HTTP::HeaderMap const&);

AK::Duration compute_current_time_offset_for_testing(Optional<DiskCache&>, HTTP::HeaderMap const& request_headers);

}
