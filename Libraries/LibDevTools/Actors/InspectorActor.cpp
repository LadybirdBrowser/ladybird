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
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<InspectorActor> InspectorActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new InspectorActor(devtools, move(name), move(tab)));
}

InspectorActor::InspectorActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
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

        auto highlighter = m_highlighters.ensure(*type_name, [&]() -> NonnullRefPtr<HighlighterActor> {
            return devtools().register_actor<HighlighterActor>(*this);
        });

        response.set("highlighter"sv, highlighter->serialize_highlighter());
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

}
