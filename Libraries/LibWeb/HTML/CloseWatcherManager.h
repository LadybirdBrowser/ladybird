/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibGC/Cell.h>
#include <LibGC/Ptr.h>
#include <LibJS/Forward.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/interaction.html#close-watcher-manager
class CloseWatcherManager final : public GC::Cell {
    GC_CELL(CloseWatcherManager, GC::Cell);
    GC_DECLARE_ALLOCATOR(CloseWatcherManager);

public:
    [[nodiscard]] static GC::Ref<CloseWatcherManager> create(JS::Realm&);

    void add(GC::Ref<CloseWatcher>);
    void remove(CloseWatcher const&);

    bool process_close_watchers();

    void notify_about_user_activation();
    bool can_prevent_close();

private:
    explicit CloseWatcherManager(JS::Realm&);

    virtual void visit_edges(GC::Cell::Visitor&) override;

    Vector<Vector<GC::Ref<CloseWatcher>>> m_groups;
    uint32_t m_allowed_number_of_groups { 1 };
    bool m_next_user_interaction_allows_a_new_group { true };
};

}
