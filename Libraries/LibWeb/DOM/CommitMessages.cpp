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

namespace Web::DOM {

void CommitMessages::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_document);
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
    }
    VERIFY_NOT_REACHED();
}

}
