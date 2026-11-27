/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Engine/FetchEngineRecord.h>
#include <LibWeb/Fetch/Fetching/NetworkObserverHooks.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>

namespace Web::Fetch::Fetching {

static Optional<HTML::CrossProcessId> top_level_traversable_for_network_configuration(Infrastructure::Request const& request)
{
    auto const* traversable_for_user_prompts = request.traversable_for_user_prompts().get_pointer<HTML::CrossProcessId>();
    // AD-HOC: For a navigation request, use the target navigable's top-level traversable.
    if (request.observed_navigation().has_value() && traversable_for_user_prompts)
        return *traversable_for_user_prompts;
    if (auto const& client = request.client_snapshot(); client && client->top_level_traversable_id.has_value())
        return client->top_level_traversable_id;
    if (traversable_for_user_prompts)
        return *traversable_for_user_prompts;
    return {};
}

// https://w3c.github.io/webdriver-bidi/#webdriver-bidi-cache-behavior
static bool webdriver_bidi_cache_behavior_is_bypass(Engine::NetworkConfiguration const& configuration, Infrastructure::Request const& request)
{
    // 1. Let navigable be null.
    // 2. If request's client is an environment settings object:
    //     1. Let environment settings be request's client.
    //     2. If there is a navigable whose active window is environment settings' global object, set navigable to
    //        that navigable's top-level traversable.
    auto navigable = top_level_traversable_for_network_configuration(request);

    // 3. If navigable is not null and navigable cache behavior map contains navigable, return navigable cache behavior
    //    map[navigable].
    if (navigable.has_value()) {
        if (auto bypass = configuration.bypass_cache_by_traversable.get(Engine::NetworkConfiguration::traversable_name(*navigable)); bypass.has_value())
            return *bypass;
    }

    // 4. Return default cache behavior.
    return configuration.bypass_cache_by_default;
}

bool webdriver_bidi_bypasses_cache(Infrastructure::FetchParams const& fetch_params)
{
    auto const* engine_record = fetch_params.engine_record();
    VERIFY(engine_record);
    auto configuration = engine_record->network_activity_observers().network_configuration();
    return configuration && webdriver_bidi_cache_behavior_is_bypass(*configuration, fetch_params.request());
}

// https://w3c.github.io/webdriver-bidi/#update-headers
static void update_headers(Infrastructure::Request& request, Vector<HTTP::Header> const& headers)
{
    // 1. Let request headers be request's header list.
    auto request_headers = request.header_list();

    // 2. For each header in headers:
    for (auto const& header : headers) {
        // 1. Set header in request headers.
        // NOTE: This always overwrites the existing value, if any. In particular it doesn't append cookies to an
        //       existing `Set-Cookie` header.
        request_headers->set(header);
    }
}

// https://w3c.github.io/webdriver-bidi/#update-request-headers
void update_request_headers_for_webdriver_bidi(Infrastructure::FetchParams const& fetch_params)
{
    auto const* engine_record = fetch_params.engine_record();
    VERIFY(engine_record);
    auto configuration = engine_record->network_activity_observers().network_configuration();
    if (!configuration)
        return;
    auto request = fetch_params.request();

    auto top_level_traversable = top_level_traversable_for_network_configuration(request);

    // 1. Assert: related navigables's size is 0 or 1.
    // 2. Update headers with request and session's extra headers' default headers
    update_headers(request, configuration->default_headers);

    // 3. Let user context headers be session's extra headers' user context headers.
    // 4. For navigable in related navigables:
    if (top_level_traversable.has_value()) {
        // 1. Let user context be navigable's associated user context.
        // 2. If user context headers contains user context then update headers with request and user context
        //    headers[user context]
        update_headers(request, configuration->default_user_context_headers);
    }

    // 5. Let navigable headers be session's extra headers' navigable headers.
    // 6. For navigable in related navigables:
    if (top_level_traversable.has_value()) {
        // 1. Let top-level traversable be navigable's top-level traversable.
        // 2. If navigable headers contains top-level traversable update headers with request and navigable
        //    headers[top-level traversable].
        if (auto headers = configuration->traversable_headers.get(Engine::NetworkConfiguration::traversable_name(*top_level_traversable)); headers.has_value())
            update_headers(request, *headers);
    }
}

}
