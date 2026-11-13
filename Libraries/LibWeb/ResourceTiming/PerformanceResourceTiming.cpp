/*
 * Copyright (c) 2025, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/ResourceTiming/PerformanceResourceTiming.h>

namespace Web::ResourceTiming {

extern "C" {
// Defined in Bindings/Rust/src/resource_timing/performance_resource_timing.rs.
void web_rust_resource_timing_mark_resource_timing(JSVM*, void* realm, Fetch::Infrastructure::FetchTimingInfo*, FlatPtr requested_url, FlatPtr initiator_type, u8 cache_mode, ResponseBodyInfoABI body_info, u16 response_status, FlatPtr delivery_type);
}

ResponseBodyInfoABI response_body_info_to_abi(Fetch::Infrastructure::Response::BodyInfo const& body_info)
{
    return {
        .encoded_size = body_info.encoded_size,
        .decoded_size = body_info.decoded_size,
        .content_type = Utf16String { body_info.content_type }.into_raw(),
    };
}

u8 cache_state_to_abi(Optional<Fetch::Infrastructure::Response::CacheState> const& cache_state)
{
    if (!cache_state.has_value())
        return 0;
    switch (*cache_state) {
    case Fetch::Infrastructure::Response::CacheState::Local:
        return 1;
    case Fetch::Infrastructure::Response::CacheState::Validated:
        return 2;
    }
    VERIFY_NOT_REACHED();
}

void mark_resource_timing(NonnullRefPtr<Fetch::Infrastructure::FetchTimingInfo> timing_info, Utf16String const& requested_url, Utf16FlyString const& initiator_type, JS::Object& global, Optional<Fetch::Infrastructure::Response::CacheState> const& cache_mode, Fetch::Infrastructure::Response::BodyInfo const& body_info, Fetch::Infrastructure::Status response_status, Utf16FlyString const& delivery_type)
{
    auto& realm = HTML::relevant_realm(global);
    web_rust_resource_timing_mark_resource_timing(
        JS::HostABI::vm_to_abi(realm.vm()),
        &realm,
        &timing_info.leak_ref(),
        Utf16String { requested_url }.into_raw(),
        initiator_type.raw_identity(),
        cache_state_to_abi(cache_mode),
        response_body_info_to_abi(body_info),
        response_status,
        delivery_type.raw_identity());
}

}
