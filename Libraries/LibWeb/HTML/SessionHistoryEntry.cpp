/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Crypto/Crypto.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/DocumentState.h>
#include <LibWeb/HTML/SessionHistoryEntry.h>
#include <LibWeb/HTML/StructuredSerialize.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SessionHistoryEntry);

void SessionHistoryEntry::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document_state);
    visitor.visit(m_original_source_browsing_context);
    visitor.visit(m_policy_container);
}

SessionHistoryEntry::SessionHistoryEntry()
    : m_classic_history_api_state(MUST(structured_serialize_for_storage(vm(), JS::js_null())))
    , m_navigation_api_state(MUST(structured_serialize_for_storage(vm(), JS::js_undefined())))
    , m_navigation_api_key(Crypto::generate_random_uuid())
    , m_navigation_api_id(Crypto::generate_random_uuid())
{
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#she-document
GC::Ptr<DOM::Document> SessionHistoryEntry::document() const
{
    // To get a session history entry's document, return its document state's document.
    if (!m_document_state)
        return {};
    return m_document_state->document();
}

}
