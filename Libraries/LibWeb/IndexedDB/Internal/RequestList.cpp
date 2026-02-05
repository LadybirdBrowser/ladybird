/*
 * Copyright (c) 2024, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/IndexedDB/Internal/Algorithms.h>
#include <LibWeb/IndexedDB/Internal/IDBRequestObserver.h>
#include <LibWeb/IndexedDB/Internal/RequestList.h>

namespace Web::IndexedDB {

GC_DEFINE_ALLOCATOR(RequestList::PendingRequestProcess);

void RequestList::all_requests_processed(GC::Heap& heap, GC::Ref<GC::Function<void()>> on_complete)
{
    GC::Ptr<PendingRequestProcess> pending_request_process;

    remove_all_matching([](auto const& entry) { return !entry; });
    for (auto const& entry : *this) {
        if (!entry->processed()) {
            if (!pending_request_process) {
                pending_request_process = heap.allocate<PendingRequestProcess>();
            }

            pending_request_process->add_request_to_observe(*entry);
        }
    }

    if (pending_request_process) {
        pending_request_process->after_all = GC::create_function(heap, [this, pending_request_process, on_complete] {
            VERIFY(!m_pending_request_queue.is_empty());
            bool was_removed = m_pending_request_queue.remove_first_matching([pending_request_process](GC::Root<PendingRequestProcess> const& stored_pending_connection_process) {
                return stored_pending_connection_process.ptr() == pending_request_process.ptr();
            });
            VERIFY(was_removed);
            queue_a_database_task(on_complete);
        });
        m_pending_request_queue.append(pending_request_process);
    } else {
        queue_a_database_task(on_complete);
    }
}

void RequestList::all_previous_requests_processed(GC::Heap& heap, GC::Ref<IDBRequest> const& request, GC::Ref<GC::Function<void()>> on_complete)
{
    GC::Ptr<PendingRequestProcess> pending_request_process;

    remove_all_matching([](auto const& entry) { return !entry; });
    for (auto const& entry : *this) {
        if (entry == request)
            break;

        if (!entry->processed()) {
            if (!pending_request_process) {
                pending_request_process = heap.allocate<PendingRequestProcess>();
            }

            pending_request_process->add_request_to_observe(*entry);
        }
    }

    if (pending_request_process) {
        pending_request_process->after_all = GC::create_function(heap, [this, pending_request_process, on_complete] {
            VERIFY(!m_pending_request_queue.is_empty());
            bool was_removed = m_pending_request_queue.remove_first_matching([pending_request_process](GC::Root<PendingRequestProcess> const& stored_pending_connection_process) {
                return stored_pending_connection_process.ptr() == pending_request_process.ptr();
            });
            VERIFY(was_removed);
            on_complete->function()();
        });
        m_pending_request_queue.append(*pending_request_process);
    } else {
        on_complete->function()();
    }
}

void RequestList::PendingRequestProcess::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(requests_waiting_on);
    visitor.visit(after_all);
}

void RequestList::PendingRequestProcess::add_request_to_observe(GC::Ref<IDBRequest> request)
{
    auto request_observer = heap().allocate<IDBRequestObserver>(request);
    request_observer->set_request_processed_changed_observer(GC::create_function(heap(), [this] {
        VERIFY(!requests_waiting_on.is_empty());
        requests_waiting_on.remove_all_matching([](GC::Ref<IDBRequestObserver> const& pending_request) {
            if (pending_request->request()->processed()) {
                pending_request->unobserve();
                return true;
            }

            return false;
        });

        if (requests_waiting_on.is_empty()) {
            after_all.as_nonnull()->function()();
        }
    }));

    requests_waiting_on.append(request_observer);
}

}
