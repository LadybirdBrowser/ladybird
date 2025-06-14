/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/CSSPropertiesActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<CSSPropertiesActor> CSSPropertiesActor::create(DevToolsServer& devtools, String name)
{
    return adopt_ref(*new CSSPropertiesActor(devtools, move(name)));
}

CSSPropertiesActor::CSSPropertiesActor(DevToolsServer& devtools, String name)
    : Actor(devtools, move(name))
{
}

CSSPropertiesActor::~CSSPropertiesActor() = default;

void CSSPropertiesActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getCSSDatabase"_sv) {
        auto css_property_list = devtools().delegate().css_property_list();

        JsonObject properties;

        for (auto const& css_property : css_property_list) {
            JsonArray subproperties;
            subproperties.must_append(css_property.name);

            JsonObject property;
            property.set("isInherited"_sv, css_property.is_inherited);
            property.set("supports"_sv, JsonArray {});
            property.set("values"_sv, JsonArray {});
            property.set("subproperties"_sv, move(subproperties));

            properties.set(css_property.name, move(property));
        }

        response.set("properties"_sv, move(properties));
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
