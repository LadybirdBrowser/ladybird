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

void InspectorActor::handle_message(StringView type, JsonObject const& message)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "getPageStyle"sv) {
        if (!m_page_style)
            m_page_style = devtools().register_actor<PageStyleActor>();

        response.set("pageStyle"sv, m_page_style->serialize_style());
        send_message(move(response));
        return;
    }

    if (type == "getHighlighterByType"sv) {
        auto type_name = message.get_string("typeName"sv);
        if (!type_name.has_value()) {
            send_missing_parameter_error("typeName"sv);
            return;
        }

        auto highlighter = m_highlighters.ensure(*type_name, [&]() -> NonnullRefPtr<HighlighterActor> {
            return devtools().register_actor<HighlighterActor>();
        });

        response.set("highlighter"sv, highlighter->serialize_highlighter());
        send_message(move(response));
        return;
    }

    if (type == "getWalker"sv) {
        if (auto tab = m_tab.strong_ref()) {
            auto block_token = block_responses();

            devtools().delegate().inspect_tab(tab->description(),
                [weak_self = make_weak_ptr<InspectorActor>(), block_token = move(block_token)](ErrorOr<JsonValue> dom_tree) mutable {
                    if (dom_tree.is_error()) {
                        dbgln_if(DEVTOOLS_DEBUG, "Unable to retrieve DOM tree: {}", dom_tree.error());
                        return;
                    }
                    if (!WalkerActor::is_suitable_for_dom_inspection(dom_tree.value())) {
                        dbgln_if(DEVTOOLS_DEBUG, "Did not receive a suitable DOM tree: {}", dom_tree);
                        return;
                    }

                    if (auto self = weak_self.strong_ref())
                        self->received_dom_tree(move(dom_tree.release_value().as_object()), move(block_token));
                });
        }

        return;
    }

    send_unrecognized_packet_type_error(type);
}

void InspectorActor::received_dom_tree(JsonObject dom_tree, BlockToken block_token)
{
    auto& walker_actor = devtools().register_actor<WalkerActor>(m_tab, move(dom_tree));

    JsonObject walker;
    walker.set("actor"sv, walker_actor.name());
    walker.set("root"sv, walker_actor.serialize_root());

    JsonObject message;
    message.set("from"sv, name());
    message.set("walker"sv, move(walker));
    send_message(move(message), move(block_token));
}

}
