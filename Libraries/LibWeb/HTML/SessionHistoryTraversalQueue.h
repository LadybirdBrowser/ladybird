/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/BinaryHeap.h>
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
    static GC::Ref<SessionHistoryTraversalQueueEntry> create(JS::VM& vm, GC::Ref<GC::Function<void()>> steps, GC::Ptr<HTML::Navigable> target_navigable, int priority, size_t insertion_order);

    GC::Ptr<HTML::Navigable> target_navigable() const { return m_target_navigable; }
    void execute_steps() const { m_steps->function()(); }
    int priority() const { return m_priority; }
    int insertion_order() const { return m_insertion_order; }

private:
    SessionHistoryTraversalQueueEntry(GC::Ref<GC::Function<void()>> steps, GC::Ptr<HTML::Navigable> target_navigable, int priority, int insertion_order)
        : m_steps(steps)
        , m_target_navigable(target_navigable)
        , m_priority(priority)
        , m_insertion_order(insertion_order)
    {
    }

    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<GC::Function<void()>> m_steps;
    GC::Ptr<HTML::Navigable> m_target_navigable;
    int m_priority { 0 };
    int m_insertion_order { 0 };
};

// https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-traversal-queue
// AD-HOC: Since SessionHistoryTraversalQueue isn't actually parallel, use a priority queue instead to avoid deadlocks
//        when a traversable performs a cross-document navigation and a new document load creates a new child navigable.
class SessionHistoryTraversalQueue : public JS::Cell {
    GC_CELL(SessionHistoryTraversalQueue, JS::Cell);
    GC_DECLARE_ALLOCATOR(SessionHistoryTraversalQueue);

public:
    SessionHistoryTraversalQueue();

    void append(GC::Ref<GC::Function<void()>> steps, int priority = 0);
    void append_sync(GC::Ref<GC::Function<void()>> steps, GC::Ptr<Navigable> target_navigable, int priority = 0);

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigations-jump-queue
    GC::Ptr<SessionHistoryTraversalQueueEntry> first_synchronous_navigation_steps_with_target_navigable_not_contained_in(HashTable<GC::Ref<Navigable>> const&);

private:
    virtual void visit_edges(Cell::Visitor&) override;

    struct QueueKey {
        int priority;
        int insertion_order;
        bool operator<(QueueKey const& other) const
        {
            if (priority != other.priority)
                return priority > other.priority;
            return insertion_order < other.insertion_order;
        }
    };

    AK::BinaryHeap<QueueKey, GC::Ref<SessionHistoryTraversalQueueEntry>, 16> m_queue;
    RefPtr<Core::Timer> m_timer;
    bool m_is_task_running { false };
    QueueKey m_current_task_key { 0, 0 };
    int s_insertion_counter { 0 };
};

}
