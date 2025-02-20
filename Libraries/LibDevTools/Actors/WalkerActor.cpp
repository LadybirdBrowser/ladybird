/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/StringUtils.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/Actors/WalkerActor.h>
#include <LibWeb/DOM/NodeType.h>

namespace DevTools {

NonnullRefPtr<WalkerActor> WalkerActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, JsonObject dom_tree)
{
    return adopt_ref(*new WalkerActor(devtools, move(name), move(tab), move(dom_tree)));
}

WalkerActor::WalkerActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, JsonObject dom_tree)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_dom_tree(move(dom_tree))
{
    populate_dom_tree_cache(m_dom_tree);
}

WalkerActor::~WalkerActor() = default;

void WalkerActor::handle_message(StringView type, JsonObject const& message)
{
    JsonObject response;
    response.set("from"sv, name());

    if (type == "children"sv) {
        auto node = message.get_string("node"sv);
        if (!node.has_value()) {
            send_missing_parameter_error("node"sv);
            return;
        }

        JsonArray nodes;

        if (auto ancestor_node = m_actor_to_dom_node_map.get(*node); ancestor_node.has_value()) {
            if (auto children = ancestor_node.value()->get_array("children"sv); children.has_value()) {

                children->for_each([&](JsonValue const& child) {
                    nodes.must_append(serialize_node(child.as_object()));
                });
            }
        }

        response.set("hasFirst"sv, !nodes.is_empty());
        response.set("hasLast"sv, !nodes.is_empty());
        response.set("nodes"sv, move(nodes));
        send_message(move(response));
        return;
    }

    if (type == "querySelector"sv) {
        auto node = message.get_string("node"sv);
        if (!node.has_value()) {
            send_missing_parameter_error("node"sv);
            return;
        }

        auto selector = message.get_string("selector"sv);
        if (!selector.has_value()) {
            send_missing_parameter_error("selector"sv);
            return;
        }

        if (auto ancestor_node = m_actor_to_dom_node_map.get(*node); ancestor_node.has_value()) {
            if (auto selected_node = find_node_by_selector(*ancestor_node.value(), *selector); selected_node.has_value()) {
                response.set("node"sv, serialize_node(*selected_node));

                if (auto parent = m_dom_node_to_parent_map.get(&selected_node.value()); parent.value() && parent.value() != ancestor_node.value()) {
                    // FIXME: Should this be a stack of nodes leading to `ancestor_node`?
                    JsonArray new_parents;
                    new_parents.must_append(serialize_node(*parent.value()));

                    response.set("newParents"sv, move(new_parents));
                }
            }
        }

        send_message(move(response));
        return;
    }

    if (type == "watchRootNode"sv) {
        response.set("type"sv, "root-available"sv);
        response.set("node"sv, serialize_root());
        send_message(move(response));

        JsonObject message;
        message.set("from"sv, name());
        send_message(move(message));

        return;
    }

    send_unrecognized_packet_type_error(type);
}

bool WalkerActor::is_suitable_for_dom_inspection(JsonValue const& node)
{
    if (!node.is_object())
        return true;

    auto const& object = node.as_object();

    if (!object.has_string("name"sv) || !object.has_string("type"sv))
        return false;

    if (auto text = object.get_string("text"sv); text.has_value()) {
        if (AK::StringUtils::is_whitespace(*text))
            return false;
    }
    if (auto data = object.get_string("data"sv); data.has_value()) {
        if (AK::StringUtils::is_whitespace(*data))
            return false;
    }

    return true;
}

JsonValue WalkerActor::serialize_root() const
{
    return serialize_node(m_dom_tree);
}

JsonValue WalkerActor::serialize_node(JsonObject const& node) const
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return {};

    auto actor = node.get_string("actor"sv);
    if (!actor.has_value())
        return {};

    auto name = node.get_string("name"sv).release_value();
    auto type = node.get_string("type"sv).release_value();

    auto dom_type = Web::DOM::NodeType::INVALID;
    JsonValue node_value;

    auto is_top_level_document = &node == &m_dom_tree;
    auto is_displayed = !is_top_level_document && node.get_bool("visible"sv).value_or(false);
    auto is_scrollable = node.get_bool("scrollable"sv).value_or(false);
    auto is_shadow_root = false;

    if (type == "document"sv) {
        dom_type = Web::DOM::NodeType::DOCUMENT_NODE;
    } else if (type == "element"sv) {
        dom_type = Web::DOM::NodeType::ELEMENT_NODE;
    } else if (type == "text"sv) {
        dom_type = Web::DOM::NodeType::TEXT_NODE;

        if (auto text = node.get_string("text"sv); text.has_value())
            node_value = text.release_value();
    } else if (type == "comment"sv) {
        dom_type = Web::DOM::NodeType::COMMENT_NODE;

        if (auto data = node.get_string("data"sv); data.has_value())
            node_value = data.release_value();
    } else if (type == "shadow-root"sv) {
        is_shadow_root = true;
    }

    size_t child_count = 0;
    if (auto children = node.get_array("children"sv); children.has_value())
        child_count = children->size();

    JsonArray attrs;

    if (auto attributes = node.get_object("attributes"sv); attributes.has_value()) {
        attributes->for_each_member([&](String const& name, JsonValue const& value) {
            if (!value.is_string())
                return;

            JsonObject attr;
            attr.set("name"sv, name);
            attr.set("value"sv, value.as_string());
            attrs.must_append(move(attr));
        });
    }

    JsonObject serialized;
    serialized.set("actor"sv, actor.release_value());
    serialized.set("attrs"sv, move(attrs));
    serialized.set("baseURI"sv, tab->description().url);
    serialized.set("causesOverflow"sv, false);
    serialized.set("containerType"sv, JsonValue {});
    serialized.set("displayName"sv, name.to_ascii_lowercase());
    serialized.set("displayType"sv, "block"sv);
    serialized.set("host"sv, JsonValue {});
    serialized.set("isAfterPseudoElement"sv, false);
    serialized.set("isAnonymous"sv, false);
    serialized.set("isBeforePseudoElement"sv, false);
    serialized.set("isDirectShadowHostChild"sv, JsonValue {});
    serialized.set("isDisplayed"sv, is_displayed);
    serialized.set("isInHTMLDocument"sv, true);
    serialized.set("isMarkerPseudoElement"sv, false);
    serialized.set("isNativeAnonymous"sv, false);
    serialized.set("isScrollable"sv, is_scrollable);
    serialized.set("isShadowHost"sv, false);
    serialized.set("isShadowRoot"sv, is_shadow_root);
    serialized.set("isTopLevelDocument"sv, is_top_level_document);
    serialized.set("nodeName"sv, name);
    serialized.set("nodeType"sv, to_underlying(dom_type));
    serialized.set("nodeValue"sv, move(node_value));
    serialized.set("numChildren"sv, child_count);
    serialized.set("shadowRootMode"sv, JsonValue {});
    serialized.set("traits"sv, JsonObject {});

    if (!is_top_level_document) {
        if (auto parent = m_dom_node_to_parent_map.get(&node); parent.has_value() && parent.value()) {
            actor = parent.value()->get_string("actor"sv);
            if (!actor.has_value())
                return {};

            serialized.set("parent"sv, actor.release_value());
        }
    }

    return serialized;
}

Optional<WalkerActor::DOMNode> WalkerActor::dom_node(StringView actor)
{
    auto maybe_dom_node = m_actor_to_dom_node_map.get(actor);
    if (!maybe_dom_node.has_value() || !maybe_dom_node.value())
        return {};

    auto const& dom_node = *maybe_dom_node.value();

    auto pseudo_element = dom_node.get_integer<UnderlyingType<Web::CSS::Selector::PseudoElement::Type>>("pseudo-element"sv).map([](auto value) {
        VERIFY(value < to_underlying(Web::CSS::Selector::PseudoElement::Type::KnownPseudoElementCount));
        return static_cast<Web::CSS::Selector::PseudoElement::Type>(value);
    });

    Web::UniqueNodeID node_id { 0 };
    if (pseudo_element.has_value())
        node_id = dom_node.get_integer<Web::UniqueNodeID::Type>("parent-id"sv).value();
    else
        node_id = dom_node.get_integer<Web::UniqueNodeID::Type>("id"sv).value();

    return DOMNode { .node = dom_node, .id = node_id, .pseudo_element = pseudo_element };
}

Optional<JsonObject const&> WalkerActor::find_node_by_selector(JsonObject const& node, StringView selector)
{
    auto matches = [&](auto const& candidate) {
        return candidate.get_string("name"sv)->equals_ignoring_ascii_case(selector);
    };

    if (matches(node))
        return node;

    if (auto children = node.get_array("children"sv); children.has_value()) {
        for (size_t i = 0; i < children->size(); ++i) {
            auto const& child = children->at(i);

            if (matches(child.as_object()))
                return child.as_object();

            if (auto result = find_node_by_selector(child.as_object(), selector); result.has_value())
                return result;
        }
    }

    return {};
}

void WalkerActor::populate_dom_tree_cache(JsonObject& node, JsonObject const* parent)
{
    m_dom_node_to_parent_map.set(&node, parent);

    auto actor = MUST(String::formatted("{}-node{}", name(), m_dom_node_count++));
    m_actor_to_dom_node_map.set(actor, &node);
    node.set("actor"sv, actor);

    auto children = node.get_array("children"sv);
    if (!children.has_value())
        return;

    children->values().remove_all_matching([&](JsonValue const& child) {
        return !is_suitable_for_dom_inspection(child);
    });

    children->for_each([&](JsonValue& child) {
        populate_dom_tree_cache(child.as_object(), &node);
    });
}

}
