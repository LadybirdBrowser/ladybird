/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/SessionHistoryTraversalQueue.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SessionHistoryTraversalQueue);
GC_DEFINE_ALLOCATOR(SessionHistoryTraversalQueueEntry);

GC::Ref<SessionHistoryTraversalQueueEntry> SessionHistoryTraversalQueueEntry::create(JS::VM& vm, GC::Ref<GC::Function<void()>> steps, GC::Ptr<HTML::Navigable> target_navigable, int priority, size_t insertion_order)
{
    return vm.heap().allocate<SessionHistoryTraversalQueueEntry>(steps, target_navigable, priority, insertion_order);
}

void SessionHistoryTraversalQueueEntry::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_steps);
    visitor.visit(m_target_navigable);
}

SessionHistoryTraversalQueue::SessionHistoryTraversalQueue()
{
    m_timer = Core::Timer::create_single_shot(0, [this] {
        if (m_is_task_running && !m_queue.is_empty()) {
            auto const& min_key = m_queue.peek_min_key();
            if (min_key < m_current_task_key) {
                while (!m_queue.is_empty()) {
                    auto entry = m_queue.pop_min();
                    m_current_task_key = min_key;
                    entry->execute_steps();
                }
            } else {
                m_timer->start();
            }
            return;
        }
        while (!m_queue.is_empty()) {
            m_is_task_running = true;
            auto min_key = m_queue.peek_min_key();
            auto entry = m_queue.pop_min();
            m_current_task_key = min_key;
            entry->execute_steps();
            m_current_task_key = QueueKey { 0, 0 };
            m_is_task_running = false;
        }
    });
}

void SessionHistoryTraversalQueue::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    Vector<GC::Ref<SessionHistoryTraversalQueueEntry>> temp;
    while (!m_queue.is_empty()) {
        auto entry = m_queue.pop_min();
        visitor.visit(entry);
        temp.append(entry);
    }
    for (auto& entry : temp) {
        m_queue.insert({ entry->priority(), entry->insertion_order() }, entry);
    }
}

void SessionHistoryTraversalQueue::append(GC::Ref<GC::Function<void()>> steps, int priority)
{
    int insertion_order = ++s_insertion_counter;
    m_queue.insert({ priority, insertion_order }, SessionHistoryTraversalQueueEntry::create(vm(), steps, nullptr, priority, insertion_order));
    if (!m_timer->is_active()) {
        m_timer->start();
    }
}

void SessionHistoryTraversalQueue::append_sync(GC::Ref<GC::Function<void()>> steps, GC::Ptr<Navigable> target_navigable, int priority)
{
    int insertion_order = ++s_insertion_counter;
    m_queue.insert({ priority, insertion_order }, SessionHistoryTraversalQueueEntry::create(vm(), steps, target_navigable, priority, insertion_order));
    if (!m_timer->is_active()) {
        m_timer->start();
    }
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigations-jump-queue
GC::Ptr<SessionHistoryTraversalQueueEntry> SessionHistoryTraversalQueue::first_synchronous_navigation_steps_with_target_navigable_not_contained_in(HashTable<GC::Ref<Navigable>> const& set)
{
    Vector<std::pair<SessionHistoryTraversalQueue::QueueKey, GC::Ref<SessionHistoryTraversalQueueEntry>>> temp;
    GC::Ptr<SessionHistoryTraversalQueueEntry> found_entry = nullptr;

    while (!m_queue.is_empty()) {
        auto key = m_queue.peek_min_key();
        auto entry = m_queue.pop_min();
        if (!found_entry && (entry->target_navigable() != nullptr) && !set.contains(*entry->target_navigable())) {
            found_entry = entry;
        } else {
            temp.append({ key, entry });
        }
    }
    for (auto& pair : temp) {
        m_queue.insert(pair.first, pair.second);
    }
    return found_entry;
}

}
