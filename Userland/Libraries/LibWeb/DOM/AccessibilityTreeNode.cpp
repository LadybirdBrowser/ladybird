/*
 * Copyright (c) 2022, Jonah Shafran <jonahshafran@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/AccessibilityTreeNode.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Text.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(AccessibilityTreeNode);

JS::NonnullGCPtr<AccessibilityTreeNode> AccessibilityTreeNode::create(Document* document, DOM::Node const* value)
{
    return document->heap().allocate<AccessibilityTreeNode>(document->realm(), value);
}

AccessibilityTreeNode::AccessibilityTreeNode(JS::GCPtr<DOM::Node const> value)
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
            MUST(object.add("id"sv, element->unique_id().value()));

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
        MUST(object.add("text"sv, text_node->data()));
    }

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

void AccessibilityTreeNode::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_value);
    visitor.visit(m_children);
}

}
