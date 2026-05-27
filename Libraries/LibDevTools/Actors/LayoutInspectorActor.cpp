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

static constexpr auto flexbox_container_node_id_key = "containerNodeId"sv;
static constexpr auto flex_item_node_id_key = "nodeId"sv;
static constexpr auto grid_container_node_id_key = "containerNodeId"sv;

static Optional<Web::UniqueNodeID> container_node_id_from_flexbox_layout(JsonObject const& flexbox_layout)
{
    auto node_id = flexbox_layout.get_i64(flexbox_container_node_id_key);
    if (!node_id.has_value())
        return {};
    return Web::UniqueNodeID { *node_id };
}

static Optional<Web::UniqueNodeID> node_id_from_flex_item(JsonObject const& flex_item)
{
    auto node_id = flex_item.get_i64(flex_item_node_id_key);
    if (!node_id.has_value())
        return {};
    return Web::UniqueNodeID { *node_id };
}

static Optional<Web::UniqueNodeID> container_node_id_from_grid_layout(JsonObject const& grid_layout)
{
    auto node_id = grid_layout.get_i64(grid_container_node_id_key);
    if (!node_id.has_value())
        return {};
    return Web::UniqueNodeID { *node_id };
}

NonnullRefPtr<FlexItemActor> FlexItemActor::create(DevToolsServer& devtools, String name, WeakPtr<WalkerActor> walker, Web::UniqueNodeID node_id, JsonObject flex_item)
{
    return adopt_ref(*new FlexItemActor(devtools, move(name), move(walker), node_id, move(flex_item)));
}

FlexItemActor::FlexItemActor(DevToolsServer& devtools, String name, WeakPtr<WalkerActor> walker, Web::UniqueNodeID node_id, JsonObject flex_item)
    : Actor(devtools, move(name))
    , m_walker(move(walker))
    , m_node_id(node_id)
    , m_flex_item(move(flex_item))
{
}

FlexItemActor::~FlexItemActor() = default;

void FlexItemActor::update_flex_item(JsonObject flex_item)
{
    m_flex_item = move(flex_item);
}

void FlexItemActor::handle_message(Message const& message)
{
    send_unrecognized_packet_type_error(message);
}

JsonValue FlexItemActor::serialize_flex_item() const
{
    auto item = m_flex_item;
    item.remove(flex_item_node_id_key);
    item.set("actor"sv, name());

    if (auto walker = m_walker.strong_ref()) {
        if (auto node_actor_name = walker->node_actor_name_for(m_node_id); node_actor_name.has_value())
            item.set("nodeActorID"sv, node_actor_name.release_value());
    }

    return item;
}

NonnullRefPtr<FlexboxActor> FlexboxActor::create(DevToolsServer& devtools, String name, WeakPtr<LayoutInspectorActor> layout_inspector, WeakPtr<WalkerActor> walker, Web::UniqueNodeID container_node_id, JsonObject flexbox_layout)
{
    return adopt_ref(*new FlexboxActor(devtools, move(name), move(layout_inspector), move(walker), container_node_id, move(flexbox_layout)));
}

FlexboxActor::FlexboxActor(DevToolsServer& devtools, String name, WeakPtr<LayoutInspectorActor> layout_inspector, WeakPtr<WalkerActor> walker, Web::UniqueNodeID container_node_id, JsonObject flexbox_layout)
    : Actor(devtools, move(name))
    , m_layout_inspector(move(layout_inspector))
    , m_walker(move(walker))
    , m_container_node_id(container_node_id)
    , m_flexbox_layout(move(flexbox_layout))
{
}

FlexboxActor::~FlexboxActor() = default;

void FlexboxActor::update_flexbox_layout(JsonObject flexbox_layout)
{
    m_flexbox_layout = move(flexbox_layout);
}

void FlexboxActor::handle_message(Message const& message)
{
    if (message.type == "getFlexItems"sv) {
        JsonArray flex_items;
        if (auto items = m_flexbox_layout.get_array("items"sv); items.has_value()) {
            items->for_each([&](auto const& item) {
                if (!item.is_object())
                    return;

                if (auto flex_item_actor = actor_for_flex_item(item.as_object()); flex_item_actor.has_value())
                    flex_items.must_append(flex_item_actor->serialize_flex_item());
            });
        }

        JsonObject response;
        response.set("flexitems"sv, move(flex_items));
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

JsonValue FlexboxActor::serialize_flexbox() const
{
    auto flexbox = m_flexbox_layout;
    flexbox.remove(flexbox_container_node_id_key);
    flexbox.remove("items"sv);
    flexbox.set("actor"sv, name());

    if (auto walker = m_walker.strong_ref()) {
        if (auto node_actor_name = walker->node_actor_name_for(m_container_node_id); node_actor_name.has_value())
            flexbox.set("containerNodeActorID"sv, node_actor_name.release_value());
    }

    return flexbox;
}

Optional<FlexItemActor&> FlexboxActor::actor_for_flex_item(JsonObject flex_item)
{
    auto node_id = node_id_from_flex_item(flex_item);
    if (!node_id.has_value())
        return {};

    if (auto* flex_item_actor = m_flex_item_actors.get(*node_id).value_or(nullptr)) {
        flex_item_actor->update_flex_item(move(flex_item));
        return *flex_item_actor;
    }

    auto& flex_item_actor = devtools().register_actor<FlexItemActor>(m_walker, *node_id, move(flex_item));
    m_flex_item_actors.set(*node_id, flex_item_actor);
    return flex_item_actor;
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
            response.set("flexbox"sv, JsonValue {});
            send_response(message, move(response));
            return;
        }

        auto only_look_at_parents = message.data.get_bool("onlyLookAtParents"sv).value_or(false);
        devtools().delegate().inspect_current_flexbox(tab->description(), dom_node->identifier.id, only_look_at_parents,
            weak_callback(*this, [message](auto& self, Optional<JsonObject> flexbox_layout) {
                JsonObject response;
                if (flexbox_layout.has_value()) {
                    if (auto flexbox_actor = self.actor_for_flexbox_layout(flexbox_layout.release_value()); flexbox_actor.has_value())
                        response.set("flexbox"sv, flexbox_actor->serialize_flexbox());
                    else
                        response.set("flexbox"sv, JsonValue {});
                } else {
                    response.set("flexbox"sv, JsonValue {});
                }

                self.send_response(message, move(response));
            }));

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

Optional<FlexboxActor&> LayoutInspectorActor::actor_for_flexbox_layout(JsonObject flexbox_layout)
{
    auto container_node_id = container_node_id_from_flexbox_layout(flexbox_layout);
    if (!container_node_id.has_value())
        return {};

    if (auto* flexbox_actor = m_flexbox_actors.get(*container_node_id).value_or(nullptr)) {
        flexbox_actor->update_flexbox_layout(move(flexbox_layout));
        return *flexbox_actor;
    }

    auto& flexbox_actor = devtools().register_actor<FlexboxActor>(make_weak_ptr<LayoutInspectorActor>(), m_walker, *container_node_id, move(flexbox_layout));
    m_flexbox_actors.set(*container_node_id, flexbox_actor);
    return flexbox_actor;
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
