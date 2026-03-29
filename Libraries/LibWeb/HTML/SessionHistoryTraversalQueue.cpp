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

GC::Ref<SessionHistoryTraversalQueueEntry> SessionHistoryTraversalQueueEntry::create(JS::VM& vm, GC::Ref<SessionHistoryTraversalSteps> steps, GC::Ptr<HTML::Navigable> target_navigable)
{
    return vm.heap().allocate<SessionHistoryTraversalQueueEntry>(steps, target_navigable);
}

void SessionHistoryTraversalQueueEntry::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_steps);
    visitor.visit(m_target_navigable);
}

SessionHistoryTraversalQueue::SessionHistoryTraversalQueue() = default;

void SessionHistoryTraversalQueue::process_queue()
{
    while (m_queue.size() > 0) {
        if (m_current_promise && !m_current_promise->is_resolved() && !m_current_promise->is_rejected()) {
            m_current_promise->when_resolved([this](Empty) {
                process_queue();
            });
            return;
        }

        auto entry = m_queue.take_first();
        m_current_promise = Core::Promise<Empty>::construct();
        entry->execute_steps(*m_current_promise);
    }
}

void SessionHistoryTraversalQueue::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_queue);
}

void SessionHistoryTraversalQueue::append(GC::Ref<SessionHistoryTraversalSteps> steps)
{
    m_queue.append(SessionHistoryTraversalQueueEntry::create(vm(), steps, nullptr));
    schedule_processing();
}

void SessionHistoryTraversalQueue::append_sync(GC::Ref<SessionHistoryTraversalSteps> steps, GC::Ptr<Navigable> target_navigable)
{
    m_queue.append(SessionHistoryTraversalQueueEntry::create(vm(), steps, target_navigable));
    schedule_processing();
}

void SessionHistoryTraversalQueue::schedule_processing()
{
    if (!m_processing_scheduled) {
        m_processing_scheduled = true;
        Core::deferred_invoke([this] {
            m_processing_scheduled = false;
            process_queue();
        });
    }
}

// https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigations-jump-queue
GC::Ptr<SessionHistoryTraversalQueueEntry> SessionHistoryTraversalQueue::first_synchronous_navigation_steps_with_target_navigable_not_contained_in(HashTable<GC::Ref<Navigable>> const& set)
{
    auto index = m_queue.find_first_index_if([&set](auto const& entry) -> bool {
        auto target_navigable = entry->target_navigable();
        if (target_navigable == nullptr)
            return false;

        if (set.contains(*target_navigable))
            return false;

        // A newly created child navigable is not yet discoverable through get_session_history_entries()
        // until its creation bookkeeping has run on the traversal queue. Do not let synchronous
        // navigation steps for that child jump ahead of the bookkeeping step that installs it.
        if (!target_navigable->has_session_history_entry_and_ready_for_navigation())
            return false;

        return true;
    });
    if (index.has_value())
        return m_queue.take(*index);
    return {};
}

}
