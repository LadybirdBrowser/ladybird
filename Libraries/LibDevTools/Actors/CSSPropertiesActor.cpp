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

void CSSPropertiesActor::handle_message(StringView type, JsonObject const&)
{
    JsonObject response;

    if (type == "getCSSDatabase"sv) {
        auto css_property_list = devtools().delegate().css_property_list();

        JsonObject properties;

        for (auto const& css_property : css_property_list) {
            JsonArray subproperties;
            subproperties.must_append(css_property.name);

            JsonObject property;
            property.set("isInherited"sv, css_property.is_inherited);
            property.set("supports"sv, JsonArray {});
            property.set("values"sv, JsonArray {});
            property.set("subproperties"sv, move(subproperties));

            properties.set(css_property.name, move(property));
        }

        response.set("properties"sv, move(properties));
        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(type);
}

}
