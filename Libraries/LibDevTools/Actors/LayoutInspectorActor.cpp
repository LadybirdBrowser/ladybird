/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/LayoutInspectorActor.h>

namespace DevTools {

NonnullRefPtr<LayoutInspectorActor> LayoutInspectorActor::create(DevToolsServer& devtools, String name)
{
    return adopt_ref(*new LayoutInspectorActor(devtools, move(name)));
}

LayoutInspectorActor::LayoutInspectorActor(DevToolsServer& devtools, String name)
    : Actor(devtools, move(name))
{
}

LayoutInspectorActor::~LayoutInspectorActor() = default;

void LayoutInspectorActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getCurrentFlexbox"sv) {
        response.set("flexbox"sv, JsonValue {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "getGrids"sv) {
        response.set("grids"sv, JsonArray {});
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
