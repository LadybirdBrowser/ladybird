/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Variant.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>
#include <LibGfx/SharedImage.h>
#include <LibSync/ConditionVariable.h>
#include <LibThreading/Forward.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Page/Page.h>

namespace Web::Compositor {

struct AsyncScrollingState;

class WEB_API CompositorThread {
    AK_MAKE_NONCOPYABLE(CompositorThread);
    AK_MAKE_NONMOVABLE(CompositorThread);

public:
    class ThreadData;

    enum class PagePresentationRegistration {
        No,
        Yes,
    };

    using BackingStorePresentationCallback = Function<void(u64 page_id, i32 front_bitmap_id, Gfx::SharedImage, i32 back_bitmap_id, Gfx::SharedImage)>;
    using FramePresentationCallback = Function<void(u64 page_id, Gfx::IntRect const&, i32 bitmap_id)>;
    struct PresentToUI {
    };
    struct PublishToExternalContent {
        NonnullRefPtr<Painting::ExternalContentSource> source;
    };
    using PresentationMode = Variant<PresentToUI, PublishToExternalContent>;

    CompositorThread(u64 page_id, PagePresentationRegistration);
    ~CompositorThread();

    static void set_frame_presentation_callbacks(NonnullRefPtr<Core::WeakEventLoopReference>, BackingStorePresentationCallback, FramePresentationCallback);
    static void clear_frame_presentation_callbacks();
    static void presented_bitmap_ready_to_paint(u64 page_id, i32 bitmap_id);
    static bool async_scroll_by(u64 page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta);

    void start(DisplayListPlayerType);
    void stop_presenting_to_client();
    void set_presentation_mode(PresentationMode);

    void update_display_list(NonnullRefPtr<Painting::DisplayList>, Painting::ScrollStateSnapshot&&);
    void update_scroll_state(Painting::ScrollStateSnapshot&&);
    void update_display_list_and_async_scrolling_state(NonnullRefPtr<Painting::DisplayList>, Painting::ScrollStateSnapshot&&, AsyncScrollingState&&);
    void invalidate_wheel_event_listener_state(u64 generation);
    bool async_scroll_by(Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect);
    Optional<Gfx::FloatPoint> pending_async_viewport_scroll_offset() const;
    bool should_defer_async_viewport_scroll_offset_adoption() const;
    bool should_defer_main_thread_present_for_async_scroll() const;
    Optional<Gfx::FloatPoint> take_pending_async_viewport_scroll_offset();
    void update_backing_stores(Gfx::IntSize, i32 front_id, i32 back_id);
    u64 present_frame(Gfx::IntRect);
    void wait_for_frame(u64 frame_id);
    void request_screenshot(NonnullRefPtr<Gfx::PaintingSurface>, Function<void()>&& callback);

private:
    NonnullRefPtr<ThreadData> m_thread_data;
    RefPtr<Threading::Thread> m_thread;

    static void register_page_compositor(u64 page_id, NonnullRefPtr<ThreadData>);
    static void unregister_page_compositor(u64 page_id, ThreadData&);
    static bool present_backing_stores_to_client(u64 page_id, i32 front_bitmap_id, Gfx::SharedImage&&, i32 back_bitmap_id, Gfx::SharedImage&&);
    static bool present_frame_to_client(u64 page_id, Gfx::IntRect const&, i32 bitmap_id);
};

}
