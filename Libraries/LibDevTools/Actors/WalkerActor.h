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

namespace DevTools {

class WalkerActor final : public Actor {
public:
    static constexpr auto base_name = "walker"sv;

    static NonnullRefPtr<WalkerActor> create(DevToolsServer&, ByteString name, WeakPtr<TabActor>, JsonObject dom_tree);
    virtual ~WalkerActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

    static bool is_suitable_for_dom_inspection(JsonValue const&);
    JsonValue serialize_root() const;

private:
    WalkerActor(DevToolsServer&, ByteString name, WeakPtr<TabActor>, JsonObject dom_tree);

    JsonValue serialize_node(JsonObject const&) const;
    Optional<JsonObject const&> find_node_by_selector(JsonObject const& node, StringView selector);

    void populate_dom_tree_cache(JsonObject& node, JsonObject const* parent = nullptr);

    WeakPtr<TabActor> m_tab;
    JsonObject m_dom_tree;

    HashMap<JsonObject const*, JsonObject const*> m_dom_node_to_parent_map;
    HashMap<ByteString, JsonObject const*> m_actor_to_dom_node_map;
    size_t m_dom_node_count { 0 };
};

}
