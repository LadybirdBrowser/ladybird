/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/NodeIdentity.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>

namespace Web::DOM {

NodeIdentity NodeIdentity::of_style_node(CSS::StyleNodeID style_node)
{
    if (style_node == 0)
        return {};
    return { Kind::StyleNode, style_node };
}

NodeIdentity NodeIdentity::of(Node const& node)
{
    if (node.is_document())
        return of_document();
    if (auto const* shadow_root = as_if<ShadowRoot>(node))
        return of_style_node(shadow_root->style_node_id());
    return of_style_node(Layout::Node::style_node_of(&node));
}

GC::Ptr<Node> NodeIdentity::resolve(Document& document) const
{
    switch (m_kind) {
    case Kind::None:
        return nullptr;
    case Kind::StyleNode:
        return document.style_computer().node_for_style_node(m_style_node);
    case Kind::Document:
        return document;
    }
    VERIFY_NOT_REACHED();
}

Layout::Node* NodeIdentity::bound_layout_node(Layout::BegunRead const& read, Layout::NodeArena& arena) const
{
    switch (m_kind) {
    case Kind::None:
        return nullptr;
    case Kind::StyleNode:
        return static_cast<Layout::Node*>(Layout::RustFFI::layout_row_bound_shell(arena.host(), &read, m_style_node.value()));
    case Kind::Document:
        return static_cast<Layout::Node*>(Layout::RustFFI::layout_row_bound_viewport_shell(arena.host(), &read));
    }
    VERIFY_NOT_REACHED();
}

bool NodeIdentity::binds(Layout::Node const& layout_node) const
{
    switch (m_kind) {
    case Kind::None:
        return false;
    case Kind::StyleNode:
    case Kind::Document:
        return Layout::RustFFI::layout_row_is_bound_to(layout_node.document_host(), Layout::Node::slot_id(&layout_node), style_node().value());
    }
    VERIFY_NOT_REACHED();
}

}
