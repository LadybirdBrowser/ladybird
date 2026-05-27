/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Forward.h>
#include <LibWeb/Forward.h>

namespace DevTools {

class DEVTOOLS_API FlexItemActor final : public Actor {
public:
    static constexpr auto base_name = "flex-item"sv;

    static NonnullRefPtr<FlexItemActor> create(DevToolsServer&, String name, WeakPtr<WalkerActor>, Web::UniqueNodeID, JsonObject);
    virtual ~FlexItemActor() override;

    void update_flex_item(JsonObject);
    Web::UniqueNodeID node_id() const { return m_node_id; }
    JsonValue serialize_flex_item() const;

private:
    FlexItemActor(DevToolsServer&, String name, WeakPtr<WalkerActor>, Web::UniqueNodeID, JsonObject);

    virtual void handle_message(Message const&) override;

    WeakPtr<WalkerActor> m_walker;
    Web::UniqueNodeID m_node_id;
    JsonObject m_flex_item;
};

class DEVTOOLS_API FlexboxActor final : public Actor {
public:
    static constexpr auto base_name = "flexbox"sv;

    static NonnullRefPtr<FlexboxActor> create(DevToolsServer&, String name, WeakPtr<LayoutInspectorActor>, WeakPtr<WalkerActor>, Web::UniqueNodeID, JsonObject);
    virtual ~FlexboxActor() override;

    void update_flexbox_layout(JsonObject);
    Web::UniqueNodeID container_node_id() const { return m_container_node_id; }
    JsonValue serialize_flexbox() const;

private:
    FlexboxActor(DevToolsServer&, String name, WeakPtr<LayoutInspectorActor>, WeakPtr<WalkerActor>, Web::UniqueNodeID, JsonObject);

    virtual void handle_message(Message const&) override;

    Optional<FlexItemActor&> actor_for_flex_item(JsonObject);

    WeakPtr<LayoutInspectorActor> m_layout_inspector;
    WeakPtr<WalkerActor> m_walker;
    Web::UniqueNodeID m_container_node_id;
    JsonObject m_flexbox_layout;
    HashMap<Web::UniqueNodeID, WeakPtr<FlexItemActor>> m_flex_item_actors;
};

class DEVTOOLS_API GridActor final : public Actor {
public:
    static constexpr auto base_name = "grid"sv;

    static NonnullRefPtr<GridActor> create(DevToolsServer&, String name, WeakPtr<LayoutInspectorActor>, WeakPtr<WalkerActor>, Web::UniqueNodeID, JsonObject);
    virtual ~GridActor() override;

    void update_grid_layout(JsonObject);
    Web::UniqueNodeID container_node_id() const { return m_container_node_id; }
    JsonValue serialize_grid() const;

private:
    GridActor(DevToolsServer&, String name, WeakPtr<LayoutInspectorActor>, WeakPtr<WalkerActor>, Web::UniqueNodeID, JsonObject);

    virtual void handle_message(Message const&) override;

    WeakPtr<LayoutInspectorActor> m_layout_inspector;
    WeakPtr<WalkerActor> m_walker;
    Web::UniqueNodeID m_container_node_id;
    JsonObject m_grid_layout;
};

class DEVTOOLS_API LayoutInspectorActor final : public Actor {
public:
    static constexpr auto base_name = "layout-inspector"sv;

    static NonnullRefPtr<LayoutInspectorActor> create(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<WalkerActor>);
    virtual ~LayoutInspectorActor() override;

private:
    LayoutInspectorActor(DevToolsServer&, String name, WeakPtr<TabActor>, WeakPtr<WalkerActor>);

    virtual void handle_message(Message const&) override;

    Optional<FlexboxActor&> actor_for_flexbox_layout(JsonObject);
    Optional<GridActor&> actor_for_grid_layout(JsonObject);

    WeakPtr<TabActor> m_tab;
    WeakPtr<WalkerActor> m_walker;
    HashMap<Web::UniqueNodeID, WeakPtr<FlexboxActor>> m_flexbox_actors;
    HashMap<Web::UniqueNodeID, WeakPtr<GridActor>> m_grid_actors;
};

}
