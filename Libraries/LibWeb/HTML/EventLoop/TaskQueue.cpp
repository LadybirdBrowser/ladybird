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
    visitor.visit(m_tasks);
    visitor.visit(m_idle_tasks);
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
        m_idle_tasks.append(task);
    else
        m_tasks.append(task);
    m_event_loop->schedule();
}

GC::Ptr<Task> TaskQueue::dequeue()
{
    auto take_task = [&](auto& tasks) -> GC::Ptr<Task> {
        if (tasks.is_empty())
            return {};
        auto task = tasks.take_first();
        if (m_last_added_task == task.ptr())
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

    for (size_t i = 0; i < m_tasks.size();) {
        if (m_event_loop->running_rendering_task() && m_tasks[i]->source() == Task::Source::Rendering) {
            ++i;
            continue;
        }

        if (m_tasks[i]->is_runnable()) {
            if (m_last_added_task == m_tasks[i].ptr())
                m_last_added_task = {};
            return m_tasks.take(i);
        }

        if (m_tasks[i]->is_permanently_unrunnable()) {
            if (m_last_added_task == m_tasks[i].ptr())
                m_last_added_task = {};
            m_tasks.remove(i);
            continue;
        }

        ++i;
    }

    for (size_t i = 0; i < m_idle_tasks.size();) {
        if (m_idle_tasks[i]->is_runnable()) {
            if (m_last_added_task == m_idle_tasks[i].ptr())
                m_last_added_task = {};
            return m_idle_tasks.take(i);
        }

        if (m_idle_tasks[i]->is_permanently_unrunnable()) {
            if (m_last_added_task == m_idle_tasks[i].ptr())
                m_last_added_task = {};
            m_idle_tasks.remove(i);
            continue;
        }

        ++i;
    }
    return nullptr;
}

bool TaskQueue::has_runnable_tasks() const
{
    if (m_event_loop->execution_paused())
        return false;

    for (auto& task : m_tasks) {
        if (m_event_loop->running_rendering_task() && task->source() == Task::Source::Rendering)
            continue;
        if (task->is_runnable())
            return true;
    }

    for (auto& task : m_idle_tasks) {
        if (task->is_runnable())
            return true;
    }
    return false;
}

void TaskQueue::remove_tasks_matching(Function<bool(HTML::Task const&)> filter)
{
    auto wrapped_filter = [&](HTML::Task const& task) {
        if (!filter(task))
            return false;
        if (m_last_added_task == &task)
            m_last_added_task = {};
        return true;
    };
    m_tasks.remove_all_matching(wrapped_filter);
    m_idle_tasks.remove_all_matching(wrapped_filter);
}

GC::Ptr<Task> TaskQueue::take_first_runnable_matching(Function<bool(HTML::Task const&)> filter)
{
    for (size_t i = 0; i < m_tasks.size();) {
        auto& task = m_tasks.at(i);

        if (task->is_runnable() && filter(*task)) {
            if (m_last_added_task == task.ptr())
                m_last_added_task = {};
            return m_tasks.take(i);
        }

        if (task->is_permanently_unrunnable()) {
            if (m_last_added_task == task.ptr())
                m_last_added_task = {};
            m_tasks.remove(i);
            continue;
        }

        ++i;
    }

    for (size_t i = 0; i < m_idle_tasks.size();) {
        auto& task = m_idle_tasks.at(i);

        if (task->is_runnable() && filter(*task)) {
            if (m_last_added_task == task.ptr())
                m_last_added_task = {};
            return m_idle_tasks.take(i);
        }

        if (task->is_permanently_unrunnable()) {
            if (m_last_added_task == task.ptr())
                m_last_added_task = {};
            m_idle_tasks.remove(i);
            continue;
        }

        ++i;
    }

    return nullptr;
}

Task const* TaskQueue::last_added_task() const
{
    return m_last_added_task.ptr();
}

bool TaskQueue::has_rendering_tasks() const
{
    return m_tasks.contains([](auto const& task) { return task->source() == Task::Source::Rendering; });
}

}
