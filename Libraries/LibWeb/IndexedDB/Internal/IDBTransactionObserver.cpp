/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/IDBTransaction.h>
#include <LibWeb/IndexedDB/Internal/IDBTransactionObserver.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(IDBTransactionObserver);

IDBTransactionObserver::IDBTransactionObserver(IDBTransaction& transaction)
    : m_transaction(transaction)
{
    m_transaction->register_transaction_observer({}, *this);
    m_observing = true;
}

IDBTransactionObserver::~IDBTransactionObserver() = default;

void IDBTransactionObserver::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_transaction);
    visitor.visit(m_transaction_finished_observer);
}

void IDBTransactionObserver::finalize()
{
    Base::finalize();
    unobserve();
}

void IDBTransactionObserver::unobserve()
{
    if (!m_observing)
        return;

    m_transaction->unregister_transaction_observer({}, *this);
    m_observing = false;
}

}
