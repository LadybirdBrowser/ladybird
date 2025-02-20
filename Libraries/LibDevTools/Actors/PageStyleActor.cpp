/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/PageStyleActor.h>

namespace DevTools {

NonnullRefPtr<PageStyleActor> PageStyleActor::create(DevToolsServer& devtools, String name)
{
    return adopt_ref(*new PageStyleActor(devtools, move(name)));
}

PageStyleActor::PageStyleActor(DevToolsServer& devtools, String name)
    : Actor(devtools, move(name))
{
}

PageStyleActor::~PageStyleActor() = default;

void PageStyleActor::handle_message(StringView type, JsonObject const&)
{
    send_unrecognized_packet_type_error(type);
}

JsonValue PageStyleActor::serialize_style() const
{
    JsonObject traits;
    traits.set("fontStyleLevel4"sv, true);
    traits.set("fontWeightLevel4"sv, true);
    traits.set("fontStretchLevel4"sv, true);
    traits.set("fontVariations"sv, true);

    JsonObject style;
    style.set("actor"sv, name());
    style.set("traits"sv, move(traits));
    return style;
}

}
