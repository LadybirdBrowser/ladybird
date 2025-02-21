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

void LayoutInspectorActor::handle_message(StringView type, JsonObject const&)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "getCurrentFlexbox"sv) {
        response.set("flexbox"sv, JsonValue {});
        send_message(move(response));
        return;
    }

    if (type == "getGrids"sv) {
        response.set("grids"sv, JsonArray {});
        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(type);
}

}
