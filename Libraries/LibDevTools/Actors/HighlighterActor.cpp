/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/HighlighterActor.h>

namespace DevTools {

NonnullRefPtr<HighlighterActor> HighlighterActor::create(DevToolsServer& devtools, ByteString name)
{
    return adopt_ref(*new HighlighterActor(devtools, move(name)));
}

HighlighterActor::HighlighterActor(DevToolsServer& devtools, ByteString name)
    : Actor(devtools, move(name))
{
}

HighlighterActor::~HighlighterActor() = default;

void HighlighterActor::handle_message(StringView type, JsonObject const&)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "show"sv) {
        response.set("value"sv, true);
        send_message(move(response));
        return;
    }

    if (type == "hide"sv) {
        send_message(move(response));
        return;
    }

    send_unrecognized_packet_type_error(type);
}

JsonValue HighlighterActor::serialize_highlighter() const
{
    JsonObject highlighter;
    highlighter.set("actor"sv, name());
    return highlighter;
}

}
