/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDevTools/Actors/AccessibilityActor.h>
#include <LibDevTools/Actors/AccessibilityWalkerActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<AccessibilityActor> AccessibilityActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
{
    return adopt_ref(*new AccessibilityActor(devtools, move(name), move(tab)));
}

AccessibilityActor::AccessibilityActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
{
}

AccessibilityActor::~AccessibilityActor() = default;

void AccessibilityActor::enable()
{
    if (m_enabled)
        return;

    m_enabled = true;

    JsonObject init_event;
    init_event.set("type"sv, "init"sv);
    send_message(move(init_event));
}

void AccessibilityActor::handle_message(Message const& message)
{
    if (message.type == "bootstrap"sv) {
        JsonObject bootstrap;
        bootstrap.set("enabled"sv, m_enabled);

        JsonObject response;
        response.set("state"sv, move(bootstrap));
        send_response(message, move(response));
        return;
    }

    if (message.type == "getSimulator"sv) {
        // FIXME: This would return a SimulatorActor for applying visual filters over the whole viewport.
        //        For now, return null.
        JsonObject response;
        response.set("simulator"sv, JsonValue {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "getTraits"sv) {
        JsonObject traits;
        traits.set("tabbingOrder"sv, true);

        JsonObject response;
        response.set("traits"sv, move(traits));
        send_response(message, move(response));
        return;
    }

    if (message.type == "getWalker"sv) {
        if (auto tab = m_tab.strong_ref()) {
            devtools().delegate().inspect_accessibility_tree(tab->description(),
                async_handler<AccessibilityActor>(message, [](auto& self, auto accessibility_tree, auto& response) {
                    if (!AccessibilityWalkerActor::is_suitable_for_accessibility_inspection(accessibility_tree)) {
                        dbgln_if(DEVTOOLS_DEBUG, "Did not receive a suitable accessibility tree: {}", accessibility_tree);
                        return;
                    }

                    self.received_accessibility_tree(response, move(accessibility_tree.as_object()));
                }));
        }
        return;
    }

    send_unrecognized_packet_type_error(message);
}

void AccessibilityActor::received_accessibility_tree(JsonObject& response, JsonObject accessibility_tree)
{
    auto& walker_actor = devtools().register_actor<AccessibilityWalkerActor>(m_tab, move(accessibility_tree));
    m_walker = walker_actor;

    JsonObject walker;
    walker.set("actor"sv, walker_actor.name());

    response.set("walker"sv, move(walker));
}

RefPtr<TabActor> AccessibilityActor::tab_for(WeakPtr<AccessibilityActor> const& weak_accessibility)
{
    if (auto accessibility = weak_accessibility.strong_ref())
        return accessibility->m_tab.strong_ref();
    return {};
}

RefPtr<AccessibilityWalkerActor> AccessibilityActor::walker_for(WeakPtr<AccessibilityActor> const& weak_accessibility)
{
    if (auto accessibility = weak_accessibility.strong_ref())
        return accessibility->m_walker.strong_ref();
    return {};
}

}
