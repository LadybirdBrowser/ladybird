/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/HTML/Navigable.h>
#include <LibWeb/Painting/BackingStore.h>

namespace Web::Painting {

class BackingStoreManager : public JS::Cell {
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

    struct BackingStore {
        i32 bitmap_id { -1 };
        Web::Painting::BackingStore* store { nullptr };
    };

    BackingStore acquire_store_for_next_frame()
    {
        BackingStore backing_store;
        backing_store.bitmap_id = m_back_bitmap_id;
        backing_store.store = m_back_store.ptr();
        swap_back_and_front();
        return backing_store;
    }

    virtual void visit_edges(Cell::Visitor& visitor) override;

    BackingStoreManager(HTML::Navigable&);

private:
    void swap_back_and_front();

    GC::Ref<HTML::Navigable> m_navigable;

    i32 m_front_bitmap_id { -1 };
    i32 m_back_bitmap_id { -1 };
    RefPtr<Painting::BackingStore> m_front_store;
    RefPtr<Painting::BackingStore> m_back_store;
    int m_next_bitmap_id { 0 };

    RefPtr<Core::Timer> m_backing_store_shrink_timer;
};

}
