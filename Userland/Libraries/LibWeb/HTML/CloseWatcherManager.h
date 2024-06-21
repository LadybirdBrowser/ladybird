/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <LibWeb/DOM/EventTarget.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/interaction.html#close-watcher-manager
class CloseWatcherManager final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(CloseWatcherManager, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(CloseWatcherManager);

public:
    [[nodiscard]] static JS::NonnullGCPtr<CloseWatcherManager> create(JS::Realm&);

    void add(JS::NonnullGCPtr<CloseWatcher>);
    void remove(CloseWatcher const&);

    bool process_close_watchers();

    void notify_about_user_activation();
    bool can_prevent_close();

private:
    explicit CloseWatcherManager(JS::Realm&);

    virtual void visit_edges(Cell::Visitor&) override;

    Vector<Vector<JS::NonnullGCPtr<CloseWatcher>>> m_groups;
    uint32_t m_allowed_number_of_groups { 1 };
    bool m_next_user_interaction_allows_a_new_group { true };
};

}
