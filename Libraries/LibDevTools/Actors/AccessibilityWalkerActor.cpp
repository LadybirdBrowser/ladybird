/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibDevTools/Actors/AccessibilityNodeActor.h>
#include <LibDevTools/Actors/AccessibilityWalkerActor.h>
#include <LibDevTools/Actors/TabActor.h>
#include <LibDevTools/DevToolsServer.h>

namespace DevTools {

NonnullRefPtr<AccessibilityWalkerActor> AccessibilityWalkerActor::create(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, JsonObject accessibility_tree)
{
    return adopt_ref(*new AccessibilityWalkerActor(devtools, move(name), move(tab), move(accessibility_tree)));
}

AccessibilityWalkerActor::AccessibilityWalkerActor(DevToolsServer& devtools, String name, WeakPtr<TabActor> tab, JsonObject accessibility_tree)
    : Actor(devtools, move(name))
    , m_tab(move(tab))
    , m_accessibility_tree(move(accessibility_tree))
{
    populate_accessibility_tree_cache();
}

AccessibilityWalkerActor::~AccessibilityWalkerActor() = default;

void AccessibilityWalkerActor::handle_message(Message const& message)
{
    if (message.type == "children"sv) {
        JsonArray children;
        MUST(children.append(serialize_root()));

        JsonObject response;
        response.set("children"sv, move(children));
        send_response(message, move(response));
        return;
    }

    if (message.type == "hideTabbingOrder"sv) {
        // Blank response is expected.
        send_response(message, JsonObject {});
        return;
    }

    if (message.type == "highlightAccessible"sv) {
        // FIXME: Highlight things.
        JsonObject response;
        response.set("value"sv, false);
        send_response(message, move(response));
        return;
    }

    if (message.type == "unhighlight"sv) {
        // FIXME: Unhighlight things.
        send_response(message, JsonObject {});
        return;
    }

    send_unrecognized_packet_type_error(message);
}

bool AccessibilityWalkerActor::is_suitable_for_accessibility_inspection(JsonValue const& node)
{
    if (!node.is_object())
        return false;

    auto const& object = node.as_object();

    if (!object.has_string("type"sv) || !object.has_string("role"sv))
        return false;

    if (!object.has_i64("id"sv))
        return false;

    if (auto text = object.get_string("text"sv); text.has_value()) {
        if (AK::StringUtils::is_whitespace(*text))
            return false;
    }

    return true;
}

JsonValue AccessibilityWalkerActor::serialize_root() const
{
    return serialize_node(m_accessibility_tree);
}

JsonValue AccessibilityWalkerActor::serialize_node(JsonObject const& node) const
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return {};

    auto actor = node.get_string("actor"sv);
    if (!actor.has_value())
        return {};

    auto name = node.get_string("name"sv).value_or(""_string);
    auto type = node.get_string("type"sv).release_value();
    auto role = node.get_string("role"sv).release_value();

    auto child_count = 0u;
    if (auto children = node.get_array("children"sv); children.has_value())
        child_count = children->size();

    JsonObject serialized;
    serialized.set("actor"sv, actor.release_value());
    serialized.set("name"sv, move(name));
    serialized.set("role"sv, move(role));
    serialized.set("useChildTargetToFetchChildren"sv, false);
    serialized.set("childCount"sv, child_count);
    serialized.set("checks"sv, JsonObject {});
    return serialized;
}

Optional<Node> AccessibilityWalkerActor::accessibility_node_for(WeakPtr<AccessibilityWalkerActor> const& weak_walker, StringView actor)
{
    if (auto walker = weak_walker.strong_ref())
        return walker->accessibility_node(actor);
    return {};
}

Optional<Node> AccessibilityWalkerActor::accessibility_node(StringView actor)
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return {};

    auto maybe_node = m_actor_to_node_map.get(actor);
    if (!maybe_node.has_value() || !maybe_node.value())
        return {};

    auto const& node = *maybe_node.value();
    auto identifier = NodeIdentifier::for_node(node);

    return Node { .node = node, .identifier = move(identifier), .tab = tab.release_nonnull() };
}

Optional<Node> AccessibilityWalkerActor::parent_of_accessibility_node_for(WeakPtr<AccessibilityWalkerActor> const& weak_walker, Node const& accessibility_node)
{
    if (auto walker = weak_walker.strong_ref())
        return walker->parent_of_accessibility_node(accessibility_node);
    return {};
}

Optional<Node> AccessibilityWalkerActor::parent_of_accessibility_node(Node const& accessibility_node)
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return {};

    auto maybe_parent_node = m_node_to_parent_map.get(&accessibility_node.node);
    if (!maybe_parent_node.has_value() || !maybe_parent_node.value())
        return {};

    auto const& parent_node = *maybe_parent_node.value();
    auto identifier = NodeIdentifier::for_node(parent_node);

    return Node { .node = parent_node, .identifier = move(identifier), .tab = tab.release_nonnull() };
}

Optional<Node> AccessibilityWalkerActor::root_accessibility_node_for(WeakPtr<AccessibilityWalkerActor> const& weak_walker)
{
    if (auto walker = weak_walker.strong_ref())
        return walker->root_accessibility_node();
    return {};
}

Optional<Node> AccessibilityWalkerActor::root_accessibility_node()
{
    auto tab = m_tab.strong_ref();
    if (!tab)
        return {};

    auto identifier = NodeIdentifier::for_node(m_accessibility_tree);

    return Node { .node = m_accessibility_tree, .identifier = move(identifier), .tab = tab.release_nonnull() };
}

void AccessibilityWalkerActor::populate_accessibility_tree_cache()
{
    m_node_to_parent_map.clear();
    m_actor_to_node_map.clear();
    m_node_id_to_actor_map.clear();
    populate_accessibility_tree_cache(m_accessibility_tree, nullptr);
}

void AccessibilityWalkerActor::populate_accessibility_tree_cache(JsonObject& node, JsonObject const* parent)
{
    auto const& node_actor = actor_for_node(node);
    node.set("actor"sv, node_actor.name());

    m_node_to_parent_map.set(&node, parent);
    m_actor_to_node_map.set(node_actor.name(), &node);
    m_node_id_to_actor_map.set(node_actor.node_identifier().id, node_actor.name());

    auto children = node.get_array("children"sv);
    if (!children.has_value())
        return;

    children->values().remove_all_matching([&](JsonValue const& child) {
        return !is_suitable_for_accessibility_inspection(child);
    });

    children->for_each([&](JsonValue& child) {
        populate_accessibility_tree_cache(child.as_object(), &node);
    });
}

AccessibilityNodeActor const& AccessibilityWalkerActor::actor_for_node(JsonObject const& node)
{
    auto identifier = NodeIdentifier::for_node(node);

    if (auto it = m_node_actors.find(identifier); it != m_node_actors.end()) {
        if (auto node_actor = it->value.strong_ref())
            return *node_actor;
    }

    auto& node_actor = devtools().register_actor<AccessibilityNodeActor>(identifier, *this);
    m_node_actors.set(identifier, node_actor);

    return node_actor;
}

}
