/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Cell.h>
#include <LibGC/CellAllocator.h>
#include <LibWeb/Forward.h>
#include <LibWeb/IndexedDB/ConnectionState.h>

namespace Web::IndexedDB {

class IDBDatabaseObserver final : public GC::Cell {
    GC_CELL(IDBDatabaseObserver, GC::Cell);
    GC_DECLARE_ALLOCATOR(IDBDatabaseObserver);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~IDBDatabaseObserver();

    [[nodiscard]] GC::Ptr<GC::Function<void()>> connection_state_changed_observer() const { return m_connection_state_changed_observer; }
    void set_connection_state_changed_observer(GC::Ptr<GC::Function<void()>> callback) { m_connection_state_changed_observer = callback; }

    GC::Ref<IDBDatabase> database() const { return m_database; }

    void unobserve();

private:
    explicit IDBDatabaseObserver(IDBDatabase&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    bool m_observing { false };

    GC::Ref<IDBDatabase> m_database;
    GC::Ptr<GC::Function<void()>> m_connection_state_changed_observer;
};

}
