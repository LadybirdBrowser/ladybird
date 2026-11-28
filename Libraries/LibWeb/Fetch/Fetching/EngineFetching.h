/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/RefPtr.h>
#include <AK/Variant.h>
#include <LibCore/ImmutableBytes.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibHTTP/Forward.h>
#include <LibRequests/RequestTransferLease.h>
#include <LibURL/Forward.h>
#include <LibWeb/ContentSecurityPolicy/FetchIntegration.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>

namespace Web::Fetch::Engine {

class FetchLoad;
class WholeBodyTransport;

}

namespace Web::Fetch::Fetching {

class PendingResponse;

// https://fetch.spec.whatwg.org/#http-network-or-cache-fetch
// If the sum of contentLength and inflightKeepaliveBytes is greater than 64 kibibytes, then return a network error.
constexpr auto keepalive_maximum_size = 64 * KiB;

#define ENUMERATE_BOOL_PARAMS                     \
    __ENUMERATE_BOOL_PARAM(IsAuthenticationFetch) \
    __ENUMERATE_BOOL_PARAM(IsNewConnectionFetch)  \
    __ENUMERATE_BOOL_PARAM(MakeCORSPreflight)     \
    __ENUMERATE_BOOL_PARAM(Recursive)

#define __ENUMERATE_BOOL_PARAM(Name) \
    enum class Name {                \
        Yes,                         \
        No,                          \
    };
ENUMERATE_BOOL_PARAMS
#undef __ENUMERATE_BOOL_PARAM

// What preparing main fetch's request leaves: the response it set, and the host still to look up for its HSTS upgrade.
struct MainFetchPreparation {
    RefPtr<Infrastructure::Response> response;
    Optional<String> host_to_look_up;
};
[[nodiscard]] MainFetchPreparation prepare_main_fetch(Infrastructure::FetchParams const&, ContentSecurityPolicy::ViolationReporter const&, Engine::KnownHSTSHostProvider&);
NonnullRefPtr<PendingResponse> recursive_main_fetch(Infrastructure::FetchParams const&, ContentSecurityPolicy::ViolationReporter const&, Engine::KnownHSTSHostProvider&);
[[nodiscard]] Optional<String> host_to_match_against_known_hsts_hosts(Infrastructure::Request const&);
void upgrade_request_to_https(Infrastructure::Request&);
void run_main_fetch_parallel_steps(Engine::MainFetchInput, NonnullRefPtr<Engine::FetchEngineRecord>);
void fetch_response_handover(Infrastructure::FetchParams const&, Infrastructure::Response&);
NonnullRefPtr<PendingResponse> scheme_fetch(Infrastructure::FetchParams const&);
NonnullRefPtr<PendingResponse> http_fetch(Infrastructure::FetchParams const&, MakeCORSPreflight make_cors_preflight = MakeCORSPreflight::No);
using RedirectHopPreparation = Variant<NonnullRefPtr<Infrastructure::Response>, Recursive>;
[[nodiscard]] RedirectHopPreparation prepare_redirect_hop(Infrastructure::FetchParams const&, Infrastructure::Response&);
WEB_API NonnullRefPtr<PendingResponse> http_redirect_fetch(Infrastructure::FetchParams const&, Infrastructure::Response&);
NonnullRefPtr<PendingResponse> http_network_or_cache_fetch(Infrastructure::FetchParams const&, IsAuthenticationFetch is_authentication_fetch = IsAuthenticationFetch::No, IsNewConnectionFetch is_new_connection_fetch = IsNewConnectionFetch::No);
NonnullRefPtr<PendingResponse> nonstandard_resource_loader_file_or_http_network_fetch(Infrastructure::FetchParams const&, HTTP::Cookie::IncludeCredentials include_credentials = HTTP::Cookie::IncludeCredentials::No, IsNewConnectionFetch is_new_connection_fetch = IsNewConnectionFetch::No, RefPtr<HTTP::MemoryCache> = {});
NonnullRefPtr<PendingResponse> cors_preflight_fetch(Infrastructure::FetchParams const&);
// Stopping the transport ends the load, and cancels the channel if it is open or still holds unread bytes.
WEB_API NonnullRefPtr<Engine::WholeBodyTransport> create_whole_body_transport(Engine::FetchBodyDeliveryGate&, Engine::FetchByteChannel&, NonnullRefPtr<Engine::FetchLoad>);
void set_sec_fetch_dest_header(Infrastructure::Request&);
void set_sec_fetch_mode_header(Infrastructure::Request&);
void set_sec_fetch_site_header(Infrastructure::Request&);
void set_sec_fetch_user_header(Infrastructure::Request&);
void append_fetch_metadata_headers_for_request(Infrastructure::Request&);
void append_user_agent_client_hints_for_request(Infrastructure::Request&);

WEB_API Utf16String network_request_failed_error_message(Optional<StringView> error_message);

WEB_API void set_http_memory_cache_enabled(bool enabled);
WEB_API bool http_memory_cache_enabled();
void clear_http_memory_cache_entries();
bool http_memory_cache_can_hold_body_in_memory(size_t);
void evict_http_memory_cache_entries_over_limits();
WEB_API Optional<Utf16String> http_cache_partition(Infrastructure::Request const&);
WEB_API void set_disk_cache_enabled_for_navigations_for_testing(bool enabled);
WEB_API bool disk_cache_enabled_for_navigations_for_testing();

}
