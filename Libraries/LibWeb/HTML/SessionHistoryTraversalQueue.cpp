/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/HTML/SessionHistoryTraversalQueue.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(SessionHistoryTraversalQueue);
GC_DEFINE_ALLOCATOR(SessionHistoryTraversalQueueEntry);

GC::Ref<SessionHistoryTraversalQueueEntry> SessionHistoryTraversalQueueEntry::create(JS::VM& vm, GC::Ref<GC::Function<void()>> steps, GC::Ptr<HTML::Navigable> target_navigable, GC::Ptr<DOM::Document> document)
{
    return vm.heap().allocate<SessionHistoryTraversalQueueEntry>(steps, target_navigable, document);
}

void SessionHistoryTraversalQueueEntry::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_steps);
    visitor.visit(m_target_navigable);
    visitor.visit(m_document);
}

SessionHistoryTraversalQueue::SessionHistoryTraversalQueue()
{
    m_timer = Core::Timer::create_single_shot(0, [this] {
        if (m_is_task_running && m_queue.size() > 0) {
            m_timer->start();
            return;
        }

        while (m_queue.size() > 0) {
            // Note: Check if current entry's load_document finished before executing the next entry
            if (m_current_document && m_current_document->ready_state() == "loading" && m_current_document->is_active()) {
                m_timer->start();
                return;
            }

            m_is_task_running = true;
            m_current_document = nullptr;
            auto entry = m_queue.take_first();
            m_current_document = entry->document();
            entry->execute_steps();
            m_is_task_running = false;
        }

        m_current_document = nullptr;
    });
}

void SessionHistoryTraversalQueue::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_queue);
    visitor.visit(m_current_document);
}

void SessionHistoryTraversalQueue::append(GC::Ref<GC::Function<void()>> steps, GC::Ptr<DOM::Document> document)
{
    m_queue.append(SessionHistoryTraversalQueueEntry::create(vm(), steps, nullptr, document));
    if (!m_timer->is_active()) {
        m_timer->start();
    }
}

void SessionHistoryTraversalQueue::append_sync(GC::Ref<GC::Function<void()>> steps, GC::Ptr<Navigable> target_navigable)
{
    m_queue.append(SessionHistoryTraversalQueueEntry::create(vm(), steps, target_navigable, nullptr));
    if (!m_timer->is_active()) {
        m_timer->start();
    }
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigations-jump-queue
GC::Ptr<SessionHistoryTraversalQueueEntry> SessionHistoryTraversalQueue::first_synchronous_navigation_steps_with_target_navigable_not_contained_in(HashTable<GC::Ref<Navigable>> const& set)
{
    auto index = m_queue.find_first_index_if([&set](auto const& entry) -> bool {
        return (entry->target_navigable() != nullptr) && !set.contains(*entry->target_navigable());
    });
    if (index.has_value())
        return m_queue.take(*index);
    return {};
}

}
