/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <LibDevTools/Actors/NodeActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NodeIdentifier NodeIdentifier::for_node(JsonObject const& node)
{
    NodeIdentifier identifier;

    identifier.pseudo_element = node.get_integer<UnderlyingType<Web::CSS::Selector::PseudoElement::Type>>("pseudo-element"sv).map([](auto value) {
        VERIFY(value < to_underlying(Web::CSS::Selector::PseudoElement::Type::KnownPseudoElementCount));
        return static_cast<Web::CSS::Selector::PseudoElement::Type>(value);
    });

    if (identifier.pseudo_element.has_value())
        identifier.id = node.get_integer<Web::UniqueNodeID::Type>("parent-id"sv).value();
    else
        identifier.id = node.get_integer<Web::UniqueNodeID::Type>("id"sv).value();

    return identifier;
}

NonnullRefPtr<NodeActor> NodeActor::create(DevToolsServer& devtools, String name, NodeIdentifier node_identifier, WeakPtr<WalkerActor> walker)
{
    return adopt_ref(*new NodeActor(devtools, move(name), move(node_identifier), move(walker)));
}

NodeActor::NodeActor(DevToolsServer& devtools, String name, NodeIdentifier node_identifier, WeakPtr<WalkerActor> walker)
    : Actor(devtools, move(name))
    , m_node_identifier(move(node_identifier))
    , m_walker(move(walker))
{
}

NodeActor::~NodeActor() = default;

void NodeActor::handle_message(StringView type, JsonObject const& message)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "getUniqueSelector"sv) {
        if (auto dom_node = WalkerActor::dom_node_for(m_walker, name()); dom_node.has_value())
            response.set("value"sv, dom_node->node.get_string("name"sv)->to_ascii_lowercase());

        send_message(move(response));
        return;
    }

    if (type == "setNodeValue"sv) {
        auto value = message.get_string("value"sv);
        if (!value.has_value()) {
            send_missing_parameter_error("value"sv);
            return;
        }

        if (auto dom_node = WalkerActor::dom_node_for(m_walker, name()); dom_node.has_value()) {
            auto block_token = block_responses();

            devtools().delegate().set_dom_node_text(
                dom_node->tab->description(), dom_node->identifier.id, value.release_value(),
                [weak_self = make_weak_ptr<NodeActor>(), block_token = move(block_token)](ErrorOr<Web::UniqueNodeID> node_id) mutable {
                    if (node_id.is_error()) {
                        dbgln_if(DEVTOOLS_DEBUG, "Unable to edit DOM node: {}", node_id.error());
                        return;
                    }

                    if (auto self = weak_self.strong_ref())
                        self->finished_editing_dom_node(move(block_token));
                });
        }

        return;
    }

    send_unrecognized_packet_type_error(type);
}

void NodeActor::finished_editing_dom_node(BlockToken block_token)
{
    JsonObject message;
    message.set("from"sv, name());
    send_message(move(message), move(block_token));
}

}
