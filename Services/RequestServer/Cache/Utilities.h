/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StringView.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <LibHTTP/HeaderMap.h>
#include <LibURL/Forward.h>

namespace RequestServer {

String serialize_url_for_cache_storage(URL::URL const&);
u64 create_cache_key(StringView url, StringView method);

bool is_cacheable(StringView method);
bool is_cacheable(u32 status_code, HTTP::HeaderMap const&);
bool is_header_exempted_from_storage(StringView name);

AK::Duration calculate_freshness_lifetime(HTTP::HeaderMap const&);
AK::Duration calculate_age(HTTP::HeaderMap const&, UnixDateTime request_time, UnixDateTime response_time);
bool is_response_fresh(AK::Duration freshness_lifetime, AK::Duration current_age);

}
