/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/InsertedNodes.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/HTML/HTMLBRElement.h>

namespace Web::Editing {

InsertedNodes::InsertedNodes(GC::RootVector<GC::Ref<DOM::Node>>&& nodes)
    : m_nodes(move(nodes))
{
    if (!m_nodes.is_empty()) {
        m_first_node = m_nodes.first();
        m_last_node = m_nodes.last();
        if (is<DOM::Text>(*m_first_node))
            m_leading_content_kind = LeadingContentKind::TextNode;
        else if (m_first_node->first_child() && is<DOM::Text>(*m_first_node->first_child()))
            m_leading_content_kind = LeadingContentKind::UnwrappedParagraphText;

        auto* last_leaf = m_last_node.ptr();
        while (last_leaf->last_child())
            last_leaf = last_leaf->last_child();
        if (editing_ignores_content(*last_leaf) && !is<HTML::HTMLBRElement>(*last_leaf))
            m_last_atomic_node = *last_leaf;
    }

    for (auto& node : m_nodes) {
        node->for_each_in_inclusive_subtree([&](GC::Ref<DOM::Node> descendant) {
            if (is<DOM::Text>(*descendant))
                m_text_nodes.append(static_cast<DOM::Text&>(*descendant));
            return TraversalDecision::Continue;
        });
    }
}

GC::RootVector<GC::Ref<DOM::Node>> InsertedNodes::connected_nodes() const
{
    GC::RootVector<GC::Ref<DOM::Node>> nodes;
    for (auto& node : m_nodes) {
        if (node->parent())
            nodes.append(node);
    }
    return nodes;
}

GC::Ref<DOM::Node> InsertedNodes::first_node() const
{
    VERIFY(m_first_node);
    return *m_first_node;
}

GC::Ref<DOM::Node> InsertedNodes::last_node() const
{
    VERIFY(m_last_node);
    return *m_last_node;
}

GC::RootVector<GC::Ref<DOM::Text>> InsertedNodes::connected_text_nodes() const
{
    GC::RootVector<GC::Ref<DOM::Text>> text_nodes;
    for (auto& text : m_text_nodes) {
        if (text->parent())
            text_nodes.append(text);
    }
    return text_nodes;
}

bool InsertedNodes::contains(DOM::Text const& candidate) const
{
    return any_of(m_text_nodes, [&](auto const& text) { return text.ptr() == &candidate && text->parent(); });
}

void InsertedNodes::did_replace_node(DOM::Node& node, DOM::Node& replacement)
{
    did_replace_node(node, replacement, replacement);
}

void InsertedNodes::did_replace_node(DOM::Node& node, DOM::Node& first_replacement, DOM::Node& last_replacement)
{
    if (m_first_node.ptr() == &node)
        m_first_node = first_replacement;
    if (m_last_node.ptr() == &node)
        m_last_node = last_replacement;
}

}
