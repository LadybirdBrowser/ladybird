/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#include <LibWeb/Forward.h>

namespace Web::Editing {

// Track the structural extent of a fragment while replacement preparation unwraps or replaces its nodes. These nodes
// are not the semantic replacement range: once preparation finishes, independent mutation-tracked endpoints own the
// inserted range just as Blink and WebKit switch from InsertedNodes to Positions before paragraph integration.
class InsertedNodes {
    AK_MAKE_NONCOPYABLE(InsertedNodes);

public:
    enum class LeadingContentKind {
        Other,
        TextNode,
        UnwrappedParagraphText,
    };

    explicit InsertedNodes(GC::RootVector<GC::Ref<DOM::Node>>&&);
    InsertedNodes(InsertedNodes&&) = default;

    bool is_empty() const { return m_nodes.is_empty(); }
    GC::RootVector<GC::Ref<DOM::Node>> connected_nodes() const;
    GC::Ref<DOM::Node> first_node() const;
    GC::Ref<DOM::Node> last_node() const;
    GC::Ptr<DOM::Node> last_atomic_node() const { return m_last_atomic_node.ptr(); }
    GC::RootVector<GC::Ref<DOM::Text>> connected_text_nodes() const;
    bool contains(DOM::Text const&) const;
    LeadingContentKind leading_content_kind() const { return m_leading_content_kind; }

    void did_replace_node(DOM::Node&, DOM::Node& replacement);
    void did_replace_node(DOM::Node&, DOM::Node& first_replacement, DOM::Node& last_replacement);

private:
    GC::RootVector<GC::Ref<DOM::Node>> m_nodes;
    GC::RootVector<GC::Ref<DOM::Text>> m_text_nodes;
    GC::Root<DOM::Node> m_first_node;
    GC::Root<DOM::Node> m_last_node;
    GC::Root<DOM::Node> m_last_atomic_node;
    LeadingContentKind m_leading_content_kind { LeadingContentKind::Other };
};

}
