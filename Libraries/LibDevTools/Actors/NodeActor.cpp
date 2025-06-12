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

    identifier.pseudo_element = node.get_integer<UnderlyingType<Web::CSS::PseudoElement>>("pseudo-element"sv).map([](auto value) {
        VERIFY(value < to_underlying(Web::CSS::PseudoElement::KnownPseudoElementCount));
        return static_cast<Web::CSS::PseudoElement>(value);
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

void NodeActor::handle_message(Message const& message)
{
    JsonObject response;

    if (message.type == "getUniqueSelector"sv) {
        auto dom_node = WalkerActor::dom_node_for(m_walker, name());
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, name());
            return;
        }

        response.set("value"sv, dom_node->node.get_string("name"sv)->to_ascii_lowercase());
        send_response(message, move(response));
        return;
    }

    if (message.type == "modifyAttributes"sv) {
        auto modifications = get_required_parameter<JsonArray>(message, "modifications"sv);
        if (!modifications.has_value())
            return;

        auto [attribute_to_replace, replacement_attributes] = parse_attribute_modification(*modifications);
        if (!attribute_to_replace.has_value() && replacement_attributes.is_empty())
            return;

        auto dom_node = WalkerActor::dom_node_for(m_walker, name());
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, name());
            return;
        }

        if (attribute_to_replace.has_value())
            devtools().delegate().replace_dom_node_attribute(dom_node->tab->description(), dom_node->identifier.id, *attribute_to_replace, move(replacement_attributes), default_async_handler(message));
        else
            devtools().delegate().add_dom_node_attributes(dom_node->tab->description(), dom_node->identifier.id, move(replacement_attributes), default_async_handler(message));

        return;
    }

    if (message.type == "setNodeValue"sv) {
        auto value = get_required_parameter<String>(message, "value"sv);
        if (!value.has_value())
            return;

        auto dom_node = WalkerActor::dom_node_for(m_walker, name());
        if (!dom_node.has_value()) {
            send_unknown_actor_error(message, name());
            return;
        }

        devtools().delegate().set_dom_node_text(dom_node->tab->description(), dom_node->identifier.id, *value, default_async_handler(message));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
