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
#include <LibThreading/Thread.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Compositor/CompositorThread.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/ExternalContentSource.h>

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <AK/Queue.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <AK/Array.h>
#    include <LibGfx/VulkanImage.h>
#    include <libdrm/drm_fourcc.h>
#endif

#include <core/SkCanvas.h>
#include <core/SkColor.h>

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

using CompositorCommand = Variant<UpdateDisplayListCommand, AsyncScrollByCommand, UpdateScrollStateCommand, UpdateBackingStoresCommand, ScreenshotCommand>;

static Optional<AsyncScrollNode> viewport_scroll_node(AsyncScrollingState const& state)
{
    for (auto const& node : state.scroll_nodes) {
        if (node.is_viewport)
            return node;
    }
    return {};
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
                            auto const wheel_event_listener_state_generation = async_scrolling_state.wheel_event_listener_state_generation;
                            auto wheel_routing_admission = wheel_routing_admission_for(async_scrolling_state);
                            {
                                Sync::MutexLocker const locker { m_mutex };
                                if (wheel_event_listener_state_generation < m_wheel_event_listener_state_generation)
                                    wheel_routing_admission = WheelRoutingAdmission::StaleWheelEventListeners;
                                else
                                    m_wheel_event_listener_state_generation = wheel_event_listener_state_generation;
                                m_wheel_routing_admission = wheel_routing_admission;
                                m_async_scrolling_viewport_rect = async_scrolling_state.viewport_rect;
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
                                m_async_scroll_tree.set_state(move(async_scrolling_state));
                                if (viewport_node.has_value() && pending_async_viewport_scroll_offset.has_value()) {
                                    auto delta = pending_async_viewport_scroll_offset->translated(-viewport_node->scroll_offset.x(), -viewport_node->scroll_offset.y());
                                    if (delta.x() != 0 || delta.y() != 0) {
                                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Reapplying pending async viewport offset {},{} to display list update",
                                            pending_async_viewport_scroll_offset->x(), pending_async_viewport_scroll_offset->y());
                                        m_async_scroll_tree.apply_scroll_delta(viewport_node->node_id, delta, m_cached_scroll_state_snapshot);
                                    }
                                }
                                m_async_scroll_tree.rebuild_wheel_scroll_targets(m_cached_display_list, m_cached_scroll_state_snapshot);
                            }
                            m_has_async_scrolling_state = true;
                        } else {
                            {
                                Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
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
                        {
                            Sync::MutexLocker const locker { m_mutex };
                            m_has_deferred_async_scroll_present = true;
                            m_deferred_async_scroll_present_viewport_rect = async_scroll_viewport_rect;
                        }
                        should_yield_to_async_scroll_present = can_present_deferred_async_scroll();
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor async scroll command complete (yield_to_present={}, raster_tasks={})",
                            should_yield_to_async_scroll_present, m_queued_rasterization_tasks.load());
                    },
                    [this](UpdateScrollStateCommand& cmd) {
                        m_cached_scroll_state_snapshot = move(cmd.scroll_state_snapshot);
                        if (m_has_async_scrolling_state) {
                            Sync::MutexLocker const locker { m_async_scroll_tree_mutex };
                            m_async_scroll_tree.rebuild_wheel_scroll_targets(m_cached_display_list, m_cached_scroll_state_snapshot);
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
