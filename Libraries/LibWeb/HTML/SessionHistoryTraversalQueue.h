/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibCore/Timer.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

struct SessionHistoryTraversalQueueEntry : public JS::Cell {
    GC_CELL(SessionHistoryTraversalQueueEntry, JS::Cell);
    GC_DECLARE_ALLOCATOR(SessionHistoryTraversalQueueEntry);

public:
    static GC::Ref<SessionHistoryTraversalQueueEntry> create(JS::VM& vm, GC::Ref<GC::Function<void()>> steps, GC::Ptr<HTML::Navigable> target_navigable);

    GC::Ptr<HTML::Navigable> target_navigable() const { return m_target_navigable; }
    void execute_steps() const { m_steps->function()(); }

private:
    SessionHistoryTraversalQueueEntry(GC::Ref<GC::Function<void()>> steps, GC::Ptr<HTML::Navigable> target_navigable)
        : m_steps(steps)
        , m_target_navigable(target_navigable)
    {
    }

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<GC::Function<void()>> m_steps;
    GC::Ptr<HTML::Navigable> m_target_navigable;
};

// https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-traversal-queue
class SessionHistoryTraversalQueue : public JS::Cell {
    GC_CELL(SessionHistoryTraversalQueue, JS::Cell);
    GC_DECLARE_ALLOCATOR(SessionHistoryTraversalQueue);

public:
    SessionHistoryTraversalQueue();

    void append(GC::Ref<GC::Function<void()>> steps);
    void append_sync(GC::Ref<GC::Function<void()>> steps, GC::Ptr<Navigable> target_navigable);

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigations-jump-queue
    GC::Ptr<SessionHistoryTraversalQueueEntry> first_synchronous_navigation_steps_with_target_navigable_not_contained_in(HashTable<GC::Ref<Navigable>> const&);

private:
    virtual void visit_edges(Cell::Visitor&) override;

    Vector<GC::Ref<SessionHistoryTraversalQueueEntry>> m_queue;
    RefPtr<Core::Timer> m_timer;
    bool m_is_task_running { false };
};

}
