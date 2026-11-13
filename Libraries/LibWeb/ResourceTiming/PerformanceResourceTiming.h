/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PerformanceResourceTiming.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWeb/PerformanceTimeline/PerformanceEntry.h>

namespace Web::ResourceTiming {

// https://w3c.github.io/resource-timing/#dom-performanceresourcetiming
class WEB_API PerformanceResourceTiming : public PerformanceTimeline::PerformanceEntry {
    WEB_RUST_INTERFACE(PerformanceResourceTiming, performance_resource_timing)
};

// The ABI of a response body info, as Bindings/Rust/src/resource_timing/performance_resource_timing.rs takes it.
struct ResponseBodyInfoABI {
    u64 encoded_size;
    u64 decoded_size;
    FlatPtr content_type;
};

ResponseBodyInfoABI response_body_info_to_abi(Fetch::Infrastructure::Response::BodyInfo const&);
u8 cache_state_to_abi(Optional<Fetch::Infrastructure::Response::CacheState> const&);

// https://w3c.github.io/resource-timing/#dfn-mark-resource-timing
WEB_API void mark_resource_timing(NonnullRefPtr<Fetch::Infrastructure::FetchTimingInfo>, Utf16String const& requested_url, Utf16FlyString const& initiator_type, JS::Object& global, Optional<Fetch::Infrastructure::Response::CacheState> const& cache_mode, Fetch::Infrastructure::Response::BodyInfo const&, Fetch::Infrastructure::Status response_status, Utf16FlyString const& delivery_type = ""_utf16_fly_string);

}
