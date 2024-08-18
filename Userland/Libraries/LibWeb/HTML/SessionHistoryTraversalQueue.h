/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibCore/Timer.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Heap/CellAllocator.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibJS/Heap/HeapFunction.h>
#include <LibJS/SafeFunction.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

struct SessionHistoryTraversalQueueEntry : public JS::Cell {
    JS_CELL(SessionHistoryTraversalQueueEntry, JS::Cell);
    JS_DECLARE_ALLOCATOR(SessionHistoryTraversalQueueEntry);

public:
    static JS::NonnullGCPtr<SessionHistoryTraversalQueueEntry> create(JS::VM& vm, JS::NonnullGCPtr<JS::HeapFunction<void()>> steps, JS::GCPtr<HTML::Navigable> target_navigable);

    JS::GCPtr<HTML::Navigable> target_navigable() const { return m_target_navigable; }
    void execute_steps() const { m_steps->function()(); }

private:
    SessionHistoryTraversalQueueEntry(JS::NonnullGCPtr<JS::HeapFunction<void()>> steps, JS::GCPtr<HTML::Navigable> target_navigable)
        : m_steps(steps)
        , m_target_navigable(target_navigable)
    {
    }

    virtual void visit_edges(Cell::Visitor&) override;

    JS::NonnullGCPtr<JS::HeapFunction<void()>> m_steps;
    JS::GCPtr<HTML::Navigable> m_target_navigable;
};

// https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-traversal-queue
class SessionHistoryTraversalQueue : public JS::Cell {
    JS_CELL(SessionHistoryTraversalQueue, JS::Cell);
    JS_DECLARE_ALLOCATOR(SessionHistoryTraversalQueue);

public:
    SessionHistoryTraversalQueue();

    void append(JS::NonnullGCPtr<JS::HeapFunction<void()>> steps);
    void append_sync(JS::NonnullGCPtr<JS::HeapFunction<void()>> steps, JS::GCPtr<Navigable> target_navigable);

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigations-jump-queue
    JS::GCPtr<SessionHistoryTraversalQueueEntry> first_synchronous_navigation_steps_with_target_navigable_not_contained_in(HashTable<JS::NonnullGCPtr<Navigable>> const&);

private:
    virtual void visit_edges(Cell::Visitor&) override;

    Vector<JS::NonnullGCPtr<SessionHistoryTraversalQueueEntry>> m_queue;
    RefPtr<Core::Timer> m_timer;
    bool m_is_task_running { false };
};

}
