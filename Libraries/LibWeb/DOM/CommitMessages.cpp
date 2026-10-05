/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/CommitMessages.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/SVG/SVGElement.h>

namespace Web::DOM {

void CommitMessages::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
}

// A container that has left the document since its box was committed has no navigable to size.
static HTML::LocalNavigable* local_content_navigable(Layout::Node& navigable_container_viewport)
{
    auto* container = as_if<HTML::NavigableContainer>(navigable_container_viewport.dom_node());
    return container ? as_if<HTML::LocalNavigable>(container->content_navigable().ptr()) : nullptr;
}

Layout::Node* CommitMessages::bound_layout_node(Layout::BegunRead const& read, NodeIdentity identity) const
{
    auto* arena = m_document->layout_node_arena_if_created();
    return arena ? identity.bound_layout_node(read, *arena) : nullptr;
}

// A message from layout names its node by the style node the style tree gave it, with 0 for the document.
void CommitMessages::append(Layout::BegunRead const& read, Layout::RustFFI::FfiCommitMessage const& message)
{
    auto identity = message.style_node == 0
        ? NodeIdentity::of_document()
        : NodeIdentity::of_style_node(CSS::StyleNodeID { message.style_node });
    switch (message.kind) {
    case Layout::RustFFI::FfiCommitMessageKind::ContentSizeChangedForContainerQueries:
        m_messages.append({ .identity = identity, .kind = Kind::ContentSizeChangedForContainerQueries });
        return;
    case Layout::RustFFI::FfiCommitMessageKind::NavigableContainerViewportCommitted:
        // A viewport the container's content navigable already has would change nothing where it is applied.
        if (auto* box = bound_layout_node(read, identity)) {
            if (auto* content_navigable = local_content_navigable(*box); content_navigable && content_navigable->viewport_size() == Painting::content_size(*box))
                return;
        }
        m_messages.append({ .identity = identity, .kind = Kind::NavigableContainerViewportCommitted });
        return;
    case Layout::RustFFI::FfiCommitMessageKind::LayoutTreeRebuildRequested:
        m_messages.append({
            .identity = identity,
            .kind = Kind::NeedsLayoutTreeUpdate,
            .layout_tree_update_reason = SetNeedsLayoutTreeUpdateReason::PseudoElementBoxEscapedRebuildRoot,
        });
        return;
    case Layout::RustFFI::FfiCommitMessageKind::TopLayerZoneRebuildNeeded:
        m_messages.append({ .identity = identity, .kind = Kind::TopLayerZoneRebuildNeeded });
        return;
    case Layout::RustFFI::FfiCommitMessageKind::ListItemCounterValueRendered:
        m_messages.append({ .identity = identity, .kind = Kind::ListItemCounterValueRendered });
        return;
    case Layout::RustFFI::FfiCommitMessageKind::UnstyledElementReached:
        m_messages.append({ .identity = identity, .kind = Kind::UnstyledElementReached });
        return;
    case Layout::RustFFI::FfiCommitMessageKind::SvgResourceReferenced:
        m_messages.append({
            .identity = identity,
            .other_identity = NodeIdentity::of_style_node(CSS::StyleNodeID { message.other_style_node }),
            .kind = Kind::SvgResourceReferenced,
        });
        return;
    }
    VERIFY_NOT_REACHED();
}

void CommitMessages::apply(Layout::BegunRead const& read)
{
    // A message appended while the list is being applied belongs after the ones in progress, and the loop below
    // reaches it there.
    if (m_applying)
        return;
    m_applying = true;
    ScopeGuard done = [&] { m_applying = false; };

    while (!m_messages.is_empty()) {
        auto messages = move(m_messages);
        for (auto const& message : messages)
            apply(read, message);
    }
}

void CommitMessages::apply(Layout::BegunRead const& read, Message const& message)
{
    switch (message.kind) {
    case Kind::ContentSizeChangedForContainerQueries:
        // Only an element can be a query container; the viewport names the document, which is not one. Layout says
        // this of every size container, but `container-type` is set far more widely than it is asked about, and one
        // no size query or container-relative unit resolved against has no dependent to record.
        if (auto* element = as_if<Element>(message.identity.resolve(m_document).ptr()); element && element->is_size_query_container())
            m_document->style_computer().style_engine().record_size_container_query_dependents(element->style_node_id());
        return;
    case Kind::NavigableContainerViewportCommitted:
        // A navigable another process hosts learns its viewport from the UI process, which the container tells of
        // the viewport's rect when its document is painted.
        if (auto* box = bound_layout_node(read, message.identity)) {
            if (auto* content_navigable = local_content_navigable(*box))
                content_navigable->set_viewport_size(Painting::content_size(*box));
        }
        return;
    case Kind::NeedsLayoutTreeUpdate:
        if (auto node = message.identity.resolve(m_document))
            node->set_needs_layout_tree_update(true, message.layout_tree_update_reason);
        return;
    case Kind::TopLayerZoneRebuildNeeded:
        m_document->set_top_layer_needs_layout_zone_rebuild();
        return;
    case Kind::ListItemCounterValueRendered:
        if (auto* element = as_if<Element>(message.identity.resolve(m_document).ptr()))
            m_document->did_render_list_item_counter_value(*element);
        return;
    case Kind::SvgResourceReferenced: {
        // Either element may have left the document since the build placed the resource box; the registration only
        // matters while both are still here.
        auto* resource = as_if<SVG::SVGElement>(message.identity.resolve(m_document).ptr());
        auto* referencing_element = as_if<Element>(message.other_identity.resolve(m_document).ptr());
        if (resource && referencing_element)
            resource->register_resource_box_referencing_element({}, *referencing_element);
        return;
    }
    case Kind::UnstyledElementReached:
        // A bypass path (top-layer iteration, slot projection, SVG mask/clip-path or pattern reference) reached an
        // element no style update settled. A targeted style update seeds the style computer's ancestor filter, so
        // descendant-combinator selectors match during its lazy re-cascade, and the next tree build gives the element
        // its box.
        if (auto* element = as_if<Element>(message.identity.resolve(m_document).ptr()); element && element->is_connected()) {
            if (!element->has_style())
                m_document->update_style_for_element({ *element });
            element->set_needs_layout_tree_update(true, SetNeedsLayoutTreeUpdateReason::StyleChange);
        }
        return;
    }
    VERIFY_NOT_REACHED();
}

}
