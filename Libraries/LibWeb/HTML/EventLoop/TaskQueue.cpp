/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ScopeGuard.h>
#include <LibGC/RootVector.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/EventLoop/TaskQueue.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(TaskQueue);

TaskQueue::TaskQueue(HTML::EventLoop& event_loop)
    : m_event_loop(event_loop)
{
}

TaskQueue::~TaskQueue() = default;

void TaskQueue::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_event_loop);
    for (auto& task : m_tasks)
        visitor.visit(task);
    for (auto& task : m_idle_tasks)
        visitor.visit(task);
    visitor.visit(m_last_added_task);
    visitor.visit(m_discarded_tasks);
}

void TaskQueue::add(GC::Ref<Task> task)
{
    // AD-HOC: Don't enqueue tasks for temporary (inert) documents used for fragment parsing.
    // FIXME: There's ongoing spec work to remove such documents: https://github.com/whatwg/html/pull/11970
    if (task->document() && task->document()->is_temporary_document_for_fragment_parsing()) {
        task->discard();
        return;
    }

    // AD-HOC: Don't enqueue a task for a destroyed document either: "destroy a document" removes the document's
    //         tasks from every task queue, and a destroyed document is never fully active again, so a task queued
    //         for it afterwards could never run. It would only sit in the queue — and queuing it wakes the event
    //         loop, which then scans the whole queue for a runnable task, but finds none.
    if (task->document() && task->document()->has_been_destroyed()) {
        task->discard();
        return;
    }

    m_last_added_task = task.ptr();
    if (task->priority() == Task::Priority::Idle)
        m_idle_tasks.append(*task);
    else
        m_tasks.append(*task);
    m_event_loop->schedule();
}

// A task that is runnable, and that no rendering update in flight holds back.
bool TaskQueue::is_runnable_now(Task const& task) const
{
    return task.is_runnable() && !m_event_loop->holds_tasks_of(task.document());
}

GC::Ptr<Task> TaskQueue::take_first_runnable()
{
    if (m_event_loop->execution_paused())
        return nullptr;

    ScopeGuard run_discard_steps_guard { [&] { run_discard_steps(); } };

    for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        auto& task = *it;

        if (m_event_loop->running_rendering_task() && task.source() == Task::Source::Rendering) {
            ++it;
            continue;
        }

        // While the frame of the last rendering task is in flight, the next keeps its place in the queue, and the
        // tasks after it wait behind it until the frame has been taken in.
        if (task.source() == Task::Source::Rendering && m_event_loop->holds_rendering_opportunity())
            return nullptr;

        if (is_runnable_now(task)) {
            if (m_last_added_task.ptr() == &task)
                m_last_added_task = {};
            it.erase();
            return &task;
        }

        if (task.is_permanently_unrunnable()) {
            ++it;
            remove_without_running(m_tasks, task);
            continue;
        }

        ++it;
    }

    for (auto it = m_idle_tasks.begin(); it != m_idle_tasks.end();) {
        auto& task = *it;

        if (is_runnable_now(task)) {
            if (m_last_added_task.ptr() == &task)
                m_last_added_task = {};
            it.erase();
            return &task;
        }

        if (task.is_permanently_unrunnable()) {
            ++it;
            remove_without_running(m_idle_tasks, task);
            continue;
        }

        ++it;
    }
    return nullptr;
}

bool TaskQueue::has_runnable_tasks() const
{
    if (m_event_loop->execution_paused())
        return false;

    for (auto& task : m_tasks) {
        if (m_event_loop->running_rendering_task() && task.source() == Task::Source::Rendering)
            continue;
        if (task.source() == Task::Source::Rendering && m_event_loop->holds_rendering_opportunity())
            return false;
        if (is_runnable_now(task))
            return true;
    }

    for (auto& task : m_idle_tasks) {
        if (is_runnable_now(task))
            return true;
    }
    return false;
}

void TaskQueue::remove_tasks_matching(Function<bool(HTML::Task const&)> filter)
{
    auto remove_matching_tasks = [&](auto& tasks) {
        for (auto it = tasks.begin(); it != tasks.end();) {
            auto& task = *it;
            ++it;
            if (filter(task))
                remove_without_running(tasks, task);
        }
    };
    remove_matching_tasks(m_tasks);
    remove_matching_tasks(m_idle_tasks);
    run_discard_steps();
}

void TaskQueue::remove_without_running(Task::Queue& tasks, Task& task)
{
    if (m_last_added_task.ptr() == &task)
        m_last_added_task = {};
    tasks.remove(task);
    m_discarded_tasks.append(task);
}

// NB: Discard steps run once the queue is no longer being walked, as they may queue or remove tasks.
void TaskQueue::run_discard_steps()
{
    while (!m_discarded_tasks.is_empty()) {
        auto discarded_tasks = move(m_discarded_tasks);
        for (auto& task : discarded_tasks)
            task->discard();
    }
}

Task const* TaskQueue::last_added_task() const
{
    return m_last_added_task.ptr();
}

bool TaskQueue::has_rendering_tasks() const
{
    for (auto const& task : m_tasks) {
        if (task.source() == Task::Source::Rendering)
            return true;
    }
    return false;
}

}
