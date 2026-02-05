/*
 * Copyright (c) 2024-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>

namespace Web::Painting {

class WEB_API BackingStoreManager : public JS::Cell {
    GC_CELL(BackingStoreManager, JS::Cell);
    GC_DECLARE_ALLOCATOR(BackingStoreManager);

public:
#ifdef AK_OS_MACOS
    static void set_browser_mach_port(Core::MachPort&&);
#endif

    enum class WindowResizingInProgress {
        No,
        Yes
    };
    void resize_backing_stores_if_needed(WindowResizingInProgress window_resize_in_progress);
    void reallocate_backing_stores(Gfx::IntSize);
    void restart_resize_timer();

    virtual void visit_edges(Cell::Visitor& visitor) override;

    BackingStoreManager(HTML::Navigable&);

private:
    GC::Ref<HTML::Navigable> m_navigable;

    i32 m_front_bitmap_id { -1 };
    i32 m_back_bitmap_id { -1 };
    int m_next_bitmap_id { 0 };

    // Used to track if backing stores need reallocation
    Gfx::IntSize m_allocated_size;

    RefPtr<Core::Timer> m_backing_store_shrink_timer;
};

}
