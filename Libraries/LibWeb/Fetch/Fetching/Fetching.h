/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/RefPtr.h>
#include <LibCore/ImmutableBytes.h>
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>
#include <LibHTTP/Cookie/IncludeCredentials.h>
#include <LibHTTP/Forward.h>
#include <LibJS/Forward.h>
#include <LibRequests/RequestTransferLease.h>
#include <LibURL/Forward.h>
#include <LibWeb/ContentSecurityPolicy/FetchIntegration.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Engine/Ids.h>
#include <LibWeb/Fetch/Infrastructure/NetworkPartitionKey.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/FetchClient.h>

namespace Web::Fetch::Fetching {

// https://fetch.spec.whatwg.org/#document-accept-header-value
// The document `Accept` header value is `text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8`.
constexpr auto document_accept_header_value = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"sv;

// https://fetch.spec.whatwg.org/#http-network-or-cache-fetch
// If the sum of contentLength and inflightKeepaliveBytes is greater than 64 kibibytes, then return a network error.
constexpr auto keepalive_maximum_size = 64 * KiB;

#define ENUMERATE_BOOL_PARAMS                               \
    __ENUMERATE_BOOL_PARAM(IsAuthenticationFetch)           \
    __ENUMERATE_BOOL_PARAM(IsNewConnectionFetch)            \
    __ENUMERATE_BOOL_PARAM(MakeCORSPreflight)               \
    __ENUMERATE_BOOL_PARAM(Recursive)                       \
    __ENUMERATE_BOOL_PARAM(CreateResponseBodyTransferLease) \
    __ENUMERATE_BOOL_PARAM(UseParallelQueue)

#define __ENUMERATE_BOOL_PARAM(Name) \
    enum class Name {                \
        Yes,                         \
        No,                          \
    };
ENUMERATE_BOOL_PARAMS
#undef __ENUMERATE_BOOL_PARAM

WEB_API GC::Ref<Infrastructure::FetchController> fetch(JS::Realm&, Infrastructure::Request&, Infrastructure::FetchAlgorithms const&, UseParallelQueue use_parallel_queue = UseParallelQueue::No, CreateResponseBodyTransferLease = CreateResponseBodyTransferLease::No);
void append_accept_language_header_if_missing(Infrastructure::Request&, LoaderConfig const&);
// The controller of the fetch the fetch params are for, on the agent that started the fetch, while the fetch runs.
[[nodiscard]] WEB_API GC::Ref<Infrastructure::FetchController> fetch_controller_of(Infrastructure::FetchParams const&);
RefPtr<PendingResponse> main_fetch(Infrastructure::FetchParams const&, ContentSecurityPolicy::ViolationReporter const&, Engine::KnownHSTSHostProvider&, Recursive recursive = Recursive::No);
[[nodiscard]] Optional<String> host_to_match_against_known_hsts_hosts(Infrastructure::Request const&);
void upgrade_request_to_https(Infrastructure::Request&);
void run_main_fetch_parallel_steps(Engine::MainFetchInput, NonnullRefPtr<Engine::FetchEngineRecord>);
void populate_request_from_client(Infrastructure::Request&, Optional<HTML::FetchClient> const& client);
void fetch_response_handover(Infrastructure::FetchParams const&, Infrastructure::Response&);
// Runs the steps of fetch response handover that follow the response's delivery to the agent that started the fetch.
void finish_fetch_response_handover(GC::Ref<Infrastructure::FetchController>, Engine::Event::ResponseDelivery);
NonnullRefPtr<PendingResponse> scheme_fetch(Infrastructure::FetchParams const&);
NonnullRefPtr<PendingResponse> http_fetch(Infrastructure::FetchParams const&, MakeCORSPreflight make_cors_preflight = MakeCORSPreflight::No);
WEB_API RefPtr<PendingResponse> http_redirect_fetch(Infrastructure::FetchParams const&, Infrastructure::Response&);
NonnullRefPtr<PendingResponse> http_network_or_cache_fetch(Infrastructure::FetchParams const&, IsAuthenticationFetch is_authentication_fetch = IsAuthenticationFetch::No, IsNewConnectionFetch is_new_connection_fetch = IsNewConnectionFetch::No);
NonnullRefPtr<PendingResponse> nonstandard_resource_loader_file_or_http_network_fetch(Infrastructure::FetchParams const&, HTTP::Cookie::IncludeCredentials include_credentials = HTTP::Cookie::IncludeCredentials::No, IsNewConnectionFetch is_new_connection_fetch = IsNewConnectionFetch::No, RefPtr<HTTP::MemoryCache> = {});
NonnullRefPtr<PendingResponse> cors_preflight_fetch(Infrastructure::FetchParams const&);
// on_network_settled runs at most once: when the network completes the body, or with an error when the body fails
// or is stopped.
WEB_API GC::Ptr<Infrastructure::Body> adopt_navigation_response_body(JS::Realm&, Infrastructure::Response&, Optional<Utf16String> const& request_server_top_level_site, Requests::RequestTransferLeaseKey, RefPtr<Infrastructure::FetchTimingInfo>, GC::Ref<GC::Function<void(Optional<StringView> error)>> on_network_settled);
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
WEB_API void clear_http_memory_cache();
bool http_memory_cache_can_hold_body_in_memory(size_t);
void evict_http_memory_cache_entries_over_limits();
WEB_API Optional<Utf16String> http_cache_partition(Infrastructure::Request const&);
WEB_API void set_disk_cache_enabled_for_navigations_for_testing(bool enabled);
WEB_API bool disk_cache_enabled_for_navigations_for_testing();

}
