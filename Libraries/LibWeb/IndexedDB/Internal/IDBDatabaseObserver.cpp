/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/IDBDatabase.h>
#include <LibWeb/IndexedDB/Internal/IDBDatabaseObserver.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBDatabaseObserver);

IDBDatabaseObserver::IDBDatabaseObserver(IDBDatabase& database)
    : m_database(database)
{
    m_database->register_database_observer({}, *this);
    m_observing = true;
}

IDBDatabaseObserver::~IDBDatabaseObserver() = default;

void IDBDatabaseObserver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_database);
    visitor.visit(m_connection_state_changed_observer);
}

void IDBDatabaseObserver::finalize()
{
    Base::finalize();
    unobserve();
}

void IDBDatabaseObserver::unobserve()
{
    if (!m_observing)
        return;

    m_database->unregister_database_observer({}, *this);
    m_observing = false;
}

}
