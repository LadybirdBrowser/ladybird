/*
 * Copyright (c) 2021-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

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
}

void TaskQueue::add(GC::Ref<Task> task)
{
    // AD-HOC: Don't enqueue tasks for temporary (inert) documents used for fragment parsing.
    // FIXME: There's ongoing spec work to remove such documents: https://github.com/whatwg/html/pull/11970
    if (task->document() && task->document()->is_temporary_document_for_fragment_parsing())
        return;

    m_last_added_task = task.ptr();
    if (task->source() == Task::Source::IdleTask)
        m_idle_tasks.append(*task);
    else
        m_tasks.append(*task);
    m_event_loop->schedule();
}

GC::Ptr<Task> TaskQueue::dequeue()
{
    auto take_task = [&](auto& tasks) -> GC::Ptr<Task> {
        if (tasks.is_empty())
            return {};
        auto* task = tasks.take_first();
        if (m_last_added_task == task)
            m_last_added_task = {};
        return task;
    };

    if (auto task = take_task(m_tasks))
        return task;
    return take_task(m_idle_tasks);
}

GC::Ptr<Task> TaskQueue::take_first_runnable()
{
    if (m_event_loop->execution_paused())
        return nullptr;

    for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        auto& task = *it;

        if (m_event_loop->running_rendering_task() && task.source() == Task::Source::Rendering) {
            ++it;
            continue;
        }

        if (task.is_runnable()) {
            if (m_last_added_task == &task)
                m_last_added_task = {};
            it.erase();
            return &task;
        }

        if (task.is_permanently_unrunnable()) {
            if (m_last_added_task == &task)
                m_last_added_task = {};
            it.erase();
            continue;
        }

        ++it;
    }

    for (auto it = m_idle_tasks.begin(); it != m_idle_tasks.end();) {
        auto& task = *it;

        if (task.is_runnable()) {
            if (m_last_added_task == &task)
                m_last_added_task = {};
            it.erase();
            return &task;
        }

        if (task.is_permanently_unrunnable()) {
            if (m_last_added_task == &task)
                m_last_added_task = {};
            it.erase();
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
        if (task.is_runnable())
            return true;
    }

    for (auto& task : m_idle_tasks) {
        if (task.is_runnable())
            return true;
    }
    return false;
}

void TaskQueue::remove_tasks_matching(Function<bool(HTML::Task const&)> filter)
{
    auto remove_matching_tasks = [&](auto& tasks) {
        for (auto it = tasks.begin(); it != tasks.end();) {
            auto& task = *it;
            if (!filter(task)) {
                ++it;
                continue;
            }
            if (m_last_added_task == &task)
                m_last_added_task = {};
            it.erase();
        }
    };
    remove_matching_tasks(m_tasks);
    remove_matching_tasks(m_idle_tasks);
}

GC::Ptr<Task> TaskQueue::take_first_runnable_matching(Function<bool(HTML::Task const&)> filter)
{
    for (auto it = m_tasks.begin(); it != m_tasks.end();) {
        auto& task = *it;

        if (task.is_runnable() && filter(task)) {
            if (m_last_added_task == &task)
                m_last_added_task = {};
            it.erase();
            return &task;
        }

        if (task.is_permanently_unrunnable()) {
            if (m_last_added_task == &task)
                m_last_added_task = {};
            it.erase();
            continue;
        }

        ++it;
    }

    for (auto it = m_idle_tasks.begin(); it != m_idle_tasks.end();) {
        auto& task = *it;

        if (task.is_runnable() && filter(task)) {
            if (m_last_added_task == &task)
                m_last_added_task = {};
            it.erase();
            return &task;
        }

        if (task.is_permanently_unrunnable()) {
            if (m_last_added_task == &task)
                m_last_added_task = {};
            it.erase();
            continue;
        }

        ++it;
    }

    return nullptr;
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
