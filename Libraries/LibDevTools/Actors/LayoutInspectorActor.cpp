/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <LibDevTools/Actors/LayoutInspectorActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

static constexpr auto grid_container_node_id_key = "containerNodeId"sv;

static Optional<Web::UniqueNodeID> container_node_id_from_grid_layout(JsonObject const& grid_layout)
{
    auto node_id = grid_layout.get_i64(grid_container_node_id_key);
    if (!node_id.has_value())
        return {};
    return Web::UniqueNodeID { *node_id };
}

NonnullRefPtr<GridActor> GridActor::create(DevToolsServer& devtools, String name, WeakPtr<LayoutInspectorActor> layout_inspector, WeakPtr<WalkerActor> walker, Web::UniqueNodeID container_node_id, JsonObject grid_layout)
{
    return adopt_ref(*new GridActor(devtools, move(name), move(layout_inspector), move(walker), container_node_id, move(grid_layout)));
}

GridActor::GridActor(DevToolsServer& devtools, String name, WeakPtr<LayoutInspectorActor> layout_inspector, WeakPtr<WalkerActor> walker, Web::UniqueNodeID container_node_id, JsonObject grid_layout)
    : Actor(devtools, move(name))
    , m_layout_inspector(move(layout_inspector))
    , m_walker(move(walker))
    , m_container_node_id(container_node_id)
    , m_grid_layout(move(grid_layout))
{
}

GridActor::~GridActor() = default;

void GridActor::update_grid_layout(JsonObject grid_layout)
{
    m_grid_layout = move(grid_layout);
}

void GridActor::handle_message(Message const& message)
{
    send_unrecognized_packet_type_error(message);
}

JsonValue GridActor::serialize_grid() const
{
    auto grid = m_grid_layout;
    grid.remove(grid_container_node_id_key);
    grid.set("actor"sv, name());

    if (auto walker = m_walker.strong_ref()) {
        if (auto node_actor_name = walker->node_actor_name_for(m_container_node_id); node_actor_name.has_value())
            grid.set("containerNodeActorID"sv, node_actor_name.release_value());
    }

    return grid;
}

NonnullRefPtr<LayoutInspectorActor> LayoutInspectorActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<WalkerActor> walker)
{
    return adopt_ref(*new LayoutInspectorActor(devtools, move(name), move(tab), move(walker)));
}

LayoutInspectorActor::LayoutInspectorActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, WeakPtr<WalkerActor> walker)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_walker(move(walker))
{
}

LayoutInspectorActor::~LayoutInspectorActor() = default;

void LayoutInspectorActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getCurrentFlexbox"sv) {
        response.set("flexbox"sv, JsonValue {});
        send_response(message, move(response));
        return;
    }

    if (message.type == "getCurrentGrid"sv) {
        auto node = get_required_parameter<String>(message, "node"sv);
        if (!node.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(m_walker, *node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *node);
            return;
        }

        auto tab = m_tab.strong_ref();
        if (!tab) {
            response.set("grid"sv, JsonValue {});
            send_response(message, move(response));
            return;
        }

        devtools().delegate().inspect_current_grid(tab->description(), dom_node->identifier.id,
            weak_callback(*this, [message](auto& self, Optional<JsonObject> grid_layout) {
                JsonObject response;
                if (grid_layout.has_value()) {
                    if (auto grid_actor = self.actor_for_grid_layout(grid_layout.release_value()); grid_actor.has_value())
                        response.set("grid"sv, grid_actor->serialize_grid());
                    else
                        response.set("grid"sv, JsonValue {});
                } else {
                    response.set("grid"sv, JsonValue {});
                }

                self.send_response(message, move(response));
            }));

        return;
    }

    if (message.type == "getGrids"sv) {
        auto root_node = get_required_parameter<String>(message, "rootNode"sv);
        if (!root_node.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(m_walker, *root_node);
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, *root_node);
            return;
        }

        auto tab = m_tab.strong_ref();
        if (!tab) {
            response.set("grids"sv, JsonArray {});
            send_response(message, move(response));
            return;
        }

        devtools().delegate().inspect_grid_layouts(tab->description(), dom_node->identifier.id,
            weak_callback(*this, [message](auto& self, JsonArray grid_layouts) {
                JsonArray grids;
                grid_layouts.for_each([&](auto const& grid_layout) {
                    if (!grid_layout.is_object())
                        return;

                    if (auto grid_actor = self.actor_for_grid_layout(grid_layout.as_object()); grid_actor.has_value())
                        grids.must_append(grid_actor->serialize_grid());
                });

                JsonObject response;
                response.set("grids"sv, move(grids));
                self.send_response(message, move(response));
            }));

        return;
    }

    send_unrecognized_packet_type_error(message);
}

Optional<GridActor&> LayoutInspectorActor::actor_for_grid_layout(JsonObject grid_layout)
{
    auto container_node_id = container_node_id_from_grid_layout(grid_layout);
    if (!container_node_id.has_value())
        return {};

    if (auto* grid_actor = m_grid_actors.get(*container_node_id).value_or(nullptr)) {
        grid_actor->update_grid_layout(move(grid_layout));
        return *grid_actor;
    }

    auto& grid_actor = devtools().register_actor<GridActor>(make_weak_ptr<LayoutInspectorActor>(), m_walker, *container_node_id, move(grid_layout));
    m_grid_actors.set(*container_node_id, grid_actor);
    return grid_actor;
}

}
