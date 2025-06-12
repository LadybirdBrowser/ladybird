/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashFunctions.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Traits.h>
#include <LibDevTools/Actor.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/Forward.h>

namespace DevTools {

struct NodeIdentifier {
    static NodeIdentifier for_node(JsonObject const& node);

    bool operator==(NodeIdentifier const&) const = default;

    Web::UniqueNodeID id { 0 };
    Optional<Web::CSS::PseudoElement> pseudo_element;
};

class NodeActor final : public Actor {
public:
    static constexpr auto base_name = "node"sv;

    static NonnullRefPtr<NodeActor> create(DevToolsServer&, String name, NodeIdentifier, WeakPtr<WalkerActor>);
    virtual ~NodeActor() override;

    NodeIdentifier const& node_identifier() const { return m_node_identifier; }
    WeakPtr<WalkerActor> const& walker() const { return m_walker; }

private:
    NodeActor(DevToolsServer&, String name, NodeIdentifier, WeakPtr<WalkerActor>);

    virtual void handle_message(Message const&) override;

    NodeIdentifier m_node_identifier;

    WeakPtr<WalkerActor> m_walker;
};

}

template<>
struct AK::Traits<DevTools::NodeIdentifier> : public AK::DefaultTraits<DevTools::NodeIdentifier> {
    static bool equals(DevTools::NodeIdentifier const& lhs, DevTools::NodeIdentifier const& rhs)
    {
        return lhs == rhs;
    }

    static unsigned hash(DevTools::NodeIdentifier const& node_identifier)
    {
        auto pseudo_element = node_identifier.pseudo_element.value_or(Web::CSS::PseudoElement::KnownPseudoElementCount);
        return pair_int_hash(node_identifier.id.value(), to_underlying(pseudo_element));
    }
};
