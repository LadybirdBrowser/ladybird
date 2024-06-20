/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Forward.h>
#include <LibWeb/Page/Page.h>
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

    RefPtr<Gfx::Bitmap> back_bitmap() { return m_back_bitmap; }
    i32 front_id() const { return m_front_bitmap_id; }

    void swap_back_and_front();

    BackingStoreManager(PageClient&);

private:
    PageClient& m_page_client;

    i32 m_front_bitmap_id { -1 };
    i32 m_back_bitmap_id { -1 };
    RefPtr<Gfx::Bitmap> m_front_bitmap;
    RefPtr<Gfx::Bitmap> m_back_bitmap;
    int m_next_bitmap_id { 0 };

    RefPtr<Core::Timer> m_backing_store_shrink_timer;
};

}
