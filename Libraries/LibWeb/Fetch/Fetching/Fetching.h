/*
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::Fetch::Fetching {

// https://fetch.spec.whatwg.org/#document-accept-header-value
// The document `Accept` header value is `text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8`.
constexpr auto document_accept_header_value = "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"sv;

// https://fetch.spec.whatwg.org/#http-network-or-cache-fetch
// If the sum of contentLength and inflightKeepaliveBytes is greater than 64 kibibytes, then return a network error.
constexpr auto keepalive_maximum_size = 64 * KiB;

#define ENUMERATE_BOOL_PARAMS                     \
    __ENUMERATE_BOOL_PARAM(IncludeCredentials)    \
    __ENUMERATE_BOOL_PARAM(IsAuthenticationFetch) \
    __ENUMERATE_BOOL_PARAM(IsNewConnectionFetch)  \
    __ENUMERATE_BOOL_PARAM(MakeCORSPreflight)     \
    __ENUMERATE_BOOL_PARAM(Recursive)             \
    __ENUMERATE_BOOL_PARAM(UseParallelQueue)

#define __ENUMERATE_BOOL_PARAM(Name) \
    enum class Name {                \
        Yes,                         \
        No,                          \
    };
ENUMERATE_BOOL_PARAMS
#undef __ENUMERATE_BOOL_PARAM

WebIDL::ExceptionOr<GC::Ref<Infrastructure::FetchController>> fetch(JS::Realm&, Infrastructure::Request&, Infrastructure::FetchAlgorithms const&, UseParallelQueue use_parallel_queue = UseParallelQueue::No);
WebIDL::ExceptionOr<GC::Ptr<PendingResponse>> main_fetch(JS::Realm&, Infrastructure::FetchParams const&, Recursive recursive = Recursive::No);
void fetch_response_handover(JS::Realm&, Infrastructure::FetchParams const&, Infrastructure::Response&);
WebIDL::ExceptionOr<GC::Ref<PendingResponse>> scheme_fetch(JS::Realm&, Infrastructure::FetchParams const&);
WebIDL::ExceptionOr<GC::Ref<PendingResponse>> http_fetch(JS::Realm&, Infrastructure::FetchParams const&, MakeCORSPreflight make_cors_preflight = MakeCORSPreflight::No);
WebIDL::ExceptionOr<GC::Ptr<PendingResponse>> http_redirect_fetch(JS::Realm&, Infrastructure::FetchParams const&, Infrastructure::Response&);
WebIDL::ExceptionOr<GC::Ref<PendingResponse>> http_network_or_cache_fetch(JS::Realm&, Infrastructure::FetchParams const&, IsAuthenticationFetch is_authentication_fetch = IsAuthenticationFetch::No, IsNewConnectionFetch is_new_connection_fetch = IsNewConnectionFetch::No);
WebIDL::ExceptionOr<GC::Ref<PendingResponse>> nonstandard_resource_loader_file_or_http_network_fetch(JS::Realm&, Infrastructure::FetchParams const&, IncludeCredentials include_credentials = IncludeCredentials::No, IsNewConnectionFetch is_new_connection_fetch = IsNewConnectionFetch::No);
WebIDL::ExceptionOr<GC::Ref<PendingResponse>> cors_preflight_fetch(JS::Realm&, Infrastructure::Request&);
void set_sec_fetch_dest_header(Infrastructure::Request&);
void set_sec_fetch_mode_header(Infrastructure::Request&);
void set_sec_fetch_site_header(Infrastructure::Request&);
void set_sec_fetch_user_header(Infrastructure::Request&);
void append_fetch_metadata_headers_for_request(Infrastructure::Request&);
}
