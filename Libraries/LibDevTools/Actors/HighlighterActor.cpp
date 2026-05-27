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

NonnullRefPtr<HighlighterActor> HighlighterActor::create(DevToolsServer& devtools, String name, WeakPtr<InspectorActor> inspector, String type_name)
{
    return adopt_ref(*new HighlighterActor(devtools, move(name), move(inspector), move(type_name)));
}

HighlighterActor::HighlighterActor(DevToolsServer& devtools, String name, WeakPtr<InspectorActor> inspector, String type_name)
    : Actor(devtools, move(name))
    , m_inspector(move(inspector))
    , m_type_name(move(type_name))
{
}

HighlighterActor::~HighlighterActor() = default;

void HighlighterActor::clear_current_highlight()
{
    if (auto tab = InspectorActor::tab_for(m_inspector)) {
        if (m_type_name == "FlexboxHighlighter"sv) {
            if (m_highlighted_flexbox_node_id.has_value())
                devtools().delegate().clear_flexbox_highlight(tab->description(), *m_highlighted_flexbox_node_id);
        } else if (m_type_name == "CssGridHighlighter"sv) {
            if (m_highlighted_grid_node_id.has_value())
                devtools().delegate().clear_grid_highlight(tab->description(), *m_highlighted_grid_node_id);
        } else if (m_is_highlighting_dom_node) {
            devtools().delegate().clear_highlighted_dom_node(tab->description());
        }
    }

    m_highlighted_flexbox_node_id = {};
    m_highlighted_grid_node_id = {};
    m_is_highlighting_dom_node = false;
}

void HighlighterActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "show"sv) {
        auto node = get_required_parameter<String>(message, "node"sv);
        if (!node.has_value())
            return;

        response.set("value"sv, false);

        if (auto dom_node = WalkerActor::dom_node_for(InspectorActor::walker_for(m_inspector), *node); dom_node.has_value()) {
            if (m_type_name == "FlexboxHighlighter"sv) {
                if (m_highlighted_flexbox_node_id.has_value() && *m_highlighted_flexbox_node_id != dom_node->identifier.id)
                    devtools().delegate().clear_flexbox_highlight(dom_node->tab->description(), *m_highlighted_flexbox_node_id);

                auto options = message.data.get("options"sv).value_or(JsonObject {});
                devtools().delegate().highlight_flexbox(dom_node->tab->description(), dom_node->identifier.id, move(options));
                m_highlighted_flexbox_node_id = dom_node->identifier.id;
            } else if (m_type_name == "CssGridHighlighter"sv) {
                if (m_highlighted_grid_node_id.has_value() && *m_highlighted_grid_node_id != dom_node->identifier.id)
                    devtools().delegate().clear_grid_highlight(dom_node->tab->description(), *m_highlighted_grid_node_id);

                auto options = message.data.get("options"sv).value_or(JsonObject {});
                devtools().delegate().highlight_grid(dom_node->tab->description(), dom_node->identifier.id, move(options));
                m_highlighted_grid_node_id = dom_node->identifier.id;
            } else {
                devtools().delegate().highlight_dom_node(dom_node->tab->description(), dom_node->identifier.id, dom_node->identifier.pseudo_element);
                m_is_highlighting_dom_node = true;
            }
            response.set("value"sv, true);
        }

        send_response(message, move(response));
        return;
    }

    if (message.type == "hide"sv) {
        clear_current_highlight();

        send_response(message, move(response));
        return;
    }

    if (message.type == "release"sv) {
        clear_current_highlight();

        send_response(message, move(response));
        devtools().unregister_actor(name());
        return;
    }

    if (message.type == "finalize"sv) {
        clear_current_highlight();
        devtools().unregister_actor(name());
        return;
    }

    send_unrecognized_packet_type_error(message);
}

JsonValue HighlighterActor::serialize_highlighter() const
{
    JsonObject highlighter;
    highlighter.set("actor"sv, name());
    return highlighter;
}

}
