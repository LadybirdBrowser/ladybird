/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/HighlighterActor.h>
#include <LibDevTools/Actors/InspectorActor.h>
#include <LibDevTools/Actors/PageStyleActor.h>
#include <LibDevTools/Actors/StyleSheetsActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<InspectorActor> InspectorActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<StyleSheetsActor> style_sheets)
{
    return adopt_ref(*new InspectorActor(devtools, move(name), move(tab), move(style_sheets)));
}

InspectorActor::InspectorActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<StyleSheetsActor> style_sheets)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_style_sheets(move(style_sheets))
{
}

InspectorActor::~InspectorActor() = default;

void InspectorActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getPageStyle"sv) {
        if (!m_page_style)
            m_page_style = devtools().register_actor<PageStyleActor>(*this);

        response.set("pageStyle"sv, m_page_style->serialize_style());
        send_response(message, move(response));
        return;
    }

    if (message.type == "getHighlighterByType"sv) {
        auto type_name = get_required_parameter<String>(message, "typeName"sv);
        if (!type_name.has_value())
            return;

        auto& highlighter = devtools().register_actor<HighlighterActor>(*this, *type_name);

        response.set("highlighter"sv, highlighter.serialize_highlighter());
        send_response(message, move(response));
        return;
    }

    if (message.type == "getWalker"sv) {
        if (auto tab = m_tab.strong_ref()) {
            devtools().delegate().inspect_tab(tab->description(),
                async_handler<InspectorActor>(message, [](auto& self, auto dom_tree, auto& response) {
                    if (!WalkerActor::is_suitable_for_dom_inspection(dom_tree)) {
                        dbgln_if(DEVTOOLS_DEBUG, "Did not receive a suitable DOM tree: {}", dom_tree);
                        return;
                    }

                    self.received_dom_tree(response, move(dom_tree.as_object()));
                }));
        }

        return;
    }

    if (message.type == "supportsHighlighters"sv) {
        response.set("value"sv, true);
        send_response(message, move(response));
        return;
    }

    if (message.type == "resolveRelativeURL"sv) {
        auto url = get_required_parameter<String>(message, "url"sv);
        if (!url.has_value())
            return;

        Optional<Web::UniqueNodeID> node_id;
        if (auto node_actor = message.data.get_string("node"sv); node_actor.has_value()) {
            auto dom_node = WalkerActor::dom_node_for(m_walker, *node_actor);
            if (!dom_node.has_value()) {
                send_unknown_actor_error(message, *node_actor);
                return;
            }

            node_id = dom_node->identifier.id;
        }

        if (auto tab = m_tab.strong_ref()) {
            devtools().delegate().resolve_dom_node_url(tab->description(), node_id, *url,
                async_handler<InspectorActor>(message, [](auto&, auto resolved_url, auto& response) {
                    response.set("value"sv, move(resolved_url));
                }));
        } else {
            response.set("value"sv, url.release_value());
            send_response(message, move(response));
        }

        return;
    }

    send_unrecognized_packet_type_error(message);
}

void InspectorActor::received_dom_tree(JsonObject& response, JsonObject dom_tree)
{
    auto& walker_actor = devtools().register_actor<WalkerActor>(m_tab, move(dom_tree));
    m_walker = walker_actor;

    JsonObject walker;
    walker.set("actor"sv, walker_actor.name());
    walker.set("root"sv, walker_actor.serialize_root());

    response.set("walker"sv, move(walker));
}

void InspectorActor::on_navigation_started()
{
    if (auto walker = m_walker.strong_ref())
        walker->document_unloaded();
    if (auto tab = m_tab.strong_ref())
        tab->reset_selected_node();
}

void InspectorActor::on_navigation_finished()
{
    auto walker = m_walker.strong_ref();
    auto tab = m_tab.strong_ref();
    if (!walker || !tab)
        return;

    devtools().delegate().inspect_tab(tab->description(),
        weak_callback(*this, [walker](auto&, auto dom_tree) {
            if (dom_tree.is_error()) {
                dbgln_if(DEVTOOLS_DEBUG, "Error inspecting tab after navigation: {}", dom_tree.error());
                return;
            }

            auto value = dom_tree.release_value();
            if (!WalkerActor::is_suitable_for_dom_inspection(value)) {
                dbgln_if(DEVTOOLS_DEBUG, "Did not receive a suitable DOM tree: {}", value);
                return;
            }

            walker->replace_dom_tree(move(value.as_object()));
        }));
}

RefPtr<TabActor> InspectorActor::tab_for(WeakPtr<InspectorActor> const& weak_inspector)
{
    if (auto inspector = weak_inspector.strong_ref())
        return inspector->m_tab.strong_ref();
    return {};
}

RefPtr<WalkerActor> InspectorActor::walker_for(WeakPtr<InspectorActor> const& weak_inspector)
{
    if (auto inspector = weak_inspector.strong_ref())
        return inspector->m_walker.strong_ref();
    return {};
}

RefPtr<StyleSheetsActor> InspectorActor::style_sheets_for(WeakPtr<InspectorActor> const& weak_inspector)
{
    if (auto inspector = weak_inspector.strong_ref())
        return inspector->m_style_sheets.strong_ref();
    return {};
}

}
