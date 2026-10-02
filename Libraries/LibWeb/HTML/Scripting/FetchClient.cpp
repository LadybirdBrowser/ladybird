/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/FetchClient.h>

namespace Web::HTML {

URL::Origin origin_of_fetch_client(FetchClient const& fetch_client)
{
    return fetch_client.visit(
        [](GC::Ref<EnvironmentSettingsObject> const& settings_object) { return settings_object->origin(); },
        [](NonnullRefPtr<RemoteEnvironmentSettings const> const& settings) { return settings->settings.origin; });
}

GC::Ptr<EnvironmentSettingsObject> settings_object_of_fetch_client(Optional<FetchClient> const& fetch_client)
{
    if (!fetch_client.has_value())
        return {};
    if (auto const* settings_object = fetch_client->get_pointer<GC::Ref<EnvironmentSettingsObject>>())
        return *settings_object;
    return {};
}

}
