/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Painting/BackingStore.h>
#include <WebContent/Forward.h>

namespace WebContent {

class BackingStoreManager {
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

    Web::Painting::BackingStore* back_store() { return m_back_store.ptr(); }
    i32 front_id() const { return m_front_bitmap_id; }

    void swap_back_and_front();

    BackingStoreManager(PageClient&);

private:
    // FIXME: We should come up with an ownership model for this class that makes the GC-checker happy
    IGNORE_GC PageClient& m_page_client;

    i32 m_front_bitmap_id { -1 };
    i32 m_back_bitmap_id { -1 };
    RefPtr<Web::Painting::BackingStore> m_front_store;
    RefPtr<Web::Painting::BackingStore> m_back_store;
    int m_next_bitmap_id { 0 };

    RefPtr<Core::Timer> m_backing_store_shrink_timer;
};

}
