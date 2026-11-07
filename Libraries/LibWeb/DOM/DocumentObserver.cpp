/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(DocumentObserver);

DocumentObserver::DocumentObserver(Document& document)
    : m_document(document)
{
    m_document->register_document_observer({}, *this);
}

GC::Ref<DocumentObserver> DocumentObserver::create(Document& document)
{
    return GC::Heap::the().allocate<DocumentObserver>(document);
}

void DocumentObserver::visit_edges(GC::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_document_became_active);
    visitor.visit(m_document_became_inactive);
    visitor.visit(m_document_completely_loaded);
    visitor.visit(m_document_readiness_observer);
    visitor.visit(m_document_visibility_state_observer);
    visitor.visit(m_document_page_showing_observer);
}

void DocumentObserver::finalize()
{
    Base::finalize();
    m_document->unregister_document_observer({}, *this);
}

void DocumentObserver::set_document(GC::Ref<Document> document)
{
    VERIFY(document != m_document);
    m_document->unregister_document_observer({}, *this);
    m_document = document;
    document->register_document_observer({}, *this);
}

void DocumentObserver::retarget_for_adoption(GC::Ref<Document> new_document)
{
    bool was_fully_active = m_document->is_fully_active();
    set_document(new_document);
    bool is_fully_active = new_document->is_fully_active();

    // NB: If the observed element moved between a fully active document and a document that is not fully active,
    //     synthesize the matching activity transition, so that observers see the same sequence of transitions as if
    //     the element had stayed put and its document had changed activity.
    if (was_fully_active == is_fully_active)
        return;
    if (is_fully_active) {
        if (m_document_became_active)
            m_document_became_active->function()();
    } else {
        if (m_document_became_inactive)
            m_document_became_inactive->function()();
    }
}

void DocumentObserver::set_document_became_active(GC::Ptr<GC::Function<void()>> callback)
{
    m_document_became_active = callback;
}

void DocumentObserver::set_document_became_inactive(GC::Ptr<GC::Function<void()>> callback)
{
    m_document_became_inactive = callback;
}

void DocumentObserver::set_document_completely_loaded(GC::Ptr<GC::Function<void()>> callback)
{
    m_document_completely_loaded = callback;
}

void DocumentObserver::set_document_readiness_observer(GC::Ptr<GC::Function<void(HTML::DocumentReadyState)>> callback)
{
    m_document_readiness_observer = callback;
}

void DocumentObserver::set_document_visibility_state_observer(GC::Ptr<GC::Function<void(HTML::VisibilityState)>> callback)
{
    m_document_visibility_state_observer = callback;
}

}
