/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <LibDevTools/Actor.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace DevTools {

struct NodeIdentifier {
    static NodeIdentifier for_node(JsonObject const& node);

    bool operator==(NodeIdentifier const&) const = default;

    Web::UniqueNodeID id { 0 };
    Optional<Web::CSS::Selector::PseudoElement::Type> pseudo_element;
};

class NodeActor final : public Actor {
public:
    static constexpr auto base_name = "node"sv;

    static NonnullRefPtr<NodeActor> create(DevToolsServer&, String name, NodeIdentifier, WeakPtr<WalkerActor>);
    virtual ~NodeActor() override;

    virtual void handle_message(StringView type, JsonObject const&) override;

    NodeIdentifier const& node_identifier() const { return m_node_identifier; }

private:
    NodeActor(DevToolsServer&, String name, NodeIdentifier, WeakPtr<WalkerActor>);

    NodeIdentifier m_node_identifier;

    WeakPtr<WalkerActor> m_walker;
};

}
