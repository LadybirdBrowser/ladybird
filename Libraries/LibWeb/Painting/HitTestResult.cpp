/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/HitTestResult.h>

namespace Web::Painting {

DOM::Node* HitTestResult::dom_node() const
{
    auto* document = arena->document();
    if (!document)
        return nullptr;
    return node.resolve(*document).ptr();
}

Optional<DOM::BoundaryPoint> BoundaryIdentity::resolve(DOM::Document& document) const
{
    auto resolved_node = node.resolve(document);
    if (!resolved_node)
        return {};
    return DOM::BoundaryPoint { *resolved_node, offset };
}

GC::Ptr<DOM::Node> CaretPosition::boundary_node() const
{
    auto* document = arena->document();
    if (!document)
        return nullptr;
    return boundary.node.resolve(*document);
}

Optional<DOM::BoundaryPoint> CaretPosition::boundary_point() const
{
    auto* document = arena->document();
    if (!document)
        return {};
    return boundary.resolve(*document);
}

Layout::Node* CaretPosition::boundary_layout_node(Layout::BegunRead const& read) const
{
    return boundary.node.bound_layout_node(read, *arena);
}

}
