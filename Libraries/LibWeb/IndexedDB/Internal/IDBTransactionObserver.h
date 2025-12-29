/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibWeb/Forward.h>

namespace Web::IndexedDB {

class IDBTransactionObserver final : public GC::Cell {
    GC_CELL(IDBTransactionObserver, GC::Cell);
    GC_DECLARE_ALLOCATOR(IDBTransactionObserver);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~IDBTransactionObserver();

    [[nodiscard]] GC::Ptr<GC::Function<void()>> transaction_finished_observer() const { return m_transaction_finished_observer; }
    void set_transaction_finished_observer(GC::Ptr<GC::Function<void()>> callback) { m_transaction_finished_observer = callback; }

    GC::Ref<IDBTransaction> transaction() const { return m_transaction; }

    void unobserve();

private:
    explicit IDBTransactionObserver(IDBTransaction&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    bool m_observing { false };

    GC::Ref<IDBTransaction> m_transaction;
    GC::Ptr<GC::Function<void()>> m_transaction_finished_observer;
};

}
