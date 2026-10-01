/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibWeb/CSS/Invalidation/ContainerQueryInvalidator.h>
#include <LibWeb/DOM/CommitMessages.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>

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

Layout::Node* CommitMessages::bound_layout_node(NodeIdentity identity) const
{
    auto* arena = m_document->layout_node_arena_if_created();
    return arena ? identity.bound_layout_node(*arena) : nullptr;
}

// A message from layout names its node by the style node the style tree gave it, with 0 for the document.
void CommitMessages::append(Layout::RustFFI::FfiCommitMessage const& message)
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
        if (auto* box = bound_layout_node(identity)) {
            if (auto* content_navigable = local_content_navigable(*box); content_navigable && content_navigable->viewport_size() == Painting::content_size(*box))
                return;
        }
        m_messages.append({ .identity = identity, .kind = Kind::NavigableContainerViewportCommitted });
        return;
    }
    VERIFY_NOT_REACHED();
}

void CommitMessages::apply()
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
            apply(message);
    }
}

void CommitMessages::apply(Message const& message)
{
    switch (message.kind) {
    case Kind::ContentSizeChangedForContainerQueries:
        // Only an element can be a query container; the viewport names the document, which is not one.
        if (auto* element = as_if<Element>(message.identity.resolve(m_document).ptr()))
            CSS::Invalidation::invalidate_descendant_styles_depending_on_size_container_query(*element);
        return;
    case Kind::NavigableContainerViewportCommitted:
        // A navigable another process hosts learns its viewport from the UI process, which the container tells of
        // the viewport's rect when its document is painted.
        if (auto* box = bound_layout_node(message.identity)) {
            if (auto* content_navigable = local_content_navigable(*box))
                content_navigable->set_viewport_size(Painting::content_size(*box));
        }
        return;
    }
    VERIFY_NOT_REACHED();
}

}
