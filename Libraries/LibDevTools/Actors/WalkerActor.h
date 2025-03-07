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
#include <LibWeb/Forward.h>
#include <LibWebView/Forward.h>

namespace DevTools {

class WalkerActor final : public Actor {
public:
    static constexpr auto base_name = "walker"sv;

    static NonnullRefPtr<WalkerActor> create(DevToolsServer&, String name, WeakPtr<TabActor>, JsonObject dom_tree);
    virtual ~WalkerActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

    static bool is_suitable_for_dom_inspection(JsonValue const&);
    JsonValue serialize_root() const;

    struct DOMNode {
        JsonObject const& node;
        NodeIdentifier identifier;
        NonnullRefPtr<TabActor> tab;
    };
    static Optional<DOMNode> dom_node_for(WeakPtr<WalkerActor> const&, StringView actor);
    Optional<DOMNode> dom_node(StringView actor);

private:
    WalkerActor(DevToolsServer&, String name, WeakPtr<TabActor>, JsonObject dom_tree);

    JsonValue serialize_node(JsonObject const&) const;
    Optional<JsonObject const&> find_node_by_selector(JsonObject const& node, StringView selector);

    void new_dom_node_mutation(WebView::Mutation);
    JsonValue serialize_mutations();

    bool replace_node_in_tree(JsonObject replacement);

    void populate_dom_tree_cache();
    void populate_dom_tree_cache(JsonObject& node, JsonObject const* parent);

    NodeActor const& actor_for_node(JsonObject const& node);

    WeakPtr<TabActor> m_tab;
    WeakPtr<LayoutInspectorActor> m_layout_inspector;

    JsonObject m_dom_tree;

    Vector<WebView::Mutation> m_dom_node_mutations;
    bool m_has_new_mutations_since_last_mutations_request { false };

    HashMap<JsonObject const*, JsonObject const*> m_dom_node_to_parent_map;
    HashMap<String, JsonObject const*> m_actor_to_dom_node_map;
    HashMap<Web::UniqueNodeID, String> m_dom_node_id_to_actor_map;
};

}
