/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Fetch/Infrastructure/FetchTimingInfo.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Infra/SerializedURL.h>
#include <LibWeb/NavigationTiming/PerformanceNavigationTiming.h>

namespace Web::NavigationTiming {

extern "C" {
// Defined in Bindings/Rust/src/navigation_timing/performance_navigation_timing.rs.
void web_rust_navigation_timing_create_the_navigation_timing_entry(JSVM*, void* realm, DOM::Document*, FlatPtr document_url, Fetch::Infrastructure::FetchTimingInfo*, u16 redirect_count, u8 navigation_type, u8 cache_mode, ResourceTiming::ResponseBodyInfoABI body_info, u16 response_status);
}

void create_the_navigation_timing_entry(DOM::Document& document, NonnullRefPtr<Fetch::Infrastructure::FetchTimingInfo> timing_info, u16 redirect_count, Bindings::NavigationTimingType navigation_type, Optional<Fetch::Infrastructure::Response::CacheState> const& cache_mode, Fetch::Infrastructure::Response::BodyInfo const& body_info, Fetch::Infrastructure::Status response_status)
{
    auto& realm = HTML::relevant_realm(document);
    web_rust_navigation_timing_create_the_navigation_timing_entry(
        JS::HostABI::vm_to_abi(realm.vm()),
        &realm,
        &document,
        utf16_string_from_url_ascii(document.url().serialize()).into_raw(),
        &timing_info.leak_ref(),
        redirect_count,
        to_underlying(navigation_type),
        ResourceTiming::cache_state_to_abi(cache_mode),
        ResourceTiming::response_body_info_to_abi(body_info),
        response_status);
}

}
