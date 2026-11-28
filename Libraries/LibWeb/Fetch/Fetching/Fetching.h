/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/RefPtr.h>
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibRequests/RequestTransferLease.h>
#include <LibWeb/Export.h>
#include <LibWeb/Fetch/Fetching/EngineFetching.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Scripting/FetchClient.h>

namespace Web::Fetch::Fetching {

enum class UseParallelQueue {
    Yes,
    No,
};

enum class CreateResponseBodyTransferLease {
    Yes,
    No,
};

WEB_API GC::Ref<Infrastructure::FetchController> fetch(JS::Realm&, Infrastructure::Request&, Infrastructure::FetchAlgorithms const&, UseParallelQueue use_parallel_queue = UseParallelQueue::No, CreateResponseBodyTransferLease = CreateResponseBodyTransferLease::No);
void append_accept_language_header_if_missing(Infrastructure::Request&, LoaderConfig const&);
// The controller of the fetch the fetch params are for, on the agent that started the fetch, while the fetch runs.
[[nodiscard]] WEB_API GC::Ref<Infrastructure::FetchController> fetch_controller_of(Infrastructure::FetchParams const&);
void main_fetch(Infrastructure::FetchParams const&, ContentSecurityPolicy::ViolationReporter const&, Engine::KnownHSTSHostProvider&);
void populate_request_from_client(Infrastructure::Request&, Optional<HTML::FetchClient> const& client);
// Runs the steps of fetch response handover that follow the response's delivery to the agent that started the fetch.
void finish_fetch_response_handover(GC::Ref<Infrastructure::FetchController>, Engine::Event::ResponseDelivery);
// on_network_settled runs at most once: when the network completes the body, or with an error when the body fails
// or is stopped.
WEB_API GC::Ptr<Infrastructure::Body> adopt_navigation_response_body(JS::Realm&, Infrastructure::Response&, Optional<Utf16String> const& request_server_top_level_site, Requests::RequestTransferLeaseKey, RefPtr<Infrastructure::FetchTimingInfo>, GC::Ref<GC::Function<void(Optional<StringView> error)>> on_network_settled);
WEB_API void clear_http_memory_cache();

}
