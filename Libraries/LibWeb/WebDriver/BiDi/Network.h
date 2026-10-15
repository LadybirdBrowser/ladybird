/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibGC/Forward.h>
#include <LibGC/Ptr.h>
#include <LibHTTP/HeaderList.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWebCommon/HTML/CrossProcessId.h>
#include <LibWebCommon/WebDriver/Response.h>

namespace Web::WebDriver::BiDi {

// NB: How Fetch proceeds with a blocked request once a command or the end of the fetch resumes it.
enum class NetworkContinuationAction : u8 {
    Continue,
    ContinueWithModifiedRequest,
    ProvideResponse,
    Fail,
    UseDefaultAuthentication,
    CancelAuthentication,
    ProvideCredentials,
    RetryAuthentication,
};

// NB: Drop a resumed interception's captured fetch state immediately, rather than waiting for collection.
class NetworkContinuation final : public JS::Cell {
    GC_CELL(NetworkContinuation, JS::Cell);
    GC_DECLARE_ALLOCATOR(NetworkContinuation);

public:
    using Callback = Function<void(NetworkContinuationAction, RefPtr<Fetch::Infrastructure::Response>)>;
    static GC::Ref<NetworkContinuation> create(GC::Heap&, Callback);
    void invoke(NetworkContinuationAction, RefPtr<Fetch::Infrastructure::Response>);
    void clear_blocked_state();

    String request_id;
    String phase;
    JsonArray intercepts;
    RefPtr<Fetch::Infrastructure::FetchParams const> fetch_params;
    GC::Ptr<Fetch::Infrastructure::FetchController> controller;
    RefPtr<Fetch::Infrastructure::Request> original_request;
    RefPtr<Fetch::Infrastructure::Response> response;

private:
    explicit NetworkContinuation(Callback);
    virtual void finalize() override;
    virtual void visit_edges(JS::Cell::Visitor&) override;
    Callback m_callback;
};

// https://w3c.github.io/webdriver-bidi/#request-id
// NB: Fetch's internal request clones and redirects share this identity.
class NetworkRequest final : public RefCounted<NetworkRequest> {
    AK_ALLOC_WITH_KMALLOC;

public:
    static NonnullRefPtr<NetworkRequest> create() { return adopt_ref(*new NetworkRequest); }

    String id;
    // The navigable the request's events go to.
    Optional<HTML::CrossProcessId> navigable_id;
    Optional<String> navigation;
    HashTable<u8> before_request_sent;
    HashTable<u8> response_started;
    HashTable<u8> completed;
    HashTable<u8> transferred;
    HashMap<u8, JsonObject> request_data;
    // Explicit overrides for headers Fetch appends during preparation. Keep them out of the unprepared list.
    RefPtr<HTTP::HeaderList> request_header_overrides;

private:
    NetworkRequest();
};

// NB: Fetch checks this cached flag before doing any network instrumentation.
WEB_API extern bool g_network_events_enabled;
WEB_API extern bool g_network_configuration_enabled;

void network_session_state_changed();
void cancel_network_interception(Fetch::Infrastructure::FetchParams const&);
void apply_network_cache_behavior(Fetch::Infrastructure::Request&);
void apply_network_extra_headers(Fetch::Infrastructure::Request&);
void apply_network_request_header_overrides(Fetch::Infrastructure::Request&);
JsonArray get_network_intercepts(Fetch::Infrastructure::FetchParams const&, StringView phase);
void block_network_request(Fetch::Infrastructure::FetchParams const&, RefPtr<Fetch::Infrastructure::Response>, String phase, JsonArray intercepts, GC::Ref<NetworkContinuation>, RefPtr<Fetch::Infrastructure::Request> original_request = nullptr);
WEB_API Response continue_network_request(StringView method, JsonObject const&);

void initialize_network_request(Fetch::Infrastructure::Request&, GC::Ptr<HTML::Navigable> = nullptr, Optional<Utf16String> const& navigation = {});
void synchronize_network_request(Fetch::Infrastructure::Request& original, Fetch::Infrastructure::Request& prepared);
void network_before_request_sent(Fetch::Infrastructure::FetchParams const&);
void network_response_started(Fetch::Infrastructure::FetchParams const&, Fetch::Infrastructure::Response&);
void network_auth_required(Fetch::Infrastructure::FetchParams const&, Fetch::Infrastructure::Response const&);
void network_response_completed(Fetch::Infrastructure::FetchParams const&, Fetch::Infrastructure::Response const&);
void network_fetch_error(Fetch::Infrastructure::FetchParams const&, StringView);

Optional<JsonObject> network_response_for_navigation(Fetch::Infrastructure::Request const&, Fetch::Infrastructure::FetchTimingInfo const&,
    Fetch::Infrastructure::Response const&);
void network_navigation_response_finished(HTML::Navigable const&, JsonObject const&, Fetch::Infrastructure::Response const&, Fetch::Infrastructure::FetchTimingInfo const*, Optional<StringView> error = {});

}
