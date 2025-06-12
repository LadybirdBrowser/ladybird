/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/RootVector.h>
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
}

void TaskQueue::add(GC::Ref<Task> task)
{
    m_tasks.append(task);
    m_event_loop->schedule();
}

GC::Ptr<Task> TaskQueue::take_first_runnable()
{
    if (m_event_loop->execution_paused())
        return nullptr;

    for (size_t i = 0; i < m_tasks.size(); ++i) {
        if (m_event_loop->running_rendering_task() && m_tasks[i]->source() == Task::Source::Rendering)
            continue;
        if (m_tasks[i]->is_runnable())
            return m_tasks.take(i);
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
    return false;
}

void TaskQueue::remove_tasks_matching(Function<bool(HTML::Task const&)> filter)
{
    m_tasks.remove_all_matching([&](auto& task) {
        return filter(*task);
    });
}

GC::RootVector<GC::Ref<Task>> TaskQueue::take_tasks_matching(Function<bool(HTML::Task const&)> filter)
{
    GC::RootVector<GC::Ref<Task>> matching_tasks(heap());

    for (size_t i = 0; i < m_tasks.size();) {
        auto& task = m_tasks.at(i);

        if (filter(*task)) {
            matching_tasks.append(task);
            m_tasks.remove(i);
        } else {
            ++i;
        }
    }

    return matching_tasks;
}

Task const* TaskQueue::last_added_task() const
{
    if (m_tasks.is_empty())
        return nullptr;
    return m_tasks.last();
}

bool TaskQueue::has_rendering_tasks() const
{
    for (auto const& task : m_tasks) {
        if (task->source() == Task::Source::Rendering)
            return true;
    }
    return false;
}

}
