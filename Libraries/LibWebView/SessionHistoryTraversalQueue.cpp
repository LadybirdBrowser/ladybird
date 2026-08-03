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
    m_algorithm_set.append({ {}, move(steps) });
    schedule_processing();
}

void SessionHistoryTraversalQueue::append_session_history_synchronous_navigation_steps(Web::HTML::CrossProcessId target_navigable, SessionHistoryTraversalSteps steps)
{
    m_algorithm_set.append({ target_navigable, move(steps) });
    schedule_processing();
}

Optional<SessionHistoryTraversalQueue::Item> SessionHistoryTraversalQueue::take_first_synchronous_navigation_steps_not_targeting(HashTable<Web::HTML::CrossProcessId> const& excluded_navigables)
{
    auto index = m_algorithm_set.find_first_index_if([&excluded_navigables](auto const& item) {
        return item.target_navigable.has_value() && !excluded_navigables.contains(*item.target_navigable);
    });
    if (!index.has_value())
        return {};
    return m_algorithm_set.take(*index);
}

void SessionHistoryTraversalQueue::process_queue()
{
    while (!m_algorithm_set.is_empty()) {
        if (m_running_steps && !m_running_steps->is_resolved() && !m_running_steps->is_rejected()) {
            m_running_steps->when_resolved([weak_this = make_weak_ptr()](Empty) {
                if (weak_this)
                    weak_this->process_queue();
            });
            return;
        }

        auto item = m_algorithm_set.take_first();
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
