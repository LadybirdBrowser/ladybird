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

class IDBRequestObserver final : public GC::Cell {
    GC_CELL(IDBRequestObserver, GC::Cell);
    GC_DECLARE_ALLOCATOR(IDBRequestObserver);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~IDBRequestObserver();

    [[nodiscard]] GC::Ptr<GC::Function<void()>> request_processed_changed_observer() const { return m_request_processed_changed_observer; }
    void set_request_processed_changed_observer(GC::Ptr<GC::Function<void()>> callback) { m_request_processed_changed_observer = callback; }

    GC::Ref<IDBRequest> request() const { return m_request; }

    void unobserve();

private:
    explicit IDBRequestObserver(IDBRequest&);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    bool m_observing { false };

    GC::Ref<IDBRequest> m_request;
    GC::Ptr<GC::Function<void()>> m_request_processed_changed_observer;
};

}
