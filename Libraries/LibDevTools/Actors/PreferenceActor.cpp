/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/PreferenceActor.h>

namespace DevTools {

NonnullRefPtr<PreferenceActor> PreferenceActor::create(DevToolsServer& devtools, String name)
{
    return adopt_ref(*new PreferenceActor(devtools, move(name)));
}

PreferenceActor::PreferenceActor(DevToolsServer& devtools, String name)
    : Actor(devtools, move(name))
{
}

PreferenceActor::~PreferenceActor() = default;

void PreferenceActor::handle_message(Message const& message)
{
    // FIXME: During session initialization, Firefox DevTools asks for the following boolean configurations:
    //            browser.privatebrowsing.autostart
    //            devtools.debugger.prompt-connection
    //            dom.serviceWorkers.enabled
    //        We just blindly return `false` for these, but we will eventually want a real configuration manager.
    if (message.type == "getBoolPref"sv) {
        JsonObject response;
        response.set("value"sv, false);
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
