/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWebView/SessionHistoryTraversalQueue.h>

namespace WebView {

void SessionHistoryTraversalQueue::append_session_history_traversal_steps(SessionHistoryTraversalSteps steps)
{
    m_algorithm_set.append({ {}, move(steps), ++m_last_enqueued_sequence_number });
    schedule_processing();
}

void SessionHistoryTraversalQueue::append_session_history_synchronous_navigation_steps(Web::HTML::CrossProcessId target_navigable, SessionHistoryTraversalSteps steps)
{
    m_algorithm_set.append({ target_navigable, move(steps), ++m_last_enqueued_sequence_number });
    schedule_processing();
}

Optional<SessionHistoryTraversalQueue::Item> SessionHistoryTraversalQueue::take_first_synchronous_navigation_steps_not_targeting(HashTable<Web::HTML::CrossProcessId> const& excluded_navigables)
{
    return take_first_synchronous_navigation_steps_not_targeting(excluded_navigables, m_last_enqueued_sequence_number);
}

Optional<SessionHistoryTraversalQueue::Item> SessionHistoryTraversalQueue::take_first_synchronous_navigation_steps_not_targeting(HashTable<Web::HTML::CrossProcessId> const& excluded_navigables, u64 maximum_sequence_number)
{
    auto index = m_algorithm_set.find_first_index_if([&](auto const& item) {
        return item.sequence_number <= maximum_sequence_number
            && item.target_navigable.has_value()
            && !excluded_navigables.contains(*item.target_navigable);
    });
    if (!index.has_value())
        return {};
    return m_algorithm_set.take(*index);
}

void SessionHistoryTraversalQueue::process_queue()
{
    for (;;) {
        if (m_running_steps && !m_running_steps->is_resolved() && !m_running_steps->is_rejected()) {
            m_running_steps->when_resolved([weak_this = make_weak_ptr()](Empty) {
                if (weak_this)
                    weak_this->process_queue();
            });
            return;
        }

        m_current_item_is_synchronous_navigation_steps = false;
        if (m_algorithm_set.is_empty())
            return;

        auto item = m_algorithm_set.take_first();
        m_current_item_is_synchronous_navigation_steps = item.target_navigable.has_value();
        m_running_steps = Core::Promise<Empty>::construct();
        item.steps(*m_running_steps);
    }
}

void SessionHistoryTraversalQueue::schedule_processing()
{
    if (m_processing_scheduled)
        return;
    m_processing_scheduled = true;
    Core::deferred_invoke([weak_this = make_weak_ptr()] {
        if (!weak_this)
            return;
        weak_this->m_processing_scheduled = false;
        weak_this->process_queue();
    });
}

}
