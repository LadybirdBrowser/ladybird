/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Utf16String.h>
#include <LibGC/Weak.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::WebDriver::BiDi {

// https://w3c.github.io/webdriver-bidi/#request-id
// NB: Fetch's internal request clones and redirects share this identity.
class NetworkRequest final : public JS::Cell {
    GC_CELL(NetworkRequest, JS::Cell);
    GC_DECLARE_ALLOCATOR(NetworkRequest);

public:
    String id;
    // NB: An observed request must not keep a navigable or its load-event delayer alive.
    GC::Weak<HTML::Navigable> navigable;
    Optional<String> navigation;
    HashTable<u8> before_request_sent;
    HashTable<u8> response_started;
    HashTable<u8> completed;
    HashTable<u8> transferred;
    HashMap<u8, JsonObject> request_data;

private:
    NetworkRequest();
};

// NB: Fetch checks this cached flag before doing any network instrumentation.
WEB_API extern bool g_network_events_enabled;

void initialize_network_request(Fetch::Infrastructure::Request&, GC::Ptr<HTML::Navigable> = nullptr, Optional<Utf16String> const& navigation = {});
void network_before_request_sent(Fetch::Infrastructure::FetchParams const&);
void network_response_started(Fetch::Infrastructure::FetchParams const&, Fetch::Infrastructure::Response&);
void network_response_completed(Fetch::Infrastructure::FetchParams const&, Fetch::Infrastructure::Response const&);
void network_fetch_error(Fetch::Infrastructure::FetchParams const&, StringView);

Optional<JsonObject> network_response_for_navigation(Fetch::Infrastructure::Request const&, Fetch::Infrastructure::FetchTimingInfo const&,
    Fetch::Infrastructure::Response const&);
void network_navigation_response_finished(HTML::Navigable const&, JsonObject const&, Fetch::Infrastructure::Response const&, Fetch::Infrastructure::FetchTimingInfo const*, Optional<StringView> error = {});

}
