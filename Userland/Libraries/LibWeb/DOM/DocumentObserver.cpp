/*
 * Copyright (c) 2023-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/DocumentObserver.h>

namespace Web::DOM {

JS_DEFINE_ALLOCATOR(DocumentObserver);

DocumentObserver::DocumentObserver(JS::Realm& realm, DOM::Document& document)
    : Bindings::PlatformObject(realm)
    , m_document(document)
{
    m_document->register_document_observer({}, *this);
}

void DocumentObserver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_document_became_inactive);
    visitor.visit(m_document_completely_loaded);
    visitor.visit(m_document_readiness_observer);
}

void DocumentObserver::finalize()
{
    Base::finalize();
    m_document->unregister_document_observer({}, *this);
}

void DocumentObserver::set_document_became_inactive(Function<void()> callback)
{
    if (callback)
        m_document_became_inactive = JS::create_heap_function(vm().heap(), move(callback));
    else
        m_document_became_inactive = nullptr;
}

void DocumentObserver::set_document_completely_loaded(Function<void()> callback)
{
    if (callback)
        m_document_completely_loaded = JS::create_heap_function(vm().heap(), move(callback));
    else
        m_document_completely_loaded = nullptr;
}

void DocumentObserver::set_document_readiness_observer(Function<void(HTML::DocumentReadyState)> callback)
{
    if (callback)
        m_document_readiness_observer = JS::create_heap_function(vm().heap(), move(callback));
    else
        m_document_readiness_observer = nullptr;
}

}
