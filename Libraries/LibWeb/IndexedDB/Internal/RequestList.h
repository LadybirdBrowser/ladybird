/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Root.h>
#include <LibWeb/IndexedDB/IDBRequest.h>

namespace Web::IndexedDB {

class RequestList final : public AK::Vector<GC::Weak<IDBRequest>> {
    AK_MAKE_NONMOVABLE(RequestList);
    AK_MAKE_NONCOPYABLE(RequestList);

public:
    RequestList() = default;

    void all_requests_processed(GC::Heap&, GC::Ref<GC::Function<void()>> on_complete);
    void all_previous_requests_processed(GC::Heap&, GC::Ref<IDBRequest> const& request, GC::Ref<GC::Function<void()>> on_complete);

private:
    struct PendingRequestProcess final : public GC::Cell {
        GC_CELL(PendingRequestProcess, GC::Cell);
        GC_DECLARE_ALLOCATOR(PendingRequestProcess);

        virtual void visit_edges(Cell::Visitor&) override;

        void add_request_to_observe(GC::Ref<IDBRequest>);

        Vector<GC::Ref<IDBRequestObserver>> requests_waiting_on;
        GC::Ptr<GC::Function<void()>> after_all;
    };

    Vector<GC::Root<PendingRequestProcess>> m_pending_request_queue;
};

}
