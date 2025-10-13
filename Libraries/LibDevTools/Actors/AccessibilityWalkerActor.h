/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibDevTools/Actor.h>
#include <LibDevTools/Actors/AccessibilityNodeActor.h>
#include <LibDevTools/Forward.h>

namespace DevTools {

class DEVTOOLS_API AccessibilityWalkerActor final : public Actor {
public:
    static constexpr auto base_name = "accessibility-walker"sv;

    static NonnullRefPtr<AccessibilityWalkerActor> create(DevToolsServer&, String name, WeakPtr<TabActor>, JsonObject accessibility_tree);
    virtual ~AccessibilityWalkerActor() override;

    static bool is_suitable_for_accessibility_inspection(JsonValue const&);
    JsonValue serialize_root() const;
    JsonValue serialize_node(JsonObject const&) const;

    static Optional<Node> accessibility_node_for(WeakPtr<AccessibilityWalkerActor> const&, StringView actor);
    Optional<Node> accessibility_node(StringView actor);

    static Optional<Node> parent_of_accessibility_node_for(WeakPtr<AccessibilityWalkerActor> const&, Node const&);
    Optional<Node> parent_of_accessibility_node(Node const&);

    static Optional<Node> root_accessibility_node_for(WeakPtr<AccessibilityWalkerActor> const&);
    Optional<Node> root_accessibility_node();

private:
    AccessibilityWalkerActor(DevToolsServer&, String name, WeakPtr<TabActor>, JsonObject accessibility_tree);

    virtual void handle_message(Message const&) override;

    void populate_accessibility_tree_cache();
    void populate_accessibility_tree_cache(JsonObject& node, JsonObject const* parent);

    AccessibilityNodeActor const& actor_for_node(JsonObject const& node);

    WeakPtr<TabActor> m_tab;
    JsonObject m_accessibility_tree;

    HashMap<JsonObject const*, JsonObject const*> m_node_to_parent_map;
    HashMap<String, JsonObject const*> m_actor_to_node_map;
    HashMap<Web::UniqueNodeID, String> m_node_id_to_actor_map;

    HashMap<NodeIdentifier, WeakPtr<AccessibilityNodeActor>> m_node_actors;
};

}
