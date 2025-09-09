/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/IDBRequest.h>
#include <LibWeb/IndexedDB/Internal/IDBRequestObserver.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBRequestObserver);

IDBRequestObserver::IDBRequestObserver(IDBRequest& request)
    : m_request(request)
{
    m_request->register_request_observer({}, *this);
    m_observing = true;
}

IDBRequestObserver::~IDBRequestObserver() = default;

void IDBRequestObserver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_request);
    visitor.visit(m_request_processed_changed_observer);
}

void IDBRequestObserver::finalize()
{
    Base::finalize();
    unobserve();
}

void IDBRequestObserver::unobserve()
{
    if (!m_observing)
        return;

    m_request->unregister_request_observer({}, *this);
    m_observing = false;
}

}
