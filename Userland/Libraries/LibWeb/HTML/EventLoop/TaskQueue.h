/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Queue.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/HTML/EventLoop/Task.h>

namespace Web::HTML {

class TaskQueue : public JS::Cell {
    JS_CELL(TaskQueue, JS::Cell);
    JS_DECLARE_ALLOCATOR(TaskQueue);

public:
    explicit TaskQueue(HTML::EventLoop&);
    virtual ~TaskQueue() override;

    bool is_empty() const { return m_tasks.is_empty(); }

    bool has_runnable_tasks() const;

    void add(JS::NonnullGCPtr<HTML::Task>);
    JS::GCPtr<HTML::Task> take_first_runnable();

    void enqueue(JS::NonnullGCPtr<HTML::Task> task) { add(task); }
    JS::GCPtr<HTML::Task> dequeue()
    {
        if (m_tasks.is_empty())
            return {};
        return m_tasks.take_first();
    }

    void remove_tasks_matching(Function<bool(HTML::Task const&)>);
    JS::MarkedVector<JS::NonnullGCPtr<Task>> take_tasks_matching(Function<bool(HTML::Task const&)>);

    Task const* last_added_task() const;

private:
    virtual void visit_edges(Visitor&) override;

    JS::NonnullGCPtr<HTML::EventLoop> m_event_loop;

    Vector<JS::NonnullGCPtr<HTML::Task>> m_tasks;
};

}
