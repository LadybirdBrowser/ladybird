/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonObject.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <LibDevTools/Actors/NodeActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibDevTools/DevToolsDelegate.h>
#include <LibDevTools/DevToolsServer.h>
#include <LibWebView/Attribute.h>

namespace DevTools {

struct AttributeModification {
    Optional<String> attribute_to_replace;
    Vector<WebView::Attribute> replacement_attributes;
};
static AttributeModification parse_attribute_modification(JsonArray const& modifications)
{
    if (modifications.is_empty())
        return {};

    Optional<String> attribute_to_replace;
    Vector<WebView::Attribute> replacement_attributes;

    auto parse_modification = [&](JsonValue const& modification) -> Variant<Empty, String, WebView::Attribute> {
        if (!modification.is_object())
            return {};

        auto name = modification.as_object().get_string("attributeName"sv);
        if (!name.has_value())
            return {};

        auto value = modification.as_object().get_string("newValue"sv);
        if (!value.has_value())
            return *name;

        return WebView::Attribute { *name, *value };
    };

    auto modification = parse_modification(modifications.at(0));
    if (modification.has<Empty>())
        return {};

    modification.visit(
        [&](String& name) { attribute_to_replace = move(name); },
        [&](WebView::Attribute& attribute) { replacement_attributes.append(move(attribute)); },
        [](Empty) { VERIFY_NOT_REACHED(); });

    for (auto i = 1uz; i < modifications.size(); ++i) {
        auto modification = parse_modification(modifications.at(i));

        if (auto* attribute = modification.get_pointer<WebView::Attribute>())
            replacement_attributes.empend(move(attribute->name), move(attribute->value));
    }

    return AttributeModification { move(attribute_to_replace), move(replacement_attributes) };
}

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

    if (type == "modifyAttributes"sv) {
        auto modifications = message.get_array("modifications"sv);
        if (!modifications.has_value()) {
            send_missing_parameter_error("modifications"sv);
            return;
        }

        auto [attribute_to_replace, replacement_attributes] = parse_attribute_modification(*modifications);
        if (!attribute_to_replace.has_value() && replacement_attributes.is_empty())
            return;

        if (auto dom_node = WalkerActor::dom_node_for(m_walker, name()); dom_node.has_value()) {
            auto block_token = block_responses();

            auto on_complete = [weak_self = make_weak_ptr<NodeActor>(), block_token = move(block_token)](ErrorOr<Web::UniqueNodeID> node_id) mutable {
                if (node_id.is_error()) {
                    dbgln_if(DEVTOOLS_DEBUG, "Unable to edit DOM node: {}", node_id.error());
                    return;
                }

                if (auto self = weak_self.strong_ref())
                    self->finished_editing_dom_node(move(block_token));
            };

            if (attribute_to_replace.has_value()) {
                devtools().delegate().replace_dom_node_attribute(dom_node->tab->description(), dom_node->identifier.id, *attribute_to_replace, move(replacement_attributes), move(on_complete));
            } else {
                devtools().delegate().add_dom_node_attributes(dom_node->tab->description(), dom_node->identifier.id, move(replacement_attributes), move(on_complete));
            }
        }

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
                dom_node->tab->description(), dom_node->identifier.id, *value,
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
