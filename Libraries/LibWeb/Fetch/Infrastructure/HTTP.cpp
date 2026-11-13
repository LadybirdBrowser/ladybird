/*
 * Copyright (c) 2022, Luke Wilde <lukew@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Fetch/Infrastructure/HTTP.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWebCommon/Loader/LoaderConfigSnapshot.h>

namespace Web::Fetch::Infrastructure {

// https://fetch.spec.whatwg.org/#default-user-agent-value
String default_user_agent_value(LoaderConfig const& loader_config, URL::Origin const& top_level_origin, URL::Origin const& origin)
{
    // A default `User-Agent` value is an implementation-defined header value for the `User-Agent` header.
    return loader_config.user_agent_for(top_level_origin, origin);
}

// The top-level origin of an environment, or the origin of its top-level creation URL, as the network partition key
// takes it.
template<typename EnvironmentType>
static Optional<URL::Origin> top_level_origin_of(EnvironmentType const& environment)
{
    if (environment.top_level_origin.has_value())
        return environment.top_level_origin;
    if (environment.top_level_creation_url.has_value())
        return environment.top_level_creation_url->origin();
    return {};
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
        Optional<URL::Origin> top_level_origin;
        if (auto const& reserved_client = request.reserved_client_snapshot())
            top_level_origin = top_level_origin_of(*reserved_client);
        return default_user_agent_value(loader_config, top_level_origin.value_or(current_url_origin), current_url_origin);
    }

    if (auto const& client = request.client_snapshot()) {
        auto top_level_origin = top_level_origin_of(*client);
        return default_user_agent_value(loader_config, top_level_origin.value_or(client->origin), client->origin);
    }

    return default_user_agent_value(loader_config, current_url_origin, current_url_origin);
}

// https://fetch.spec.whatwg.org/#environment-default-user-agent-value
String environment_default_user_agent_value(LoaderConfig const& loader_config, HTML::EnvironmentSettingsObject const& environment)
{
    // FIXME: Honor WebDriver BiDi emulated User-Agent.
    auto origin = environment.origin();
    auto top_level_origin = top_level_origin_of(environment);
    return default_user_agent_value(loader_config, top_level_origin.value_or(origin), origin);
}

}
