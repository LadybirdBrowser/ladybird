/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/ARIA/Roles.h>
#include <LibWeb/DOM/AccessibilityTreeNode.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLHeadElement.h>
#include <LibWebView/AccessibilityNodeData.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(AccessibilityTreeNode);

GC::Ref<AccessibilityTreeNode> AccessibilityTreeNode::create(Document* document, DOM::Node const* value)
{
    return document->realm().create<AccessibilityTreeNode>(value);
}

AccessibilityTreeNode::AccessibilityTreeNode(GC::Ptr<DOM::Node const> value)
    : m_value(value)
{
    m_children = {};
}

void AccessibilityTreeNode::serialize_tree_as_json(JsonObjectSerializer<StringBuilder>& object, Document const& document) const
{
    if (value()->is_document()) {
        VERIFY_NOT_REACHED();
    } else if (value()->is_element()) {
        auto const* element = static_cast<DOM::Element const*>(value().ptr());

        if (element->include_in_accessibility_tree()) {
            MUST(object.add("type"sv, "element"sv));

            auto role = element->role_or_default();
            bool has_role = role.has_value() && !ARIA::is_abstract_role(*role);

            auto name = MUST(element->accessible_name(document));
            MUST(object.add("name"sv, name));
            auto description = MUST(element->accessible_description(document));
            MUST(object.add("description"sv, description));

            if (has_role)
                MUST(object.add("role"sv, ARIA::role_name(*role)));
            else
                MUST(object.add("role"sv, ""sv));
        } else {
            VERIFY_NOT_REACHED();
        }
    } else if (value()->is_text()) {
        MUST(object.add("type"sv, "text"sv));

        auto const* text_node = static_cast<DOM::Text const*>(value().ptr());
        MUST(object.add("name"sv, text_node->data().to_utf8()));
        MUST(object.add("role"sv, "text leaf"sv));
    }

    MUST(object.add("id"sv, value()->unique_id().value()));

    if (value()->has_child_nodes()) {
        auto node_children = MUST(object.add_array("children"sv));
        for (auto& child : children()) {
            if (child->value()->is_uninteresting_whitespace_node())
                continue;
            JsonObjectSerializer<StringBuilder> child_object = MUST(node_children.add_object());
            child->serialize_tree_as_json(child_object, document);
            MUST(child_object.finish());
        }
        MUST(node_children.finish());
    }
}

void AccessibilityTreeNode::serialize_tree_as_node_data(Vector<WebView::AccessibilityNodeData>& out, Document const& document, i64 parent_id) const
{
    WebView::AccessibilityNodeData node_data;
    node_data.parent_id = parent_id;

    if (value()->is_element()) {
        auto const& element = static_cast<DOM::Element const&>(*value());

        node_data.id = static_cast<i64>(element.unique_id().value());
        auto role = element.role_or_default();
        if (role.has_value() && !ARIA::is_abstract_role(*role))
            node_data.role = MUST(String::from_utf8(ARIA::role_name(*role)));

        node_data.name = MUST(element.accessible_name(document));
        node_data.description = MUST(element.accessible_description(document));
        node_data.bounds = element.get_bounding_client_rect().to_type<int>();

        if (auto level = element.aria_level(); level.has_value()) {
            if (auto parsed = level->bytes_as_string_view().to_number<i32>(); parsed.has_value())
                node_data.heading_level = *parsed;
        }

        // Extract aria-live value (explicit attribute or implicit from role)
        if (auto live_attr = element.get_attribute(ARIA::AttributeNames::aria_live); live_attr.has_value()) {
            node_data.live = live_attr.release_value();
        } else if (role.has_value() && ARIA::is_live_region_role(*role)) {
            if (*role == ARIA::Role::alert)
                node_data.live = "assertive"_string;
            else
                node_data.live = "polite"_string;
        }

        if (document.active_element() == &element)
            node_data.is_focused = true;
    } else if (value()->is_text()) {
        auto const& text_node = static_cast<DOM::Text const&>(*value());
        node_data.id = static_cast<i64>(text_node.unique_id().value());
        node_data.role = "text leaf"_string;
        node_data.name = text_node.data().to_utf8();

        // Text nodes have no bounding rect of their own.
        // Use the parent element's bounds as an approximation.
        if (auto parent = text_node.parent_element())
            node_data.bounds = parent->get_bounding_client_rect().to_type<int>();
    }

    auto my_id = node_data.id;

    auto should_skip_child = [](AccessibilityTreeNode const& child) -> bool {
        if (child.value()->is_uninteresting_whitespace_node())
            return true;
        if (child.value()->is_text()) {
            for (auto* ancestor = child.value()->parent(); ancestor; ancestor = ancestor->parent()) {
                if (is<HTML::HTMLHeadElement>(*ancestor))
                    return true;
            }
        }
        return false;
    };

    for (auto const& child : children()) {
        if (should_skip_child(*child))
            continue;
        node_data.child_ids.append(static_cast<i64>(child->value()->unique_id().value()));
    }

    out.append(move(node_data));

    for (auto const& child : children()) {
        if (should_skip_child(*child))
            continue;
        child->serialize_tree_as_node_data(out, document, my_id);
    }
}

void AccessibilityTreeNode::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_value);
    visitor.visit(m_children);
}

}
