/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/JsonObject.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibDevTools/Actor.h>
#include <LibDevTools/Actors/NodeActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/Forward.h>
#include <LibDevTools/Node.h>
#include <LibWeb/Forward.h>
#include <LibWebView/Forward.h>

namespace DevTools {

class DEVTOOLS_API WalkerActor final : public Actor {
public:
    static constexpr auto base_name = "walker"sv;

    static NonnullRefPtr<WalkerActor> create(DevToolsServer&, String name, WeakPtr<TabActor>, JsonObject dom_tree);
    virtual ~WalkerActor() override;

    static bool is_suitable_for_dom_inspection(JsonValue const&);
    JsonValue serialize_root() const;

    static Optional<Node> dom_node_for(WeakPtr<WalkerActor> const&, StringView actor);
    Optional<Node> dom_node(StringView actor);
    Optional<String> node_actor_name_for(Web::UniqueNodeID) const;
    void document_unloaded();
    void replace_dom_tree(JsonObject);

private:
    WalkerActor(DevToolsServer&, String name, WeakPtr<TabActor>, JsonObject dom_tree);

    virtual void handle_message(Message const&) override;

    JsonValue serialize_node(JsonObject const&) const;
    Optional<JsonObject const&> find_node_by_selector(JsonObject const& node, StringView selector);

    Optional<JsonObject const&> previous_sibling_for_node(JsonObject const& node);
    Optional<JsonObject const&> next_sibling_for_node(JsonObject const& node);
    Optional<JsonObject const&> parent_grid_node_for_node(JsonObject const& node) const;
    Optional<JsonObject const&> remove_node(JsonObject const& node);

    void new_dom_node_mutation(WebView::Mutation);
    JsonValue serialize_mutations();

    bool replace_node_in_tree(JsonObject replacement);

    void clear_dom_tree_cache();
    void clear_dom_tree_state();
    void populate_dom_tree_cache();
    void populate_dom_tree_cache(JsonObject& node, JsonObject const* parent);

    NodeActor const& actor_for_node(JsonObject const& node);
    void handle_node_picker_event(DevToolsDelegate::NodePickerEvent);
    void stop_node_picker();
    Optional<JsonObject const&> element_node_for_picker_node(JsonObject const&) const;
    JsonObject serialize_disconnected_node(JsonObject const&) const;

    WeakPtr<TabActor> m_tab;
    WeakPtr<LayoutInspectorActor> m_layout_inspector;

    JsonObject m_dom_tree;
    bool m_has_live_dom_tree { true };

    Vector<WebView::Mutation> m_dom_node_mutations;
    bool m_has_new_mutations_since_last_mutations_request { false };

    HashMap<JsonObject const*, JsonObject const*> m_dom_node_to_parent_map;
    HashMap<String, JsonObject const*> m_actor_to_dom_node_map;
    HashMap<Web::UniqueNodeID, String> m_dom_node_id_to_actor_map;

    HashMap<NodeIdentifier, WeakPtr<NodeActor>> m_node_actors;

    bool m_is_picking_node { false };
    Optional<Web::UniqueNodeID> m_picker_hovered_node_id;
};

}
