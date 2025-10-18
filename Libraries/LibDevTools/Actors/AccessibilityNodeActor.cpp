/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Enumerate.h>
#include <LibDevTools/Actors/AccessibilityNodeActor.h>
#include <LibDevTools/Actors/AccessibilityWalkerActor.h>

namespace DevTools {

NonnullRefPtr<AccessibilityNodeActor> AccessibilityNodeActor::create(DevToolsServer& devtools, String name, NodeIdentifier node_identifier, WeakPtr<AccessibilityWalkerActor> walker)
{
    return adopt_ref(*new AccessibilityNodeActor(devtools, move(name), move(node_identifier), move(walker)));
}

AccessibilityNodeActor::AccessibilityNodeActor(DevToolsServer& devtools, String name, NodeIdentifier node_identifier, WeakPtr<AccessibilityWalkerActor> walker)
    : Actor(devtools, move(name))
    , m_node_identifier(node_identifier)
    , m_walker(move(walker))
{
}

AccessibilityNodeActor::~AccessibilityNodeActor() = default;

void AccessibilityNodeActor::handle_message(Message const& message)
{
    if (message.type == "audit"sv) {
        // FIXME: Implement accessibility audits.
        JsonObject audit;

        JsonObject response;
        response.set("type"sv, "audited"sv);
        response.set("audit"sv, audit);
        send_response(message, move(response));

        // For whatever reason, we need to send this a second time with no `type`.
        JsonObject second_response;
        second_response.set("audit"sv, audit);
        send_message(move(second_response));
        return;
    }

    if (message.type == "children"sv) {
        if (auto walker = m_walker.strong_ref()) {
            auto ancestor_node = walker->accessibility_node(name());
            if (!ancestor_node.has_value()) {
                send_unknown_actor_error(message, name());
                return;
            }

            JsonArray children;
            if (auto child_nodes = ancestor_node->node.get_array("children"sv); child_nodes.has_value()) {
                child_nodes->for_each([&](JsonValue const& child) {
                    children.must_append(walker->serialize_node(child.as_object()));
                });
            }

            JsonObject response;
            response.set("children"sv, move(children));
            send_response(message, move(response));
            return;
        }
        send_unknown_actor_error(message, name());
        return;
    }

    if (message.type == "getRelations"sv) {
        if (auto walker = m_walker.strong_ref()) {
            auto accessibility_node = walker->accessibility_node(name());
            if (!accessibility_node.has_value()) {
                send_unknown_actor_error(message, name());
                return;
            }

            auto root_node = walker->root_accessibility_node();

            JsonArray relations;

            auto report_relation = [&](StringView type, Node const& node) {
                JsonArray targets;
                MUST(targets.append(walker->serialize_node(node.node)));

                JsonObject relation;
                relation.set("targets"sv, move(targets));
                relation.set("type"sv, type);

                MUST(relations.append(move(relation)));
            };

            // For the root node, list itself as an "embeds" relation.
            if (root_node.has_value() && root_node->identifier == accessibility_node->identifier)
                report_relation("embeds"sv, root_node.value());

            // For all nodes, list the root as the "containing document" relation.
            report_relation("containing document"sv, root_node.value());

            // FIXME: Figure out what other relations we need to report here.

            JsonObject response;
            response.set("relations"sv, move(relations));
            send_response(message, move(response));
            return;
        }
        send_unknown_actor_error(message, name());
        return;
    }

    if (message.type == "hydrate"sv) {
        auto accessibility_node = AccessibilityWalkerActor::accessibility_node_for(m_walker, name());
        if (!accessibility_node.has_value()) {
            send_unknown_actor_error(message, name());
            return;
        }

        auto parent_node = AccessibilityWalkerActor::parent_of_accessibility_node_for(m_walker, *accessibility_node);

        auto const& node_json = accessibility_node->node;
        auto dom_node_type = parse_dom_node_type(node_json.get_string("type"sv).value());

        auto index_in_parent = 0;
        if (parent_node.has_value()) {
            if (auto parent_children = parent_node->node.get_array("children"sv); parent_children.has_value()) {
                for (auto const& [index, child] : enumerate(parent_children->values())) {
                    if (child.as_object().get_i64("id"sv) == accessibility_node->identifier.id) {
                        index_in_parent = index;
                        break;
                    }
                }
            }
        }

        // FIXME: Populate these.
        JsonArray actions;
        JsonObject attributes;
        JsonArray states;

        JsonObject properties;
        properties.set("actions"sv, move(actions));
        properties.set("attributes"sv, move(attributes));
        properties.set("description"sv, node_json.get_string("description"sv).value_or({}));
        properties.set("domNodeType"sv, to_underlying(dom_node_type));
        properties.set("indexInParent"sv, index_in_parent);
        // FIXME: Value of the accesskey attribute
        properties.set("keyboardShortcut"sv, ""sv);
        properties.set("states"sv, move(states));
        // FIXME: Implement
        properties.set("value"sv, ""sv);

        JsonObject response;
        response.set("properties"sv, move(properties));
        send_response(message, move(response));
        return;
    }

    send_unrecognized_packet_type_error(message);
}

}
