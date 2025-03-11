/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/HighlighterActor.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<HighlighterActor> HighlighterActor::create(DevToolsServer& devtools, String name, WeakPtr<InspectorActor> inspector)
{
    return adopt_ref(*new HighlighterActor(devtools, move(name), move(inspector)));
}

HighlighterActor::HighlighterActor(DevToolsServer& devtools, String name, WeakPtr<InspectorActor> inspector)
    : Actor(devtools, move(name))
    , m_inspector(move(inspector))
{
}

HighlighterActor::~HighlighterActor() = default;

void HighlighterActor::handle_message(StringView type, JsonObject const& message)
{
    JsonObject response;

    if (type == "show"sv) {
        auto node = get_required_parameter<String>(message, "node"sv);
        if (!node.has_value())
            return;

        response.set("value"sv, false);

        if (auto dom_node = WalkerActor::dom_node_for(InspectorActor::walker_for(m_inspector), *node); dom_node.has_value()) {
            devtools().delegate().highlight_dom_node(dom_node->tab->description(), dom_node->identifier.id, dom_node->identifier.pseudo_element);
            response.set("value"sv, true);
        }

        send_message(move(response));
        return;
    }

    if (type == "hide"sv) {
        if (auto tab = InspectorActor::tab_for(m_inspector))
            devtools().delegate().clear_highlighted_dom_node(tab->description());

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
