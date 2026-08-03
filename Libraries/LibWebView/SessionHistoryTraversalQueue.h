/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibCore/Promise.h>
#include <LibWeb/HTML/CrossProcessId.h>
#include <LibWebView/Export.h>

namespace WebView {

// The steps resolve the promise they are given once they are complete.
using SessionHistoryTraversalSteps = Function<void(NonnullRefPtr<Core::Promise<Empty>>)>;

// https://html.spec.whatwg.org/multipage/document-sequences.html#tn-session-history-traversal-queue
class WEBVIEW_API SessionHistoryTraversalQueue
    : public Weakable<SessionHistoryTraversalQueue> {
    AK_MAKE_NONCOPYABLE(SessionHistoryTraversalQueue);
    AK_MAKE_NONMOVABLE(SessionHistoryTraversalQueue);

public:
    SessionHistoryTraversalQueue() = default;

    struct Item {
        // Present for synchronous navigation steps, empty for plain algorithm steps.
        Optional<Web::HTML::CrossProcessId> target_navigable;
        SessionHistoryTraversalSteps steps;
    };

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#tn-append-session-history-traversal-steps
    void append_session_history_traversal_steps(SessionHistoryTraversalSteps);

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#tn-append-session-history-sync-nav-steps
    void append_session_history_synchronous_navigation_steps(Web::HTML::CrossProcessId target_navigable, SessionHistoryTraversalSteps);

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#sync-navigations-jump-queue
    Optional<Item> take_first_synchronous_navigation_steps_not_targeting(HashTable<Web::HTML::CrossProcessId> const& excluded_navigables);

    bool is_empty() const { return m_algorithm_set.is_empty(); }

private:
    void process_queue();
    void schedule_processing();

    // https://html.spec.whatwg.org/multipage/browsing-the-web.html#session-history-traversal-parallel-queue-algorithm-set
    Vector<Item> m_algorithm_set;

    bool m_processing_scheduled { false };
    RefPtr<Core::Promise<Empty>> m_running_steps;
};

}
