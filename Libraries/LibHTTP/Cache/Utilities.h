/*
 * Copyright (c) 2025-2026, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/LexicalPath.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibHTTP/Forward.h>
#include <LibHTTP/HeaderList.h>
#include <LibURL/Forward.h>

namespace HTTP {

constexpr inline auto TEST_CACHE_ENABLED_HEADER = "X-Ladybird-Enable-Disk-Cache"sv;
constexpr inline auto TEST_CACHE_STATUS_HEADER = "X-Ladybird-Disk-Cache-Status"sv;
constexpr inline auto TEST_CACHE_REVALIDATION_STATUS_HEADER = "X-Ladybird-Revalidation-Status"sv;
constexpr inline auto TEST_CACHE_REQUEST_TIME_OFFSET = "X-Ladybird-Request-Time-Offset"sv;

constexpr inline u64 DEFAULT_MAXIMUM_DISK_CACHE_SIZE = 5 * GiB;

u64 compute_maximum_disk_cache_size(u64 free_bytes, u64 limit_maximum_disk_cache_size = DEFAULT_MAXIMUM_DISK_CACHE_SIZE);
u64 compute_maximum_disk_cache_entry_size(u64 maximum_disk_cache_size);

String serialize_url_for_cache_storage(URL::URL const&);
u64 create_cache_key(StringView url, StringView method);
u64 create_vary_key(HeaderList const& request_headers, HeaderList const& response_headers);
LexicalPath path_for_cache_entry(LexicalPath const& cache_directory, u64 cache_key, u64 vary_key);

bool is_cacheable(StringView method, HeaderList const&);
bool is_cacheable(u32 status_code, HeaderList const&);
bool is_header_exempted_from_storage(StringView name);

AK::Duration calculate_freshness_lifetime(u32 status_code, HeaderList const&, AK::Duration current_time_offset_for_testing = {});
AK::Duration calculate_age(HeaderList const&, UnixDateTime request_time, UnixDateTime response_time, AK::Duration current_time_offset_for_testing = {});
AK::Duration calculate_stale_while_revalidate_lifetime(HeaderList const&, AK::Duration freshness_lifetime);

enum class CacheLifetimeStatus {
    Fresh,
    Expired,
    MustRevalidate,
    StaleWhileRevalidate,
};
CacheLifetimeStatus cache_lifetime_status(HeaderList const& request_headers, HeaderList const& response_headers, AK::Duration freshness_lifetime, AK::Duration current_age);

struct RevalidationAttributes {
    static RevalidationAttributes create(HeaderList const&);

    Optional<ByteString> etag;
    Optional<ByteString> last_modified;
};

void store_header_and_trailer_fields(HeaderList&, HeaderList const&);
void update_header_fields(HeaderList&, HeaderList const&);

bool contains_cache_control_directive(StringView cache_control, StringView directive);
Optional<StringView> extract_cache_control_directive(StringView cache_control, StringView directive);
Optional<AK::Duration> extract_cache_control_duration_directive(StringView cache_control, StringView directive, Optional<AK::Duration> valueless_fallback = {});

ByteString normalize_request_vary_header_values(StringView header, HeaderList const& request_headers);

AK::Duration compute_current_time_offset_for_testing(Optional<DiskCache&>, HeaderList const& request_headers);

}
