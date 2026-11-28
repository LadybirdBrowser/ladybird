/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/HTTP.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWebCommon/Loader/LoaderConfigSnapshot.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#default-user-agent-value
String default_user_agent_value(LoaderConfig const& loader_config, URL::Origin const& top_level_origin, URL::Origin const& origin)
{
    // A default `User-Agent` value is an implementation-defined header value for the `User-Agent` header.
    return loader_config.user_agent_for(top_level_origin, origin);
}

// The top-level origin is the environment's, or that of its top-level creation URL.
String default_user_agent_value(LoaderConfig const& loader_config, HTML::SerializedEnvironment const& environment, URL::Origin const& origin)
{
    auto top_level_origin = environment.top_level_origin;
    if (!top_level_origin.has_value() && environment.top_level_creation_url.has_value())
        top_level_origin = environment.top_level_creation_url->origin();
    return default_user_agent_value(loader_config, top_level_origin.value_or(origin), origin);
}

String default_user_agent_value(LoaderConfig const& loader_config, Request const& request)
{
    auto current_url_origin = request.current_url().origin();

    if (request.mode() == Request::Mode::Navigate) {
        // A top-level navigation request is for the top-level document it creates, whose site is that of the current
        // URL, even after a cross-site redirect.
        if (request.destination() == Request::Destination::Document)
            return default_user_agent_value(loader_config, current_url_origin, current_url_origin);

        // Any other navigation request is for a document under the top-level of its reserved client.
        if (auto const& reserved_client = request.reserved_client_snapshot())
            return default_user_agent_value(loader_config, reserved_client->environment, current_url_origin);
        return default_user_agent_value(loader_config, current_url_origin, current_url_origin);
    }

    if (auto const& client = request.client_snapshot())
        return default_user_agent_value(loader_config, client->environment, client->origin);

    return default_user_agent_value(loader_config, current_url_origin, current_url_origin);
}

}
