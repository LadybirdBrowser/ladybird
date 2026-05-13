/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SharedImageBuffer.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Compositor/CompositorThread.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/ExternalContentSource.h>

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/Queue.h>
#include <AK/Time.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <AK/Array.h>
#    include <LibGfx/VulkanImage.h>
#    include <libdrm/drm_fourcc.h>
#endif

#include <core/SkCanvas.h>
#include <core/SkColor.h>
#include <core/SkPaint.h>
#include <core/SkRRect.h>

#include <LibCore/Platform/ScopedAutoreleasePool.h>

namespace Web::Compositor {

struct BackingStoreState {
    RefPtr<Gfx::PaintingSurface> front_store;
    RefPtr<Gfx::PaintingSurface> back_store;
    i32 front_bitmap_id { -1 };
    i32 back_bitmap_id { -1 };

    void swap()
    {
        AK::swap(front_store, back_store);
        AK::swap(front_bitmap_id, back_bitmap_id);
    }

    bool is_valid() const { return front_store && back_store; }
};

struct UpdateDisplayListCommand {
    NonnullRefPtr<Painting::DisplayList> display_list;
    Painting::ScrollStateSnapshot scroll_state_snapshot;
    Optional<AsyncScrollingState> async_scrolling_state;
};

struct AsyncScrollByCommand {
    Gfx::FloatPoint position;
    Gfx::FloatPoint delta_in_device_pixels;
    Gfx::IntRect viewport_rect;
    AsyncScrollNodeID scroll_target;
};

struct ViewportScrollbarDragCommand {
    size_t scrollbar_index { 0 };
    float primary_position { 0 };
    float thumb_grab_position { 0 };
};

struct UpdateScrollStateCommand {
    Painting::ScrollStateSnapshot scroll_state_snapshot;
};

struct UpdateBackingStoresCommand {
    Gfx::IntSize size;
    i32 front_bitmap_id;
    i32 back_bitmap_id;
};

struct ScreenshotCommand {
    NonnullRefPtr<Gfx::PaintingSurface> target_surface;
    Function<void()> callback;
};

using CompositorCommand = Variant<UpdateDisplayListCommand, AsyncScrollByCommand, ViewportScrollbarDragCommand, UpdateScrollStateCommand, UpdateBackingStoresCommand, ScreenshotCommand>;

static Optional<AsyncScrollNode> viewport_scroll_node(AsyncScrollingState const& state)
{
    for (auto const& node : state.scroll_nodes) {
        if (node.is_viewport)
            return node;
    }
    return {};
}

static SkRect to_skia_rect(Gfx::IntRect const& rect)
{
    return SkRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height());
}

static Gfx::Orientation orientation_for_scrollbar(ViewportScrollbar const& scrollbar)
{
    return scrollbar.vertical ? Gfx::Orientation::Vertical : Gfx::Orientation::Horizontal;
}

struct ViewportScrollbarIdentity {
    AsyncScrollNodeID scroll_node_id;
    bool vertical { false };
};

static ViewportScrollbarIdentity viewport_scrollbar_identity(ViewportScrollbar const& scrollbar)
{
    return { scrollbar.scroll_node_id, scrollbar.vertical };
}

static Optional<ViewportScrollbarIdentity> viewport_scrollbar_identity_at(ReadonlySpan<ViewportScrollbar> scrollbars, Optional<size_t> scrollbar_index)
{
    if (!scrollbar_index.has_value() || *scrollbar_index >= scrollbars.size())
        return {};
    return viewport_scrollbar_identity(scrollbars[*scrollbar_index]);
}

static Optional<size_t> find_viewport_scrollbar_index(ReadonlySpan<ViewportScrollbar> scrollbars, ViewportScrollbarIdentity identity)
{
    for (size_t i = 0; i < scrollbars.size(); ++i) {
        if (scrollbars[i].scroll_node_id == identity.scroll_node_id && scrollbars[i].vertical == identity.vertical)
            return i;
    }
    return {};
}

static Gfx::IntRect scrollbar_gutter_rect(ViewportScrollbar const& scrollbar, bool expanded)
{
    return expanded ? scrollbar.expanded_gutter_rect : scrollbar.gutter_rect;
}

static double scrollbar_scroll_size(ViewportScrollbar const& scrollbar, bool expanded)
{
    return expanded ? scrollbar.expanded_scroll_size : scrollbar.scroll_size;
}

static Gfx::IntRect translated_thumb_rect(ViewportScrollbar const& scrollbar, Painting::ScrollStateSnapshot const& scroll_state_snapshot, bool expanded)
{
    auto thumb_rect = expanded ? scrollbar.expanded_thumb_rect : scrollbar.thumb_rect;
    auto scroll_size = scrollbar_scroll_size(scrollbar, expanded);
    auto device_offset = scroll_state_snapshot.device_offset_for_index(scrollbar.scroll_frame_index);
    if (scrollbar.vertical)
        thumb_rect.translate_by(0, static_cast<int>(-device_offset.y() * scroll_size));
    else
        thumb_rect.translate_by(static_cast<int>(-device_offset.x() * scroll_size), 0);
    return thumb_rect;
}

static Gfx::IntRect translated_thumb_rect(ViewportScrollbar const& scrollbar, Gfx::FloatPoint scroll_offset, bool expanded)
{
    auto orientation = orientation_for_scrollbar(scrollbar);
    auto thumb_rect = expanded ? scrollbar.expanded_thumb_rect : scrollbar.thumb_rect;
    thumb_rect.translate_primary_offset_for_orientation(orientation, static_cast<int>(scroll_offset.primary_offset_for_orientation(orientation) * scrollbar_scroll_size(scrollbar, expanded)));
    return thumb_rect;
}

static Gfx::IntRect scrollbar_hit_rect(ViewportScrollbar const& scrollbar, Gfx::FloatPoint scroll_offset)
{
    static constexpr int scrollbar_hit_slop = 4;

    auto rect = translated_thumb_rect(scrollbar, scroll_offset, false).united(translated_thumb_rect(scrollbar, scroll_offset, true));
    auto expanded_gutter_rect = scrollbar_gutter_rect(scrollbar, true);
    if (!expanded_gutter_rect.is_empty())
        rect.unite(expanded_gutter_rect);
    rect.inflate(scrollbar_hit_slop, scrollbar_hit_slop);
    return rect;
}

static void paint_viewport_scrollbar(Gfx::PaintingSurface& surface, ViewportScrollbar const& scrollbar, Painting::ScrollStateSnapshot const& scroll_state_snapshot, bool expanded)
{
    auto thumb_rect = translated_thumb_rect(scrollbar, scroll_state_snapshot, expanded);
    auto& canvas = surface.canvas();

    SkPaint gutter_fill_paint;
    gutter_fill_paint.setColor(to_skia_color(scrollbar.track_color));
    canvas.drawRect(to_skia_rect(scrollbar_gutter_rect(scrollbar, expanded)), gutter_fill_paint);

    auto skia_thumb_rect = to_skia_rect(thumb_rect);
    auto radius = skia_thumb_rect.width() / 2;
    auto thumb_rrect = SkRRect::MakeRectXY(skia_thumb_rect, radius, radius);

    SkPaint thumb_fill_paint;
    thumb_fill_paint.setColor(to_skia_color(scrollbar.thumb_color));
    canvas.drawRRect(thumb_rrect, thumb_fill_paint);

    SkPaint stroke_paint;
    stroke_paint.setStroke(true);
    stroke_paint.setStrokeWidth(1);
    stroke_paint.setAntiAlias(true);
    stroke_paint.setColor(to_skia_color(scrollbar.thumb_color.lightened()));
    canvas.drawRRect(thumb_rrect, stroke_paint);
}

static void paint_viewport_scrollbars(Gfx::PaintingSurface& surface, ReadonlySpan<ViewportScrollbar> scrollbars, Painting::ScrollStateSnapshot const& scroll_state_snapshot, Optional<size_t> hovered_scrollbar_index, Optional<size_t> captured_scrollbar_index)
{
    for (size_t i = 0; i < scrollbars.size(); ++i)
        paint_viewport_scrollbar(surface, scrollbars[i], scroll_state_snapshot, hovered_scrollbar_index == i || captured_scrollbar_index == i);
}

static void flush_surface(Gfx::PaintingSurface& surface)
{
    if (auto context = surface.skia_backend_context())
        context->flush_and_submit(&surface.sk_surface());
    surface.flush();
}

struct BackingStorePair {
    RefPtr<Gfx::PaintingSurface> front;
    RefPtr<Gfx::PaintingSurface> back;
};

#ifdef USE_VULKAN
static NonnullRefPtr<Gfx::PaintingSurface> create_gpu_painting_surface_with_bitmap_flush(Gfx::IntSize size, Gfx::SharedImageBuffer& buffer, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto surface = Gfx::PaintingSurface::create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context);
    auto bitmap = buffer.bitmap();
    surface->on_flush = [bitmap = move(bitmap)](auto& surface) {
        surface.read_into_bitmap(*bitmap);
    };
    return surface;
}
#endif

static BackingStorePair create_shareable_bitmap_backing_stores([[maybe_unused]] Gfx::IntSize size, Gfx::SharedImageBuffer& front_buffer, Gfx::SharedImageBuffer& back_buffer, RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
#ifdef AK_OS_MACOS
    if (skia_backend_context) {
        return {
            .front = Gfx::PaintingSurface::create_from_shared_image_buffer(front_buffer, *skia_backend_context),
            .back = Gfx::PaintingSurface::create_from_shared_image_buffer(back_buffer, *skia_backend_context),
        };
    }
#else
#    ifdef USE_VULKAN
    if (skia_backend_context) {
        return {
            .front = create_gpu_painting_surface_with_bitmap_flush(size, front_buffer, skia_backend_context),
            .back = create_gpu_painting_surface_with_bitmap_flush(size, back_buffer, skia_backend_context),
        };
    }
#    else
    (void)skia_backend_context;
#    endif
#endif

    return {
        .front = Gfx::PaintingSurface::wrap_bitmap(*front_buffer.bitmap()),
        .back = Gfx::PaintingSurface::wrap_bitmap(*back_buffer.bitmap()),
    };
}

#ifdef USE_VULKAN_DMABUF_IMAGES
struct DMABufBackingStorePair {
    RefPtr<Gfx::PaintingSurface> front;
    RefPtr<Gfx::PaintingSurface> back;
    Gfx::SharedImage front_shared_image;
    Gfx::SharedImage back_shared_image;
};

static ErrorOr<DMABufBackingStorePair> create_linear_dmabuf_backing_stores(Gfx::IntSize size, Gfx::SkiaBackendContext& skia_backend_context)
{
    auto const& vulkan_context = skia_backend_context.vulkan_context();
    static constexpr Array<uint64_t, 1> linear_modifiers = { DRM_FORMAT_MOD_LINEAR };
    auto front_image = TRY(Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    auto back_image = TRY(Gfx::create_shared_vulkan_image(vulkan_context, size.width(), size.height(), VK_FORMAT_B8G8R8A8_UNORM, linear_modifiers.span()));
    auto front_shared_image = Gfx::duplicate_shared_image(*front_image);
    auto back_shared_image = Gfx::duplicate_shared_image(*back_image);

    return DMABufBackingStorePair {
        .front = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(front_image), Gfx::PaintingSurface::Origin::TopLeft),
        .back = Gfx::PaintingSurface::create_from_vkimage(skia_backend_context, move(back_image), Gfx::PaintingSurface::Origin::TopLeft),
        .front_shared_image = move(front_shared_image),
        .back_shared_image = move(back_shared_image),
    };
}
#endif

class CompositorThread::ThreadData final : public AtomicRefCounted<ThreadData> {
public:
    ThreadData(u64 page_id, NonnullRefPtr<Core::WeakEventLoopReference>&& main_thread_event_loop, CompositorThread::PagePresentationRegistration page_presentation_registration)
        : m_page_id(page_id)
        , m_main_thread_event_loop(move(main_thread_event_loop))
        , m_presents_to_client(page_presentation_registration == CompositorThread::PagePresentationRegistration::Yes)
    {
    }

    ~ThreadData() = default;

    u64 page_id() const { return m_page_id; }
    bool presents_to_client() const { return m_presents_to_client; }
    void stop_presenting_to_client()
    {
        Sync::MutexLocker const locker { m_mutex };
        m_presents_to_client = false;
    }

    void set_presentation_mode(CompositorThread::PresentationMode mode)
    {
        Sync::MutexLocker const locker { m_mutex };
        m_presentation_mode = move(mode);
    }

    void exit()
    {
        Sync::MutexLocker const locker { m_mutex };
        m_exit = true;
        m_command_ready.signal();
        m_ready_to_paint.signal();
        m_frame_completed.broadcast();
    }

    void enqueue_command(CompositorCommand&& command)
    {
        Sync::MutexLocker const locker { m_mutex };
        m_command_queue.enqueue(move(command));
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor command queued (queue_size={}, raster_tasks={}, needs_present={}, deferred_async_present={})",
            m_command_queue.size(), m_queued_rasterization_tasks.load(), m_needs_present, m_has_deferred_async_scroll_present);
        m_command_ready.signal();
    }

    u64 set_needs_present(Gfx::IntRect viewport_rect)
    {
        Sync::MutexLocker const locker { m_mutex };
        m_needs_present = true;
        m_pending_viewport_rect = viewport_rect;
        m_submitted_frame_id++;
        m_command_ready.signal();
        return m_submitted_frame_id;
    }

    void mark_frame_complete(u64 frame_id)
    {
        Sync::MutexLocker const locker { m_mutex };
        m_completed_frame_id = frame_id;
        m_frame_completed.broadcast();
    }

    void wait_for_frame(u64 frame_id)
    {
        Sync::MutexLocker const locker { m_mutex };
        while (m_completed_frame_id < frame_id && !m_exit)
            m_frame_completed.wait();
    }

    Optional<Gfx::FloatPoint> take_pending_async_viewport_scroll_offset()
    {
        Sync::MutexLocker const locker { m_mutex };
        auto scroll_offset = m_pending_async_viewport_scroll_offset;
        m_pending_async_viewport_scroll_offset.clear();
        return scroll_offset;
    }

    Optional<Gfx::FloatPoint> pending_async_viewport_scroll_offset() const
    {
        Sync::MutexLocker const locker { m_mutex };
        return m_pending_async_viewport_scroll_offset;
    }

    bool should_defer_async_viewport_scroll_offset_adoption() const
    {
        Sync::MutexLocker const locker { m_mutex };
        return m_pending_async_viewport_scroll_offset.has_value()
            && m_is_rasterizing;
    }

    bool should_defer_main_thread_present_for_async_scroll() const
    {
        Sync::MutexLocker const locker { m_mutex };
        return m_pending_async_viewport_scroll_offset.has_value()
            && (m_is_rasterizing || m_has_deferred_async_scroll_present || m_queued_rasterization_tasks > 0);
    }

    void invalidate_wheel_event_listener_state(u64 generation)
    {
        Sync::MutexLocker const locker { m_mutex };
        m_wheel_event_listener_state_generation = max(m_wheel_event_listener_state_generation, generation);
        m_wheel_routing_admission = WheelRoutingAdmission::StaleWheelEventListeners;
        m_can_accept_async_wheel_events = false;
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Invalidated compositor wheel listener state (generation={})", generation);
    }

    struct ViewportWheelTarget {
        Optional<AsyncScrollNodeID> node_id;
        bool rejected_non_viewport_target { false };
    };

    ViewportWheelTarget hit_test_viewport_scroll_node_for_wheel(Gfx::FloatPoint position, Gfx::FloatPoint delta) const
    {
        Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
        auto scroll_target = m_async_scroll_tree.hit_test_scroll_node_for_wheel(position, delta);
        if (!scroll_target.has_value())
            return {};
        if (!m_async_scroll_tree.scroll_node_is_viewport(*scroll_target))
            return { {}, true };
        return { scroll_target, false };
    }

    bool enqueue_async_scroll_by(Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect)
    {
        if (!m_can_accept_async_wheel_events.load()) {
            auto wheel_routing_admission = [this] {
                Sync::MutexLocker const locker { m_mutex };
                return m_wheel_routing_admission;
            }();
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: compositor cannot accept async wheel events ({})",
                wheel_routing_admission_to_string(wheel_routing_admission));
            return false;
        }
        auto scroll_target = hit_test_viewport_scroll_node_for_wheel(position, delta_in_device_pixels);
        if (scroll_target.rejected_non_viewport_target) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: non-viewport target at {},{} device delta {},{}",
                position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            return false;
        }
        if (!scroll_target.node_id.has_value()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: no wheel target at {},{} for device delta {},{}",
                position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            return false;
        }

        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor accepted main-thread async scroll enqueue at {},{} device delta {},{} viewport={}x{} at {},{}",
            position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y(), viewport_rect.width(), viewport_rect.height(), viewport_rect.x(), viewport_rect.y());
        enqueue_command(AsyncScrollByCommand { position, delta_in_device_pixels, viewport_rect, *scroll_target.node_id });
        return true;
    }

    bool enqueue_async_scroll_by(Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels)
    {
        if (!m_can_accept_async_wheel_events.load()) {
            auto wheel_routing_admission = [this] {
                Sync::MutexLocker const locker { m_mutex };
                return m_wheel_routing_admission;
            }();
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: compositor cannot accept async wheel events ({})",
                wheel_routing_admission_to_string(wheel_routing_admission));
            return false;
        }

        auto viewport_rect = [this] {
            Sync::MutexLocker const locker { m_mutex };
            return m_async_scrolling_viewport_rect;
        }();
        auto scroll_target = hit_test_viewport_scroll_node_for_wheel(position, delta_in_device_pixels);
        if (scroll_target.rejected_non_viewport_target) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: non-viewport target at {},{} device delta {},{}",
                position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            return false;
        }
        if (!scroll_target.node_id.has_value()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: no wheel target at {},{} for device delta {},{}",
                position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            return false;
        }

        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor accepted async scroll enqueue at {},{} device delta {},{} viewport={}x{} at {},{}",
            position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y(), viewport_rect.width(), viewport_rect.height(), viewport_rect.x(), viewport_rect.y());
        enqueue_command(AsyncScrollByCommand { position, delta_in_device_pixels, viewport_rect, *scroll_target.node_id });
        return true;
    }

    Optional<size_t> hit_test_viewport_scrollbar(Gfx::FloatPoint position) const
    {
        Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
        for (size_t i = 0; i < m_viewport_scrollbars.size(); ++i) {
            auto const& scrollbar = m_viewport_scrollbars[i];
            auto scroll_offset = m_async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id);
            if (!scroll_offset.has_value())
                continue;

            if (scrollbar_hit_rect(scrollbar, *scroll_offset).to_type<float>().contains(position))
                return i;
        }
        return {};
    }

    bool handle_viewport_scrollbar_mouse_event(MouseEvent const& event)
    {
        auto position = Gfx::FloatPoint {
            static_cast<float>(event.position.x().value()),
            static_cast<float>(event.position.y().value()),
        };
        auto primary_position_for_scrollbar = [&](ViewportScrollbar const& scrollbar) {
            return position.primary_offset_for_orientation(orientation_for_scrollbar(scrollbar));
        };

        switch (event.type) {
        case MouseEvent::Type::MouseDown: {
            if (event.button != UIEvents::MouseButton::Primary)
                return false;

            Optional<size_t> scrollbar_index;
            float thumb_grab_position = 0;
            float primary_position = 0;
            {
                Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
                for (size_t i = 0; i < m_viewport_scrollbars.size(); ++i) {
                    auto const& scrollbar = m_viewport_scrollbars[i];
                    auto scroll_offset = m_async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id);
                    if (!scroll_offset.has_value())
                        continue;

                    auto expanded = m_hovered_viewport_scrollbar_index == i || m_captured_viewport_scrollbar_index == i;
                    auto orientation = orientation_for_scrollbar(scrollbar);
                    auto thumb_rect = translated_thumb_rect(scrollbar, *scroll_offset, expanded);
                    primary_position = primary_position_for_scrollbar(scrollbar);
                    if (thumb_rect.to_type<float>().contains(position)) {
                        thumb_grab_position = primary_position - static_cast<float>(thumb_rect.primary_offset_for_orientation(orientation));
                        scrollbar_index = i;
                        break;
                    }
                    if (scrollbar_hit_rect(scrollbar, *scroll_offset).to_type<float>().contains(position)) {
                        auto gutter_rect = scrollbar_gutter_rect(scrollbar, true);
                        auto thumb_size = static_cast<float>(thumb_rect.primary_size_for_orientation(orientation));
                        auto gutter_start = static_cast<float>(gutter_rect.primary_offset_for_orientation(orientation));
                        auto gutter_size = static_cast<float>(gutter_rect.primary_size_for_orientation(orientation));
                        auto offset_relative_to_gutter = primary_position - gutter_start;
                        thumb_grab_position = max(min(offset_relative_to_gutter, thumb_size / 2), offset_relative_to_gutter - gutter_size + thumb_size);
                        scrollbar_index = i;
                        break;
                    }
                }

                if (!scrollbar_index.has_value())
                    return false;

                m_captured_viewport_scrollbar_index = *scrollbar_index;
                m_hovered_viewport_scrollbar_index = *scrollbar_index;
                m_viewport_scrollbar_thumb_grab_position = thumb_grab_position;
            }

            schedule_viewport_scrollbar_present();
            enqueue_command(ViewportScrollbarDragCommand { *scrollbar_index, primary_position, thumb_grab_position });
            return true;
        }
        case MouseEvent::Type::MouseMove: {
            Optional<size_t> scrollbar_index;
            float thumb_grab_position = 0;
            float primary_position = 0;
            {
                Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
                if (m_captured_viewport_scrollbar_index.has_value()) {
                    scrollbar_index = *m_captured_viewport_scrollbar_index;
                    thumb_grab_position = m_viewport_scrollbar_thumb_grab_position;
                    if (*scrollbar_index >= m_viewport_scrollbars.size()) {
                        m_captured_viewport_scrollbar_index.clear();
                        return false;
                    }
                    primary_position = primary_position_for_scrollbar(m_viewport_scrollbars[*scrollbar_index]);
                }
            }
            if (scrollbar_index.has_value()) {
                enqueue_command(ViewportScrollbarDragCommand { *scrollbar_index, primary_position, thumb_grab_position });
                return true;
            }
            auto hovered_scrollbar_index = hit_test_viewport_scrollbar(position);
            set_hovered_viewport_scrollbar(hovered_scrollbar_index);
            return hovered_scrollbar_index.has_value();
        }
        case MouseEvent::Type::MouseUp: {
            Optional<size_t> scrollbar_index;
            float thumb_grab_position = 0;
            float primary_position = 0;
            {
                Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
                if (!m_captured_viewport_scrollbar_index.has_value())
                    return false;
                scrollbar_index = *m_captured_viewport_scrollbar_index;
                thumb_grab_position = m_viewport_scrollbar_thumb_grab_position;
                if (*scrollbar_index >= m_viewport_scrollbars.size()) {
                    m_captured_viewport_scrollbar_index.clear();
                    return false;
                }
                primary_position = primary_position_for_scrollbar(m_viewport_scrollbars[*scrollbar_index]);
                m_captured_viewport_scrollbar_index.clear();
            }
            schedule_viewport_scrollbar_present();
            enqueue_command(ViewportScrollbarDragCommand { *scrollbar_index, primary_position, thumb_grab_position });
            return true;
        }
        case MouseEvent::Type::MouseLeave: {
            auto has_capture = [this] {
                Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
                return m_captured_viewport_scrollbar_index.has_value();
            }();
            set_hovered_viewport_scrollbar({});
            return has_capture;
        }
        case MouseEvent::Type::MouseWheel:
            return false;
        }
        VERIFY_NOT_REACHED();
    }

    void compositor_loop(DisplayListPlayerType display_list_player_type)
    {
        initialize_skia_player(display_list_player_type);

        while (true) {
            {
                Sync::MutexLocker const locker { m_mutex };
                while (m_command_queue.is_empty() && !has_presentable_work() && !m_exit) {
                    m_command_ready.wait();
                }
                if (m_exit)
                    break;
            }

            // Drain autoreleased Objective-C objects created by Metal/Skia each iteration,
            // since this background thread has no autorelease pool.
            Core::ScopedAutoreleasePool autorelease_pool;

            while (true) {
                auto command = [this]() -> Optional<CompositorCommand> {
                    Sync::MutexLocker const locker { m_mutex };
                    if (m_command_queue.is_empty())
                        return {};
                    auto command = m_command_queue.dequeue();
                    return command;
                }();

                if (!command.has_value())
                    break;

                bool should_yield_to_async_scroll_present = false;
                command->visit(
                    [this](UpdateDisplayListCommand& cmd) {
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor processing display list update (has_async_state={}, raster_tasks={}, deferred_async_present={})",
                            cmd.async_scrolling_state.has_value(), m_queued_rasterization_tasks.load(), m_has_deferred_async_scroll_present);
                        m_cached_display_list = move(cmd.display_list);
                        m_cached_scroll_state_snapshot = move(cmd.scroll_state_snapshot);
                        if (cmd.async_scrolling_state.has_value()) {
                            auto async_scrolling_state = cmd.async_scrolling_state.release_value();
                            auto async_scrolling_viewport_rect = async_scrolling_state.viewport_rect;
                            auto const wheel_event_listener_state_generation = async_scrolling_state.wheel_event_listener_state_generation;
                            auto wheel_routing_admission = wheel_routing_admission_for(async_scrolling_state);
                            {
                                Sync::MutexLocker const locker { m_mutex };
                                if (wheel_event_listener_state_generation < m_wheel_event_listener_state_generation)
                                    wheel_routing_admission = WheelRoutingAdmission::StaleWheelEventListeners;
                                else
                                    m_wheel_event_listener_state_generation = wheel_event_listener_state_generation;
                                m_wheel_routing_admission = wheel_routing_admission;
                            }
                            m_can_accept_async_wheel_events = wheel_routing_admission == WheelRoutingAdmission::Accepted;
                            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor wheel routing admission: {} (scroll_nodes={}, sticky_areas={}, blocking_regions={})",
                                wheel_routing_admission_to_string(wheel_routing_admission),
                                async_scrolling_state.scroll_nodes.size(),
                                async_scrolling_state.sticky_areas.size(),
                                async_scrolling_state.blocking_wheel_event_regions.size());
                            auto viewport_node = viewport_scroll_node(async_scrolling_state);
                            auto pending_async_viewport_scroll_offset = [this] {
                                Sync::MutexLocker const locker { m_mutex };
                                return m_pending_async_viewport_scroll_offset;
                            }();

                            {
                                Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
                                auto hovered_scrollbar_identity = viewport_scrollbar_identity_at(m_viewport_scrollbars, m_hovered_viewport_scrollbar_index);
                                auto captured_scrollbar_identity = viewport_scrollbar_identity_at(m_viewport_scrollbars, m_captured_viewport_scrollbar_index);
                                m_viewport_scrollbars = move(async_scrolling_state.viewport_scrollbars);
                                m_hovered_viewport_scrollbar_index = hovered_scrollbar_identity.has_value() ? find_viewport_scrollbar_index(m_viewport_scrollbars, *hovered_scrollbar_identity) : Optional<size_t> {};
                                m_captured_viewport_scrollbar_index = captured_scrollbar_identity.has_value() ? find_viewport_scrollbar_index(m_viewport_scrollbars, *captured_scrollbar_identity) : Optional<size_t> {};
                                m_async_scroll_tree.set_state(move(async_scrolling_state));
                                if (viewport_node.has_value() && pending_async_viewport_scroll_offset.has_value()) {
                                    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Reapplying pending async viewport offset {},{} to display list update",
                                        pending_async_viewport_scroll_offset->x(), pending_async_viewport_scroll_offset->y());
                                    if (auto reconciled_scroll_offset = m_async_scroll_tree.set_scroll_offset(viewport_node->node_id, *pending_async_viewport_scroll_offset, m_cached_scroll_state_snapshot); reconciled_scroll_offset.has_value())
                                        async_scrolling_viewport_rect.set_location(reconciled_scroll_offset->to_type<int>());
                                }
                                m_async_scroll_tree.rebuild_wheel_scroll_targets(m_cached_display_list, m_cached_scroll_state_snapshot);
                            }
                            {
                                Sync::MutexLocker const locker { m_mutex };
                                m_async_scrolling_viewport_rect = async_scrolling_viewport_rect;
                            }
                            m_has_async_scrolling_state = true;
                        } else {
                            {
                                Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
                                m_viewport_scrollbars.clear();
                                m_hovered_viewport_scrollbar_index.clear();
                                m_captured_viewport_scrollbar_index.clear();
                                m_async_scroll_tree.clear_wheel_scroll_targets();
                            }
                            m_has_async_scrolling_state = false;
                            m_can_accept_async_wheel_events = false;
                            {
                                Sync::MutexLocker const locker { m_mutex };
                                m_wheel_routing_admission = WheelRoutingAdmission::NoAsyncScrollingState;
                            }
                        }
                    },
                    [this, &should_yield_to_async_scroll_present](AsyncScrollByCommand& cmd) {
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor processing async scroll command at {},{} device delta {},{} (raster_tasks={}, deferred_async_present={})",
                            cmd.position.x(), cmd.position.y(), cmd.delta_in_device_pixels.x(), cmd.delta_in_device_pixels.y(), m_queued_rasterization_tasks.load(), m_has_deferred_async_scroll_present);
                        auto async_scroll_viewport_rect = cmd.viewport_rect;
                        Optional<Gfx::FloatPoint> scroll_offset;
                        {
                            Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
                            if (!m_async_scroll_tree.apply_scroll_delta(cmd.scroll_target, cmd.delta_in_device_pixels, m_cached_scroll_state_snapshot)) {
                                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Dropping async scroll command: scroll tree consumed no delta");
                                return;
                            }
                            scroll_offset = m_async_scroll_tree.scroll_offset_for_node(cmd.scroll_target);
                            m_async_scroll_tree.rebuild_wheel_scroll_targets(m_cached_display_list, m_cached_scroll_state_snapshot);
                        }
                        if (scroll_offset.has_value()) {
                            async_scroll_viewport_rect.set_location(scroll_offset->to_type<int>());
                            Sync::MutexLocker const locker { m_mutex };
                            m_pending_async_viewport_scroll_offset = *scroll_offset;
                            m_async_scrolling_viewport_rect = async_scroll_viewport_rect;
                            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Stored pending async viewport offset {},{}",
                                scroll_offset->x(), scroll_offset->y());
                        }
                        invoke_on_main_thread([] {
                            HTML::main_thread_event_loop().queue_task_to_update_the_rendering();
                        });
                        {
                            Sync::MutexLocker const locker { m_mutex };
                            m_has_deferred_async_scroll_present = true;
                            m_deferred_async_scroll_present_viewport_rect = async_scroll_viewport_rect;
                        }
                        should_yield_to_async_scroll_present = can_present_deferred_async_scroll();
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor async scroll command complete (yield_to_present={}, raster_tasks={})",
                            should_yield_to_async_scroll_present, m_queued_rasterization_tasks.load());
                    },
                    [this, &should_yield_to_async_scroll_present](ViewportScrollbarDragCommand& cmd) {
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor processing viewport scrollbar drag (index={}, position={}, grab={})",
                            cmd.scrollbar_index, cmd.primary_position, cmd.thumb_grab_position);
                        if (apply_viewport_scrollbar_drag(cmd.scrollbar_index, cmd.primary_position, cmd.thumb_grab_position)) {
                            should_yield_to_async_scroll_present = can_present_deferred_async_scroll();
                            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor viewport scrollbar drag complete (yield_to_present={}, raster_tasks={})",
                                should_yield_to_async_scroll_present, m_queued_rasterization_tasks.load());
                        }
                    },
                    [this](UpdateScrollStateCommand& cmd) {
                        m_cached_scroll_state_snapshot = move(cmd.scroll_state_snapshot);
                        if (m_has_async_scrolling_state) {
                            auto pending_async_viewport_scroll_offset = [this] {
                                Sync::MutexLocker const locker { m_mutex };
                                return m_pending_async_viewport_scroll_offset;
                            }();
                            Optional<Gfx::FloatPoint> reconciled_viewport_scroll_offset;
                            {
                                Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
                                if (pending_async_viewport_scroll_offset.has_value()) {
                                    if (auto viewport_node_id = m_async_scroll_tree.viewport_scroll_node_id(); viewport_node_id.has_value()) {
                                        // A main-thread scroll-state update can be older than compositor-side async
                                        // scrolling. Preserve the compositor-visible viewport offset in the fresh
                                        // snapshot before rebuilding hit-test data from it.
                                        reconciled_viewport_scroll_offset = m_async_scroll_tree.set_scroll_offset(*viewport_node_id, *pending_async_viewport_scroll_offset, m_cached_scroll_state_snapshot);
                                    }
                                }
                                m_async_scroll_tree.rebuild_wheel_scroll_targets(m_cached_display_list, m_cached_scroll_state_snapshot);
                            }
                            if (reconciled_viewport_scroll_offset.has_value()) {
                                Sync::MutexLocker const mutex_locker { m_mutex };
                                auto reconciled_viewport_rect = m_async_scrolling_viewport_rect;
                                reconciled_viewport_rect.set_location(reconciled_viewport_scroll_offset->to_type<int>());
                                m_async_scrolling_viewport_rect = reconciled_viewport_rect;
                            }
                        }
                    },
                    [this](UpdateBackingStoresCommand& cmd) {
                        if (m_has_async_scrolling_state) {
                            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor received backing stores front={} back={} size={}x{}",
                                cmd.front_bitmap_id, cmd.back_bitmap_id, cmd.size.width(), cmd.size.height());
                        }
                        allocate_backing_stores(cmd);
                        m_backing_stores.front_bitmap_id = cmd.front_bitmap_id;
                        m_backing_stores.back_bitmap_id = cmd.back_bitmap_id;
                    },
                    [this](ScreenshotCommand& cmd) {
                        if (!m_cached_display_list)
                            return;
                        m_skia_player->execute(*m_cached_display_list, m_cached_scroll_state_snapshot, *cmd.target_surface);
                        paint_viewport_scrollbar_overlay(*cmd.target_surface);
                        flush_surface(*cmd.target_surface);
                        if (cmd.callback) {
                            invoke_on_main_thread([callback = move(cmd.callback)]() mutable {
                                callback();
                            });
                        }
                    });

                if (m_exit)
                    break;
                if (should_yield_to_async_scroll_present) {
                    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor yielding command drain to present async scroll");
                    break;
                }
            }

            if (m_exit)
                break;

            bool should_present = false;
            Gfx::IntRect viewport_rect;
            u64 presenting_frame_id = 0;
            bool should_present_deferred_async_scroll = false;
            Gfx::IntRect deferred_async_scroll_viewport_rect;
            Optional<u64> deferred_async_scroll_presenting_frame_id;
            {
                Sync::MutexLocker const locker { m_mutex };
                if (m_has_deferred_async_scroll_present && m_queued_rasterization_tasks == 0) {
                    should_present_deferred_async_scroll = true;
                    deferred_async_scroll_viewport_rect = m_deferred_async_scroll_present_viewport_rect;
                    m_has_deferred_async_scroll_present = false;
                    if (m_needs_present) {
                        deferred_async_scroll_presenting_frame_id = m_submitted_frame_id;
                        m_needs_present = false;
                    }
                    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor selected deferred async present (raster_tasks={}, pending_main_thread_present={})",
                        m_queued_rasterization_tasks.load(), deferred_async_scroll_presenting_frame_id.has_value());
                } else if (m_needs_present && m_queued_rasterization_tasks == 0) {
                    should_present = true;
                    viewport_rect = m_pending_viewport_rect;
                    presenting_frame_id = m_submitted_frame_id;
                    m_needs_present = false;
                }
            }

            if (should_present_deferred_async_scroll)
                present_frame(deferred_async_scroll_viewport_rect, deferred_async_scroll_presenting_frame_id, PresentFrameDelivery::AsyncScroll);
            else if (should_present)
                present_frame(viewport_rect, presenting_frame_id, PresentFrameDelivery::MainThread);
        }
    }

private:
    enum class PresentFrameDelivery {
        MainThread,
        AsyncScroll,
    };

    void paint_viewport_scrollbar_overlay(Gfx::PaintingSurface& surface)
    {
        Vector<ViewportScrollbar> scrollbars;
        Optional<size_t> hovered_scrollbar_index;
        Optional<size_t> captured_scrollbar_index;
        {
            Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
            scrollbars = m_viewport_scrollbars;
            hovered_scrollbar_index = m_hovered_viewport_scrollbar_index;
            captured_scrollbar_index = m_captured_viewport_scrollbar_index;
        }
        paint_viewport_scrollbars(surface, scrollbars, m_cached_scroll_state_snapshot, hovered_scrollbar_index, captured_scrollbar_index);
    }

    void schedule_viewport_scrollbar_present()
    {
        Sync::MutexLocker const locker { m_mutex };
        if (m_async_scrolling_viewport_rect.is_empty())
            return;
        m_has_deferred_async_scroll_present = true;
        m_deferred_async_scroll_present_viewport_rect = m_async_scrolling_viewport_rect;
        m_command_ready.signal();
    }

    void set_hovered_viewport_scrollbar(Optional<size_t> scrollbar_index)
    {
        bool changed = false;
        {
            Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
            if (m_hovered_viewport_scrollbar_index == scrollbar_index)
                return;
            changed = true;
            m_hovered_viewport_scrollbar_index = scrollbar_index;
        }
        if (changed)
            schedule_viewport_scrollbar_present();
    }

    bool apply_viewport_scrollbar_drag(size_t scrollbar_index, float primary_position, float thumb_grab_position)
    {
        Optional<Gfx::FloatPoint> scroll_offset;
        {
            Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
            if (scrollbar_index >= m_viewport_scrollbars.size())
                return false;

            auto const& scrollbar = m_viewport_scrollbars[scrollbar_index];
            auto expanded = m_hovered_viewport_scrollbar_index == scrollbar_index || m_captured_viewport_scrollbar_index == scrollbar_index;
            auto scroll_size = scrollbar_scroll_size(scrollbar, expanded);
            if (scroll_size == 0)
                return false;

            auto current_scroll_offset = m_async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id);
            if (!current_scroll_offset.has_value())
                return false;

            auto orientation = orientation_for_scrollbar(scrollbar);
            auto thumb_rect = expanded ? scrollbar.expanded_thumb_rect : scrollbar.thumb_rect;
            auto min_thumb_position = static_cast<float>(thumb_rect.primary_offset_for_orientation(orientation));
            auto max_thumb_position = min_thumb_position + scrollbar.max_scroll_offset * static_cast<float>(scroll_size);
            auto target_thumb_position = AK::clamp(primary_position - thumb_grab_position, min_thumb_position, max_thumb_position);
            auto target_scroll_offset = (target_thumb_position - min_thumb_position) / static_cast<float>(scroll_size);

            Gfx::FloatPoint delta;
            delta.set_primary_offset_for_orientation(orientation, target_scroll_offset - current_scroll_offset->primary_offset_for_orientation(orientation));
            if (delta.x() == 0 && delta.y() == 0)
                return false;

            if (!m_async_scroll_tree.apply_scroll_delta(scrollbar.scroll_node_id, delta, m_cached_scroll_state_snapshot))
                return false;
            scroll_offset = m_async_scroll_tree.scroll_offset_for_node(scrollbar.scroll_node_id);
            m_async_scroll_tree.rebuild_wheel_scroll_targets(m_cached_display_list, m_cached_scroll_state_snapshot);
        }

        if (!scroll_offset.has_value())
            return false;

        Gfx::IntRect async_scroll_viewport_rect;
        {
            Sync::MutexLocker const locker { m_mutex };
            async_scroll_viewport_rect = m_async_scrolling_viewport_rect;
            async_scroll_viewport_rect.set_location(scroll_offset->to_type<int>());
            m_pending_async_viewport_scroll_offset = *scroll_offset;
            m_async_scrolling_viewport_rect = async_scroll_viewport_rect;
            m_has_deferred_async_scroll_present = true;
            m_deferred_async_scroll_present_viewport_rect = async_scroll_viewport_rect;
        }
        invoke_on_main_thread([] {
            HTML::main_thread_event_loop().queue_task_to_update_the_rendering();
        });
        return true;
    }

    bool has_presentable_work() const
    {
        return (m_has_deferred_async_scroll_present && m_queued_rasterization_tasks == 0)
            || (m_needs_present && m_queued_rasterization_tasks == 0);
    }

    bool can_present_deferred_async_scroll() const
    {
        Sync::MutexLocker const locker { m_mutex };
        return m_has_deferred_async_scroll_present && m_queued_rasterization_tasks == 0;
    }

    void present_frame(Gfx::IntRect viewport_rect, Optional<u64> presenting_frame_id = {}, PresentFrameDelivery delivery = PresentFrameDelivery::MainThread)
    {
        auto delivery_name = delivery == PresentFrameDelivery::AsyncScroll ? "compositor-thread"sv : "main-thread"sv;
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Begin {} present (frame={}, raster_tasks={}, viewport={}x{} at {},{})",
            delivery_name, presenting_frame_id.value_or(0), m_queued_rasterization_tasks.load(), viewport_rect.width(), viewport_rect.height(), viewport_rect.x(), viewport_rect.y());

        {
            Sync::MutexLocker const locker { m_mutex };
            if (delivery == PresentFrameDelivery::AsyncScroll && m_queued_rasterization_tasks > 0) {
                m_has_deferred_async_scroll_present = true;
                m_deferred_async_scroll_present_viewport_rect = viewport_rect;
                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Deferring async scroll present until a backing store is ready (raster_tasks={})",
                    m_queued_rasterization_tasks.load());
                return;
            }
            VERIFY(m_queued_rasterization_tasks == 0);
            if (m_exit)
                return;
            m_is_rasterizing = true;
        }

        auto presentation_mode = [this] {
            Sync::MutexLocker const locker { m_mutex };
            return m_presentation_mode;
        }();

        if (m_cached_display_list && m_backing_stores.is_valid()) {
            auto should_clear_back_store = presentation_mode.visit(
                [](CompositorThread::PresentToUI) { return false; },
                [](CompositorThread::PublishToExternalContent const&) { return true; });
            if (should_clear_back_store) {
                // Embedded navigables leave their PaintConfig canvas unfilled, so double-buffered back stores must be
                // cleared before repainting.
                m_backing_stores.back_store->canvas().clear(SK_ColorTRANSPARENT);
            }
            m_skia_player->execute(*m_cached_display_list, m_cached_scroll_state_snapshot, *m_backing_stores.back_store);
            paint_viewport_scrollbar_overlay(*m_backing_stores.back_store);
            flush_surface(*m_backing_stores.back_store);
            i32 rendered_bitmap_id = m_backing_stores.back_bitmap_id;
            m_backing_stores.swap();
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Finished {} display-list replay into bitmap {}",
                delivery_name, rendered_bitmap_id);

            presentation_mode.visit(
                [this, viewport_rect, rendered_bitmap_id, delivery](CompositorThread::PresentToUI) {
                    if (m_presents_to_client) {
                        finish_rasterizing(rendered_bitmap_id);
                        if (delivery == PresentFrameDelivery::AsyncScroll) {
                            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Finished async scroll raster into bitmap {} (raster_tasks={})",
                                rendered_bitmap_id, m_queued_rasterization_tasks.load());
                        }
                        VERIFY(CompositorThread::present_frame_to_client(m_page_id, viewport_rect, rendered_bitmap_id));
                    } else {
                        Sync::MutexLocker const locker { m_mutex };
                        m_is_rasterizing = false;
                    }
                },
                [this](CompositorThread::PublishToExternalContent const& mode) {
                    {
                        Sync::MutexLocker const locker { m_mutex };
                        m_is_rasterizing = false;
                    }
                    if (m_has_async_scrolling_state)
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Publishing present to external content source");
                    auto snapshot = Gfx::DecodedImageFrame { *m_backing_stores.front_store->snapshot_bitmap() };
                    mode.source->update(move(snapshot));
                });
        } else {
            {
                Sync::MutexLocker const locker { m_mutex };
                m_is_rasterizing = false;
            }
            if (m_has_async_scrolling_state) {
                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Skipping {} present: cached_display_list={}, backing_stores_valid={}",
                    delivery_name, !!m_cached_display_list, m_backing_stores.is_valid());
            }
        }

        if (presenting_frame_id.has_value())
            mark_frame_complete(*presenting_frame_id);
    }

    void initialize_skia_player(DisplayListPlayerType display_list_player_type)
    {
        switch (display_list_player_type) {
        case DisplayListPlayerType::SkiaGPUIfAvailable:
            m_skia_backend_context = Gfx::SkiaBackendContext::create_independent_gpu_backend();
            break;
        case DisplayListPlayerType::SkiaCPU:
            break;
        }
        m_skia_player = make<Painting::DisplayListPlayerSkia>(m_skia_backend_context);
    }

    void publish_backing_store_pair(UpdateBackingStoresCommand& cmd, Gfx::SharedImage front_shared_image, Gfx::SharedImage back_shared_image)
    {
        if (!m_presents_to_client)
            return;

        VERIFY(CompositorThread::present_backing_stores_to_client(m_page_id, cmd.front_bitmap_id, move(front_shared_image), cmd.back_bitmap_id, move(back_shared_image)));
    }

    void allocate_backing_stores(UpdateBackingStoresCommand& cmd)
    {
#ifdef USE_VULKAN_DMABUF_IMAGES
        if (m_skia_backend_context && m_presents_to_client) {
            auto backing_stores = create_linear_dmabuf_backing_stores(cmd.size, *m_skia_backend_context);
            if (!backing_stores.is_error()) {
                auto backing_store_pair = backing_stores.release_value();
                m_backing_stores.front_store = move(backing_store_pair.front);
                m_backing_stores.back_store = move(backing_store_pair.back);
                publish_backing_store_pair(cmd, move(backing_store_pair.front_shared_image), move(backing_store_pair.back_shared_image));
                return;
            }
        }
#endif

        auto front_buffer = Gfx::SharedImageBuffer::create(cmd.size);
        auto back_buffer = Gfx::SharedImageBuffer::create(cmd.size);
        auto front_shared_image = front_buffer.export_shared_image();
        auto back_shared_image = back_buffer.export_shared_image();
        auto backing_store_pair = create_shareable_bitmap_backing_stores(cmd.size, front_buffer, back_buffer, m_skia_backend_context);
        m_backing_stores.front_store = move(backing_store_pair.front);
        m_backing_stores.back_store = move(backing_store_pair.back);
        publish_backing_store_pair(cmd, move(front_shared_image), move(back_shared_image));
        if (m_has_async_scrolling_state) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Allocated bitmap backing stores front={} back={} size={}x{}",
                cmd.front_bitmap_id, cmd.back_bitmap_id, cmd.size.width(), cmd.size.height());
        }
    }

    template<typename Invokee>
    void invoke_on_main_thread(Invokee invokee)
    {
        if (m_exit)
            return;
        auto event_loop = m_main_thread_event_loop->take();
        if (!event_loop)
            return;
        event_loop->deferred_invoke([self = NonnullRefPtr(*this), invokee = move(invokee)]() mutable {
            invokee();
        });
    }

    u64 m_page_id { 0 };
    NonnullRefPtr<Core::WeakEventLoopReference> m_main_thread_event_loop;
    bool m_presents_to_client { false };

    mutable Sync::Mutex m_mutex;
    mutable Sync::ConditionVariable m_command_ready { m_mutex };
    Atomic<bool> m_exit { false };

    Queue<CompositorCommand> m_command_queue;

    OwnPtr<Painting::DisplayListPlayerSkia> m_skia_player;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    RefPtr<Painting::DisplayList> m_cached_display_list;
    Painting::ScrollStateSnapshot m_cached_scroll_state_snapshot;
    Vector<ViewportScrollbar> m_viewport_scrollbars;
    Optional<size_t> m_hovered_viewport_scrollbar_index;
    Optional<size_t> m_captured_viewport_scrollbar_index;
    float m_viewport_scrollbar_thumb_grab_position { 0 };
    mutable Sync::Mutex m_async_scroll_tree_mutex;
    AsyncScrollTree m_async_scroll_tree;
    BackingStoreState m_backing_stores;
    CompositorThread::PresentationMode m_presentation_mode { CompositorThread::PresentToUI {} };

    Atomic<i32> m_queued_rasterization_tasks { 0 };
    Optional<i32> m_presented_bitmap_id_awaiting_ack;
    mutable Sync::ConditionVariable m_ready_to_paint { m_mutex };
    bool m_is_rasterizing { false };

    bool m_needs_present { false };
    Gfx::IntRect m_pending_viewport_rect;
    bool m_has_deferred_async_scroll_present { false };
    Gfx::IntRect m_deferred_async_scroll_present_viewport_rect;

    u64 m_submitted_frame_id { 0 };
    u64 m_completed_frame_id { 0 };
    mutable Sync::ConditionVariable m_frame_completed { m_mutex };
    Optional<Gfx::FloatPoint> m_pending_async_viewport_scroll_offset;
    Gfx::IntRect m_async_scrolling_viewport_rect;
    Atomic<bool> m_has_async_scrolling_state { false };
    Atomic<bool> m_can_accept_async_wheel_events { false };
    u64 m_wheel_event_listener_state_generation { 0 };
    WheelRoutingAdmission m_wheel_routing_admission { WheelRoutingAdmission::NoAsyncScrollingState };

public:
    void finish_rasterizing(i32 bitmap_id)
    {
        Sync::MutexLocker const locker { m_mutex };
        m_is_rasterizing = false;
        VERIFY(!m_presented_bitmap_id_awaiting_ack.has_value());
        m_presented_bitmap_id_awaiting_ack = bitmap_id;
        m_queued_rasterization_tasks++;
        VERIFY(m_queued_rasterization_tasks == 1);
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rasterized bitmap {} waiting for ready_to_paint (raster_tasks={})",
            bitmap_id, m_queued_rasterization_tasks.load());
    }

    void decrement_queued_tasks(i32 bitmap_id)
    {
        Sync::MutexLocker const locker { m_mutex };
        if (m_presented_bitmap_id_awaiting_ack != bitmap_id) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Ignoring stale ready_to_paint for bitmap {} while awaiting bitmap {} (raster_tasks={})",
                bitmap_id, m_presented_bitmap_id_awaiting_ack.value_or(-1), m_queued_rasterization_tasks.load());
            return;
        }

        VERIFY(m_queued_rasterization_tasks == 1);
        m_presented_bitmap_id_awaiting_ack.clear();
        m_queued_rasterization_tasks--;
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor ready_to_paint released bitmap {} (raster_tasks={})",
            bitmap_id, m_queued_rasterization_tasks.load());
        m_ready_to_paint.signal();
        m_command_ready.signal();
    }
};

struct FramePresentationState {
    RefPtr<Core::WeakEventLoopReference> event_loop;
    CompositorThread::BackingStorePresentationCallback backing_store_callback;
    CompositorThread::FramePresentationCallback frame_callback;
};

static Sync::Mutex& compositor_presentation_state_mutex()
{
    static NeverDestroyed<Sync::Mutex> mutex;
    return *mutex;
}

static HashMap<u64, NonnullRefPtr<CompositorThread::ThreadData>>& page_compositors()
{
    static NeverDestroyed<HashMap<u64, NonnullRefPtr<CompositorThread::ThreadData>>> compositors;
    return *compositors;
}

static FramePresentationState& frame_presentation_state()
{
    static NeverDestroyed<FramePresentationState> state;
    return *state;
}

CompositorThread::CompositorThread(u64 page_id, PagePresentationRegistration page_presentation_registration)
    : m_thread_data(adopt_ref(*new ThreadData(page_id, Core::EventLoop::current_weak(), page_presentation_registration)))
{
    if (page_presentation_registration == PagePresentationRegistration::Yes)
        register_page_compositor(page_id, m_thread_data);
}

CompositorThread::~CompositorThread()
{
    unregister_page_compositor(m_thread_data->page_id(), *m_thread_data);
    m_thread_data->exit();
}

void CompositorThread::register_page_compositor(u64 page_id, NonnullRefPtr<ThreadData> thread_data)
{
    Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
    auto replaced_existing_compositor = page_compositors().contains(page_id);
    page_compositors().set(page_id, move(thread_data));
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Registered page {} for compositor presentation (replaced_existing={})",
        page_id, replaced_existing_compositor);
}

void CompositorThread::unregister_page_compositor(u64 page_id, ThreadData& thread_data)
{
    Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
    auto compositor = page_compositors().find(page_id);
    if (compositor == page_compositors().end()) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Skipping compositor unregister for page {}: no compositor registered",
            page_id);
        return;
    }
    if (compositor->value.ptr() != &thread_data) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Skipping compositor unregister for page {}: compositor changed",
            page_id);
        return;
    }
    page_compositors().remove(compositor);
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Unregistered page {} from compositor presentation", page_id);
}

void CompositorThread::set_frame_presentation_callbacks(NonnullRefPtr<Core::WeakEventLoopReference> event_loop, BackingStorePresentationCallback backing_store_callback, FramePresentationCallback frame_callback)
{
    Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
    auto& state = frame_presentation_state();
    state.event_loop = move(event_loop);
    state.backing_store_callback = move(backing_store_callback);
    state.frame_callback = move(frame_callback);
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Installed compositor presentation callbacks");
}

void CompositorThread::clear_frame_presentation_callbacks()
{
    Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
    auto& state = frame_presentation_state();
    state.event_loop = nullptr;
    state.backing_store_callback = {};
    state.frame_callback = {};
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cleared compositor presentation callbacks");
}

bool CompositorThread::present_backing_stores_to_client(u64 page_id, i32 front_bitmap_id, Gfx::SharedImage&& front_shared_image, i32 back_bitmap_id, Gfx::SharedImage&& back_shared_image)
{
    RefPtr<Core::WeakEventLoopReference> event_loop_reference;
    {
        Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
        if (!page_compositors().contains(page_id)) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present backing stores front={} back={} for page {}: no compositor registered",
                front_bitmap_id, back_bitmap_id, page_id);
            return false;
        }
        auto& state = frame_presentation_state();
        if (!state.backing_store_callback) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present backing stores front={} back={} for page {}: no presentation callback",
                front_bitmap_id, back_bitmap_id, page_id);
            return false;
        }
        event_loop_reference = state.event_loop;
    }

    if (!event_loop_reference) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present backing stores front={} back={} for page {}: no presentation event loop",
            front_bitmap_id, back_bitmap_id, page_id);
        return false;
    }
    auto event_loop = event_loop_reference->take();
    if (!event_loop) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present backing stores front={} back={} for page {}: presentation event loop is gone",
            front_bitmap_id, back_bitmap_id, page_id);
        return false;
    }

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Queueing UI backing stores for page {} front={} back={}",
        page_id, front_bitmap_id, back_bitmap_id);
    event_loop->deferred_invoke([page_id, front_bitmap_id, front_shared_image = move(front_shared_image), back_bitmap_id, back_shared_image = move(back_shared_image)]() mutable {
        Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
        if (!page_compositors().contains(page_id)) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Dropping queued UI backing stores for page {} front={} back={}: page unregistered",
                page_id, front_bitmap_id, back_bitmap_id);
            return;
        }
        auto& state = frame_presentation_state();
        if (state.backing_store_callback) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Delivering UI backing stores for page {} front={} back={}",
                page_id, front_bitmap_id, back_bitmap_id);
            state.backing_store_callback(page_id, front_bitmap_id, move(front_shared_image), back_bitmap_id, move(back_shared_image));
        } else {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Dropping queued UI backing stores for page {} front={} back={}: callback cleared",
                page_id, front_bitmap_id, back_bitmap_id);
        }
    });
    return true;
}

bool CompositorThread::present_frame_to_client(u64 page_id, Gfx::IntRect const& viewport_rect, i32 bitmap_id)
{
    RefPtr<Core::WeakEventLoopReference> event_loop_reference;
    {
        Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
        if (!page_compositors().contains(page_id)) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present bitmap {} for page {}: no compositor registered",
                bitmap_id, page_id);
            return false;
        }
        auto& state = frame_presentation_state();
        if (!state.frame_callback) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present bitmap {} for page {}: no presentation callback",
                bitmap_id, page_id);
            return false;
        }
        event_loop_reference = state.event_loop;
    }

    if (!event_loop_reference) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present bitmap {} for page {}: no presentation event loop",
            bitmap_id, page_id);
        return false;
    }
    auto event_loop = event_loop_reference->take();
    if (!event_loop) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present bitmap {} for page {}: presentation event loop is gone",
            bitmap_id, page_id);
        return false;
    }

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Queueing UI present for page {} bitmap {} viewport={}x{} at {},{}",
        page_id, bitmap_id, viewport_rect.width(), viewport_rect.height(), viewport_rect.x(), viewport_rect.y());
    event_loop->deferred_invoke([page_id, viewport_rect, bitmap_id] {
        Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
        if (!page_compositors().contains(page_id)) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Dropping queued UI present for page {} bitmap {}: page unregistered",
                page_id, bitmap_id);
            return;
        }
        auto& state = frame_presentation_state();
        if (state.frame_callback) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Delivering UI present for page {} bitmap {} viewport={}x{} at {},{}",
                page_id, bitmap_id, viewport_rect.width(), viewport_rect.height(), viewport_rect.x(), viewport_rect.y());
            state.frame_callback(page_id, viewport_rect, bitmap_id);
        } else {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Dropping queued UI present for page {} bitmap {}: callback cleared",
                page_id, bitmap_id);
        }
    });
    return true;
}

void CompositorThread::presented_bitmap_ready_to_paint(u64 page_id, i32 bitmap_id)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Received compositor ready_to_paint for page {} bitmap {}",
        page_id, bitmap_id);
    RefPtr<ThreadData> thread_data;
    {
        Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
        auto compositor = page_compositors().find(page_id);
        if (compositor == page_compositors().end()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Ignoring compositor ready_to_paint for page {} bitmap {}: no compositor registered",
                page_id, bitmap_id);
            return;
        }
        thread_data = compositor->value;
    }
    thread_data->decrement_queued_tasks(bitmap_id);
}

bool CompositorThread::async_scroll_by(u64 page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels)
{
    RefPtr<ThreadData> thread_data;
    {
        Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
        auto compositor = page_compositors().find(page_id);
        if (compositor == page_compositors().end()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Async scroll cannot scroll page {}: no compositor registered", page_id);
            return false;
        }
        thread_data = compositor->value;
    }
    return thread_data->enqueue_async_scroll_by(position, delta_in_device_pixels);
}

bool CompositorThread::handle_mouse_event(u64 page_id, MouseEvent const& event)
{
    RefPtr<ThreadData> thread_data;
    {
        Sync::MutexLocker const locker { compositor_presentation_state_mutex() };
        auto compositor = page_compositors().find(page_id);
        if (compositor == page_compositors().end()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Viewport scrollbar mouse event cannot be handled for page {}: no compositor registered", page_id);
            return false;
        }
        thread_data = compositor->value;
    }
    return thread_data->handle_viewport_scrollbar_mouse_event(event);
}

void CompositorThread::start(DisplayListPlayerType display_list_player_type)
{
    m_thread = Threading::Thread::construct("Compositor"sv, [thread_data = m_thread_data, display_list_player_type] {
        thread_data->compositor_loop(display_list_player_type);
        return static_cast<intptr_t>(0);
    });
    m_thread->start();
    m_thread->detach();
}

void CompositorThread::set_presentation_mode(PresentationMode mode)
{
    m_thread_data->set_presentation_mode(move(mode));
}

void CompositorThread::stop_presenting_to_client()
{
    m_thread_data->stop_presenting_to_client();
    unregister_page_compositor(m_thread_data->page_id(), *m_thread_data);
}

void CompositorThread::update_display_list(NonnullRefPtr<Painting::DisplayList> display_list, Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    m_thread_data->enqueue_command(UpdateDisplayListCommand { move(display_list), move(scroll_state_snapshot), {} });
}

void CompositorThread::update_display_list_and_async_scrolling_state(NonnullRefPtr<Painting::DisplayList> display_list, Painting::ScrollStateSnapshot&& scroll_state_snapshot, AsyncScrollingState&& async_scrolling_state)
{
    m_thread_data->enqueue_command(UpdateDisplayListCommand { move(display_list), move(scroll_state_snapshot), Optional<AsyncScrollingState> { move(async_scrolling_state) } });
}

void CompositorThread::invalidate_wheel_event_listener_state(u64 generation)
{
    m_thread_data->invalidate_wheel_event_listener_state(generation);
}

bool CompositorThread::async_scroll_by(Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect)
{
    return m_thread_data->enqueue_async_scroll_by(position, delta_in_device_pixels, viewport_rect);
}

Optional<Gfx::FloatPoint> CompositorThread::pending_async_viewport_scroll_offset() const
{
    return m_thread_data->pending_async_viewport_scroll_offset();
}

bool CompositorThread::should_defer_async_viewport_scroll_offset_adoption() const
{
    return m_thread_data->should_defer_async_viewport_scroll_offset_adoption();
}

bool CompositorThread::should_defer_main_thread_present_for_async_scroll() const
{
    return m_thread_data->should_defer_main_thread_present_for_async_scroll();
}

Optional<Gfx::FloatPoint> CompositorThread::take_pending_async_viewport_scroll_offset()
{
    return m_thread_data->take_pending_async_viewport_scroll_offset();
}

void CompositorThread::update_scroll_state(Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    m_thread_data->enqueue_command(UpdateScrollStateCommand { move(scroll_state_snapshot) });
}

void CompositorThread::update_backing_stores(Gfx::IntSize size, i32 front_id, i32 back_id)
{
    m_thread_data->enqueue_command(UpdateBackingStoresCommand { size, front_id, back_id });
}

u64 CompositorThread::present_frame(Gfx::IntRect viewport_rect)
{
    return m_thread_data->set_needs_present(viewport_rect);
}

void CompositorThread::wait_for_frame(u64 frame_id)
{
    m_thread_data->wait_for_frame(frame_id);
}

void CompositorThread::request_screenshot(NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    m_thread_data->enqueue_command(ScreenshotCommand { move(target_surface), move(callback) });
}

}
