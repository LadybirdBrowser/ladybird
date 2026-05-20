/*
 * Copyright (c) 2025-2026, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImage.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Compositor/AsyncScrollTree.h>
#include <LibWeb/Compositor/AsyncScrollingState.h>
#include <LibWeb/Compositor/BackingStoreManager.h>
#include <LibWeb/Compositor/CompositorClient.h>
#include <LibWeb/Compositor/CompositorContextState.h>
#include <LibWeb/Compositor/CompositorThread.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>
#include <LibWeb/Painting/DisplayListResourceStorage.h>

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/Queue.h>
#include <AK/Time.h>

#include <core/SkCanvas.h>
#include <core/SkColor.h>
#include <gpu/ganesh/GrDirectContext.h>

#include <LibCore/Platform/ScopedAutoreleasePool.h>

namespace Web::Compositor {

static constexpr auto skia_deferred_cleanup_interval = AK::Duration::from_seconds(1);
static constexpr auto skia_aggressive_cleanup_interval = AK::Duration::from_seconds(5);
static constexpr auto skia_deferred_cleanup_resource_age = std::chrono::seconds(5);
static constexpr auto skia_resource_cache_high_watermark = 384 * MiB;
static constexpr auto skia_resource_cache_critical_watermark = 512 * MiB;

struct UpdateDisplayListCommand {
    NonnullRefPtr<Painting::DisplayList> display_list;
    Painting::DisplayListResourceTransaction resource_transaction;
    Painting::ScrollStateSnapshot scroll_state_snapshot;
};

struct AsyncScrollByCommand {
    Gfx::FloatPoint position;
    Gfx::FloatPoint delta_in_device_pixels;
    Gfx::IntRect viewport_rect;
    AsyncScrollNodeID scroll_target;
    Optional<AsyncScrollOperationID> operation_id;
};

struct ViewportScrollbarDragCommand {
    size_t scrollbar_index { 0 };
    float primary_position { 0 };
    float thumb_grab_position { 0 };
};

struct UpdateScrollStateCommand {
    Painting::ScrollStateSnapshot scroll_state_snapshot;
};

struct UpdateVideoFrameCommand {
    Painting::VideoFrameResourceId frame_id;
    NonnullRefPtr<Media::VideoFrame const> frame;
};

struct ClearVideoFrameCommand {
    Painting::VideoFrameResourceId frame_id;
};

struct UpdateCompositorSurfaceCommand {
    Painting::CompositorSurfaceId surface_id;
    Gfx::SharedImage shared_image;
};

struct ClearCompositorSurfaceCommand {
    Painting::CompositorSurfaceId surface_id;
};

struct ViewportSizeUpdatedCommand {
    Gfx::IntSize viewport_size;
    bool is_top_level_traversable { false };
    WindowResizingInProgress window_resize_in_progress { WindowResizingInProgress::No };
};

struct PresentFrameCommand {
    Gfx::IntRect viewport_rect;
};

struct ScreenshotCommand {
    NonnullRefPtr<Gfx::PaintingSurface> target_surface;
    ScreenshotRequestId request_id;
};

using CompositorCommand = Variant<UpdateDisplayListCommand, AsyncScrollByCommand, ViewportScrollbarDragCommand,
    UpdateScrollStateCommand, UpdateVideoFrameCommand, ClearVideoFrameCommand, UpdateCompositorSurfaceCommand,
    ClearCompositorSurfaceCommand, ViewportSizeUpdatedCommand, PresentFrameCommand, ScreenshotCommand>;

struct CompositorCommandEnvelope {
    CompositorContextId context_id;
    CompositorCommand command;
};

static void flush_surface(Gfx::PaintingSurface& surface)
{
    if (auto context = surface.skia_backend_context()) {
        context->flush_and_submit(&surface.sk_surface());
        auto* skia_context = context->sk_context();

        static thread_local Optional<MonotonicTime> s_last_deferred_cleanup;
        static thread_local Optional<MonotonicTime> s_last_aggressive_cleanup;

        auto const now = MonotonicTime::now();
        if (!s_last_deferred_cleanup.has_value() || now - *s_last_deferred_cleanup >= skia_deferred_cleanup_interval) {
            s_last_deferred_cleanup = now;
            skia_context->performDeferredCleanup(skia_deferred_cleanup_resource_age);

            size_t resource_bytes = 0;
            skia_context->getResourceCacheUsage(nullptr, &resource_bytes);
            if (resource_bytes >= skia_resource_cache_high_watermark && (!s_last_aggressive_cleanup.has_value() || now - *s_last_aggressive_cleanup >= skia_aggressive_cleanup_interval)) {
                s_last_aggressive_cleanup = now;
                skia_context->performDeferredCleanup(std::chrono::milliseconds(0));
                skia_context->getResourceCacheUsage(nullptr, &resource_bytes);
                if (resource_bytes >= skia_resource_cache_critical_watermark)
                    skia_context->purgeUnlockedResources(GrPurgeResourceOptions::kScratchResourcesOnly);
            }
        }
    }
    surface.flush();
}

static constexpr u64 page_presenting_context_id_tag = 1ull << 63;

static CompositorContextId compositor_context_id_for_page(u64 page_id)
{
    VERIFY((page_id & page_presenting_context_id_tag) == 0);
    return CompositorContextId { page_presenting_context_id_tag | page_id };
}

static bool is_page_presenting_context_id(CompositorContextId context_id)
{
    return (context_id.value() & page_presenting_context_id_tag) != 0;
}

class CompositorThread::ThreadData final : public AtomicRefCounted<ThreadData> {
public:
    explicit ThreadData(NonnullRefPtr<CompositorMainThreadClient> main_thread_client)
        : m_main_thread_client(move(main_thread_client))
    {
    }

    ~ThreadData() = default;

    struct ContextState final
        : public AtomicRefCounted<ContextState>
        , public CompositorContextState {
        ContextState(Optional<u64> page_id, PagePresentationRegistration page_presentation_registration)
            : CompositorContextState(page_id, page_presentation_registration)
        {
        }

        mutable Sync::Mutex state_mutex;
    };

    void register_context(CompositorContextId context_id, Optional<u64> page_id, PagePresentationRegistration page_presentation_registration)
    {
        Sync::MutexLocker const locker { m_mutex };
        VERIFY(!m_contexts.contains(context_id));
        if (page_presentation_registration == PagePresentationRegistration::Yes) {
            VERIFY(page_id.has_value());
            VERIFY(context_id == compositor_context_id_for_page(*page_id));
        } else {
            VERIFY(!page_id.has_value());
            VERIFY(!is_page_presenting_context_id(context_id));
        }
        m_contexts.set(context_id, adopt_ref(*new ContextState(page_id, page_presentation_registration)));
    }

    void unregister_context(CompositorContextId context_id)
    {
        Sync::MutexLocker const locker { m_mutex };
        m_contexts.remove(context_id);
    }

    RefPtr<ContextState> context_state(CompositorContextId context_id)
    {
        Sync::MutexLocker const locker { m_mutex };
        return m_contexts.get(context_id).value_or(nullptr);
    }

    void stop_presenting_to_client(CompositorContextId context_id)
    {
        Sync::MutexLocker const locker { m_mutex };
        if (auto context = m_contexts.get(context_id).value_or(nullptr))
            context->presents_to_client = false;
    }

    void set_ui_presentation_client(NonnullRefPtr<CompositorUIPresentationClient> ui_presentation_client)
    {
        Sync::MutexLocker const locker { m_mutex };
        m_ui_presentation_client = move(ui_presentation_client);
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Installed compositor UI presentation client");
    }

    void clear_ui_presentation_client()
    {
        Sync::MutexLocker const locker { m_mutex };
        m_ui_presentation_client = nullptr;
        bool did_clear_pending_presentation = false;
        for (auto& context : m_contexts) {
            if (!context.value->presented_bitmap_id_awaiting_ack.has_value())
                continue;
            context.value->presented_bitmap_id_awaiting_ack.clear();
            did_clear_pending_presentation = true;
        }
        if (did_clear_pending_presentation)
            m_command_ready.signal();
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cleared compositor UI presentation client");
    }

    void set_presentation_mode(CompositorContextId context_id, PresentationMode mode)
    {
        Sync::MutexLocker const locker { m_mutex };
        if (auto context = m_contexts.get(context_id).value_or(nullptr))
            context->presentation_mode = move(mode);
    }

    void exit()
    {
        Sync::MutexLocker const locker { m_mutex };
        m_exit = true;
        m_command_ready.signal();
    }

    void enqueue_command(CompositorContextId context_id, CompositorCommand&& command)
    {
        Sync::MutexLocker const locker { m_mutex };
        m_command_queue.enqueue({ context_id, move(command) });
        if constexpr (COMPOSITOR_DEBUG) {
            auto context = m_contexts.get(context_id).value_or(nullptr);
            dbgln("[Compositor] Compositor command queued (queue_size={}, awaiting_bitmap={}, needs_present={}, deferred_async_present={})",
                m_command_queue.size(),
                context ? context->presented_bitmap_id_awaiting_ack.value_or(-1) : -1,
                context ? context->needs_present : false,
                context ? context->has_deferred_async_scroll_present : false);
        }
        m_command_ready.signal();
    }

    void enqueue_present_frame(CompositorContextId context_id, Gfx::IntRect viewport_rect)
    {
        Sync::MutexLocker const locker { m_mutex };
        if (!m_contexts.contains(context_id))
            return;
        m_command_queue.enqueue({ context_id, PresentFrameCommand { viewport_rect } });
        m_command_ready.signal();
    }

    PendingAsyncScrollUpdates take_pending_async_scroll_updates(CompositorContextId context_id)
    {
        Sync::MutexLocker const locker { m_mutex };
        if (auto context = m_contexts.get(context_id).value_or(nullptr))
            return context->take_pending_async_scroll_updates();
        return {};
    }

    bool should_defer_async_scroll_offset_adoption(CompositorContextId context_id) const
    {
        Sync::MutexLocker const locker { m_mutex };
        auto context = m_contexts.get(context_id).value_or(nullptr);
        if (!context)
            return false;
        return !context->pending_async_scroll_offsets.is_empty()
            && context->is_rasterizing;
    }

    bool should_defer_main_thread_present_for_async_scroll(CompositorContextId context_id) const
    {
        Sync::MutexLocker const locker { m_mutex };
        auto context = m_contexts.get(context_id).value_or(nullptr);
        if (!context)
            return false;
        return !context->pending_async_scroll_offsets.is_empty()
            && (context->is_rasterizing || context->has_deferred_async_scroll_present || context->has_presented_bitmap_awaiting_ack());
    }

    void invalidate_wheel_event_listener_state(CompositorContextId context_id, u64 generation)
    {
        Sync::MutexLocker const locker { m_mutex };
        auto context = m_contexts.get(context_id).value_or(nullptr);
        if (!context)
            return;
        context->wheel_event_listener_state_generation = max(context->wheel_event_listener_state_generation, generation);
        context->wheel_routing_admission = WheelRoutingAdmission::StaleWheelEventListeners;
        context->can_accept_async_wheel_events = false;
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Invalidated compositor wheel listener state (generation={})", generation);
    }

    AsyncScrollEnqueueResult enqueue_async_scroll_by(CompositorContextId context_id, UniqueNodeID expected_document_id, Gfx::FloatPoint position,
        Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, AsyncScrollOperationTracking operation_tracking)
    {
        auto context_state = this->context_state(context_id);
        if (!context_state)
            return {};
        auto& context = *context_state;
        auto rejected_wheel_routing_admission = [&]() -> Optional<WheelRoutingAdmission> {
            Sync::MutexLocker const locker { m_mutex };
            if (context.can_accept_async_wheel_events)
                return {};
            return context.wheel_routing_admission;
        }();
        if (rejected_wheel_routing_admission.has_value()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: compositor cannot accept async wheel events ({})",
                wheel_routing_admission_to_string(*rejected_wheel_routing_admission));
            return {};
        }
        auto scroll_target = [&] {
            Sync::MutexLocker const locker { context.state_mutex };
            return context.hit_test_scroll_node_for_wheel(position, delta_in_device_pixels);
        }();
        if (scroll_target.blocked_by_main_thread_region) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: main-thread wheel region at {},{} device delta {},{}",
                position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            return {};
        }
        if (scroll_target.blocked_by_wheel_event_region) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: blocking wheel event region at {},{} device delta {},{}",
                position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            return {};
        }
        if (!scroll_target.node_id.has_value()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: no wheel target at {},{} for device delta {},{}",
                position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            return {};
        }
        if (scroll_target.node_id->document_id != expected_document_id) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: stale wheel target at {},{} for current document",
                position.x(), position.y());
            return {};
        }

        Optional<AsyncScrollOperationID> operation_id;
        if (operation_tracking == AsyncScrollOperationTracking::Yes) {
            Sync::MutexLocker const locker { m_mutex };
            operation_id = context.next_async_scroll_operation_id();
        }
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor accepted main-thread async scroll enqueue at {},{} device delta {},{} for scroll node {} viewport={}x{} at {},{}",
            position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y(), scroll_target.node_id->scroll_frame_index.value(), viewport_rect.width(), viewport_rect.height(), viewport_rect.x(), viewport_rect.y());
        enqueue_command(context_id, AsyncScrollByCommand { position, delta_in_device_pixels, viewport_rect, *scroll_target.node_id, operation_id });
        return { true, operation_id };
    }

    bool enqueue_async_scroll_by(u64 page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels)
    {
        auto context_id = compositor_context_id_for_page(page_id);
        auto context_state = this->context_state(context_id);
        if (!context_state)
            return false;
        auto& context = *context_state;
        if (!context.presents_to_client)
            return false;
        auto rejected_wheel_routing_admission = [&]() -> Optional<WheelRoutingAdmission> {
            Sync::MutexLocker const locker { m_mutex };
            if (context.can_accept_async_wheel_events)
                return {};
            return context.wheel_routing_admission;
        }();
        if (rejected_wheel_routing_admission.has_value()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: compositor cannot accept async wheel events ({})",
                wheel_routing_admission_to_string(*rejected_wheel_routing_admission));
            return false;
        }

        auto viewport_rect = [&] {
            Sync::MutexLocker const locker { m_mutex };
            return context.async_scrolling_viewport_rect;
        }();
        auto scroll_target = [&] {
            Sync::MutexLocker const locker { context.state_mutex };
            return context.hit_test_scroll_node_for_wheel(position, delta_in_device_pixels);
        }();
        if (scroll_target.blocked_by_main_thread_region) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: main-thread wheel region at {},{} device delta {},{}",
                position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            return false;
        }
        if (scroll_target.blocked_by_wheel_event_region) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: blocking wheel event region at {},{} device delta {},{}",
                position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            return false;
        }
        if (!scroll_target.node_id.has_value()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rejecting async scroll enqueue: no wheel target at {},{} for device delta {},{}",
                position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y());
            return false;
        }

        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor accepted async scroll enqueue at {},{} device delta {},{} for scroll node {} viewport={}x{} at {},{}",
            position.x(), position.y(), delta_in_device_pixels.x(), delta_in_device_pixels.y(), scroll_target.node_id->scroll_frame_index.value(), viewport_rect.width(), viewport_rect.height(), viewport_rect.x(), viewport_rect.y());
        enqueue_command(context_id, AsyncScrollByCommand { position, delta_in_device_pixels, viewport_rect, *scroll_target.node_id, {} });
        return true;
    }

    bool handle_viewport_scrollbar_mouse_event(u64 page_id, MouseEvent const& event)
    {
        auto context_id = compositor_context_id_for_page(page_id);
        auto context_state = this->context_state(context_id);
        if (!context_state)
            return false;
        auto& context = *context_state;
        if (!context.presents_to_client)
            return false;
        auto position = Gfx::FloatPoint {
            static_cast<float>(event.position.x().value()),
            static_cast<float>(event.position.y().value()),
        };

        switch (event.type) {
        case MouseEvent::Type::MouseDown: {
            if (event.button != UIEvents::MouseButton::Primary)
                return false;

            auto drag = [&] {
                Sync::MutexLocker const locker { context.state_mutex };
                return context.begin_viewport_scrollbar_drag(position);
            }();
            if (!drag.has_value())
                return false;

            schedule_viewport_scrollbar_present(context);
            enqueue_command(context_id, ViewportScrollbarDragCommand { drag->scrollbar_index, drag->primary_position, drag->thumb_grab_position });
            return true;
        }
        case MouseEvent::Type::MouseMove: {
            bool had_capture = false;
            Optional<CompositorContextState::ViewportScrollbarDrag> drag;
            {
                Sync::MutexLocker const locker { context.state_mutex };
                had_capture = context.captured_viewport_scrollbar_index.has_value();
                drag = context.captured_viewport_scrollbar_drag(position);
            }
            if (had_capture) {
                if (!drag.has_value())
                    return false;
                enqueue_command(context_id, ViewportScrollbarDragCommand { drag->scrollbar_index, drag->primary_position, drag->thumb_grab_position });
                return true;
            }
            auto hovered_scrollbar_index = [&] {
                Sync::MutexLocker const locker { context.state_mutex };
                return context.hit_test_viewport_scrollbar(position);
            }();
            set_hovered_viewport_scrollbar(context, hovered_scrollbar_index);
            return hovered_scrollbar_index.has_value();
        }
        case MouseEvent::Type::MouseUp: {
            auto drag = [&] {
                Sync::MutexLocker const locker { context.state_mutex };
                return context.release_captured_viewport_scrollbar_drag(position);
            }();
            if (!drag.has_value())
                return false;
            schedule_viewport_scrollbar_present(context);
            enqueue_command(context_id, ViewportScrollbarDragCommand { drag->scrollbar_index, drag->primary_position, drag->thumb_grab_position });
            return true;
        }
        case MouseEvent::Type::MouseLeave: {
            auto has_capture = [&] {
                Sync::MutexLocker const locker { context.state_mutex };
                return context.captured_viewport_scrollbar_index.has_value();
            }();
            set_hovered_viewport_scrollbar(context, {});
            return has_capture;
        }
        case MouseEvent::Type::MouseWheel:
            return false;
        }
        VERIFY_NOT_REACHED();
    }

    void install_display_list_update(ContextState& context, NonnullRefPtr<Painting::DisplayList> display_list, Painting::ScrollStateSnapshot&& scroll_state_snapshot)
    {
        context.cached_display_list = move(display_list);
        context.cached_scroll_state_snapshot = move(scroll_state_snapshot);
        auto async_scrolling_state = async_scrolling_state_from_display_list(*context.cached_display_list);
        auto async_scrolling_viewport_rect = async_scrolling_state.viewport_rect;
        auto const wheel_event_listener_state_generation = async_scrolling_state.wheel_event_listener_state_generation;
        auto wheel_routing_admission = wheel_routing_admission_for(async_scrolling_state);
        {
            Sync::MutexLocker const locker { m_mutex };
            if (wheel_event_listener_state_generation < context.wheel_event_listener_state_generation)
                wheel_routing_admission = WheelRoutingAdmission::StaleWheelEventListeners;
            else
                context.wheel_event_listener_state_generation = wheel_event_listener_state_generation;
            context.wheel_routing_admission = wheel_routing_admission;
            context.can_accept_async_wheel_events = wheel_routing_admission == WheelRoutingAdmission::Accepted;
        }
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor wheel routing admission: {} (scroll_nodes={}, sticky_areas={}, blocking_regions={})",
            wheel_routing_admission_to_string(wheel_routing_admission),
            async_scrolling_state.scroll_nodes.size(),
            async_scrolling_state.sticky_areas.size(),
            async_scrolling_state.blocking_wheel_event_regions.size());
        auto pending_async_scroll_offsets = [this, &context] {
            Sync::MutexLocker const locker { m_mutex };
            return context.pending_async_scroll_offsets;
        }();

        {
            Sync::MutexLocker const locker { context.state_mutex };
            context.set_async_scrolling_state(move(async_scrolling_state));
            if (!pending_async_scroll_offsets.is_empty()) {
                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Reapplying {} pending async scroll offset(s) to display list update",
                    pending_async_scroll_offsets.size());
                if (auto viewport_scroll_offset = context.reapply_pending_async_scroll_offsets(pending_async_scroll_offsets); viewport_scroll_offset.has_value())
                    async_scrolling_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
            }
            context.async_scroll_tree.rebuild_wheel_hit_test_targets(context.cached_display_list, context.cached_scroll_state_snapshot);
        }
        {
            Sync::MutexLocker const locker { m_mutex };
            context.async_scrolling_viewport_rect = async_scrolling_viewport_rect;
        }
        context.has_async_scrolling_state = true;
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
                auto command = [this]() -> Optional<CompositorCommandEnvelope> {
                    Sync::MutexLocker const locker { m_mutex };
                    if (m_command_queue.is_empty())
                        return {};
                    auto command = m_command_queue.dequeue();
                    return command;
                }();

                if (!command.has_value())
                    break;

                auto context_state = this->context_state(command->context_id);
                if (!context_state)
                    continue;
                auto& context = *context_state;

                bool should_yield_to_async_scroll_present = false;
                command->command.visit(
                    [this, &context](UpdateDisplayListCommand& cmd) {
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor processing display list update (deferred_async_present={})",
                            context.has_deferred_async_scroll_present);
                        context.display_list_resource_storage.apply_transaction(move(cmd.resource_transaction));
                        install_display_list_update(context, move(cmd.display_list), move(cmd.scroll_state_snapshot));
                    },
                    [this, &context, &should_yield_to_async_scroll_present](AsyncScrollByCommand& cmd) {
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor processing async scroll command at {},{} device delta {},{} for scroll node {} (deferred_async_present={})",
                            cmd.position.x(), cmd.position.y(), cmd.delta_in_device_pixels.x(), cmd.delta_in_device_pixels.y(), cmd.scroll_target.scroll_frame_index.value(), context.has_deferred_async_scroll_present);
                        auto async_scroll_viewport_rect = cmd.viewport_rect;
                        Vector<AsyncScrollOffset> scroll_offsets;
                        {
                            Sync::MutexLocker const locker { context.state_mutex };
                            scroll_offsets = context.async_scroll_tree.apply_scroll_delta(cmd.scroll_target, cmd.delta_in_device_pixels, context.cached_scroll_state_snapshot);
                            if (scroll_offsets.is_empty()) {
                                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Dropping async scroll command: scroll tree consumed no delta");
                                // The operation produced no scroll offset (e.g. already at the scroll boundary), so it
                                // cannot be reported through the pending-offset path; report its completion separately.
                                complete_async_scroll_operation(context, cmd.operation_id);
                                return;
                            }
                            context.async_scroll_tree.rebuild_wheel_hit_test_targets(context.cached_display_list, context.cached_scroll_state_snapshot);
                        }
                        if (auto viewport_scroll_offset = CompositorContextState::viewport_scroll_offset_from(scroll_offsets); viewport_scroll_offset.has_value())
                            async_scroll_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
                        store_pending_async_scroll_offsets(context, scroll_offsets, cmd.operation_id);
                        {
                            Sync::MutexLocker const locker { m_mutex };
                            context.async_scrolling_viewport_rect = async_scroll_viewport_rect;
                        }
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Stored {} pending async scroll offset(s)",
                            scroll_offsets.size());
                        m_main_thread_client->request_rendering_update();
                        {
                            Sync::MutexLocker const locker { m_mutex };
                            context.has_deferred_async_scroll_present = true;
                            context.deferred_async_scroll_present_viewport_rect = async_scroll_viewport_rect;
                        }
                        should_yield_to_async_scroll_present = can_present_deferred_async_scroll(context);
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor async scroll command complete (yield_to_present={})",
                            should_yield_to_async_scroll_present);
                    },
                    [this, &context, &should_yield_to_async_scroll_present](ViewportScrollbarDragCommand& cmd) {
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor processing viewport scrollbar drag (index={}, position={}, grab={})",
                            cmd.scrollbar_index, cmd.primary_position, cmd.thumb_grab_position);
                        if (apply_viewport_scrollbar_drag(context, cmd.scrollbar_index, cmd.primary_position, cmd.thumb_grab_position)) {
                            should_yield_to_async_scroll_present = can_present_deferred_async_scroll(context);
                            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor viewport scrollbar drag complete (yield_to_present={})",
                                should_yield_to_async_scroll_present);
                        }
                    },
                    [this, &context](UpdateScrollStateCommand& cmd) {
                        context.cached_scroll_state_snapshot = move(cmd.scroll_state_snapshot);
                        if (context.has_async_scrolling_state) {
                            auto pending_async_scroll_offsets = [this, &context] {
                                Sync::MutexLocker const locker { m_mutex };
                                return context.pending_async_scroll_offsets;
                            }();
                            Optional<Gfx::FloatPoint> reconciled_viewport_scroll_offset;
                            {
                                Sync::MutexLocker const locker { context.state_mutex };
                                // A main-thread scroll-state update can be older than compositor-side async
                                // scrolling. Preserve compositor-visible offsets in the fresh snapshot before
                                // rebuilding hit-test data from it.
                                reconciled_viewport_scroll_offset = context.reapply_pending_async_scroll_offsets(pending_async_scroll_offsets);
                                context.async_scroll_tree.rebuild_wheel_hit_test_targets(context.cached_display_list, context.cached_scroll_state_snapshot);
                            }
                            if (reconciled_viewport_scroll_offset.has_value()) {
                                Sync::MutexLocker const mutex_locker { m_mutex };
                                auto reconciled_viewport_rect = context.async_scrolling_viewport_rect;
                                reconciled_viewport_rect.set_location(reconciled_viewport_scroll_offset->to_type<int>());
                                context.async_scrolling_viewport_rect = reconciled_viewport_rect;
                            }
                        }
                    },
                    [&context](UpdateVideoFrameCommand& cmd) {
                        context.display_list_resource_storage.update_video_frame(cmd.frame_id, move(cmd.frame));
                    },
                    [&context](ClearVideoFrameCommand& cmd) {
                        context.display_list_resource_storage.clear_video_frame(cmd.frame_id);
                    },
                    [&context](UpdateCompositorSurfaceCommand& cmd) {
                        context.display_list_resource_storage.update_compositor_surface(cmd.surface_id, move(cmd.shared_image));
                    },
                    [&context](ClearCompositorSurfaceCommand& cmd) {
                        context.display_list_resource_storage.clear_compositor_surface(cmd.surface_id);
                    },
                    [this, &context](ViewportSizeUpdatedCommand& cmd) {
                        auto allocation = context.backing_store_manager.resize_backing_stores_if_needed(
                            cmd.viewport_size, cmd.is_top_level_traversable, cmd.window_resize_in_progress);
                        if (!allocation.has_value())
                            return;

                        if (context.has_async_scrolling_state) {
                            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor resizing backing stores front={} back={} size={}x{}",
                                allocation->front_bitmap_id, allocation->back_bitmap_id, allocation->size.width(), allocation->size.height());
                        }
                        if (auto publication = context.backing_store_manager.allocate_backing_stores(*allocation, m_skia_backend_context, context.presents_to_client); publication.has_value())
                            publish_backing_store_pair(context, publication.release_value());
                    },
                    [this, &context](PresentFrameCommand& cmd) {
                        {
                            Sync::MutexLocker const locker { m_mutex };
                            if (context.has_presented_bitmap_awaiting_ack()) {
                                context.needs_present = true;
                                context.pending_viewport_rect = cmd.viewport_rect;
                                return;
                            }
                        }
                        present_frame(context, cmd.viewport_rect, PresentFrameDelivery::MainThread);
                    },
                    [this, &context](ScreenshotCommand& cmd) {
                        if (!context.cached_display_list) {
                            m_main_thread_client->did_fail_screenshot(cmd.request_id);
                            return;
                        }
                        m_skia_player->execute(*context.cached_display_list, context.display_list_resource_storage, context.cached_scroll_state_snapshot, *cmd.target_surface);
                        paint_viewport_scrollbar_overlay(context, *cmd.target_surface);
                        flush_surface(*cmd.target_surface);
                        m_main_thread_client->did_complete_screenshot(cmd.request_id);
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
            RefPtr<ContextState> present_context_state;
            Gfx::IntRect viewport_rect;
            bool should_present_deferred_async_scroll = false;
            Gfx::IntRect deferred_async_scroll_viewport_rect;
            {
                Sync::MutexLocker const locker { m_mutex };
                for (auto& [_, context] : m_contexts) {
                    if (context->has_deferred_async_scroll_present && !context->has_presented_bitmap_awaiting_ack()) {
                        present_context_state = context;
                        should_present_deferred_async_scroll = true;
                        deferred_async_scroll_viewport_rect = context->deferred_async_scroll_present_viewport_rect;
                        context->has_deferred_async_scroll_present = false;
                        auto had_pending_main_thread_present = context->needs_present;
                        if (had_pending_main_thread_present)
                            context->needs_present = false;
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor selected deferred async present (pending_main_thread_present={})",
                            had_pending_main_thread_present);
                        break;
                    }

                    if (context->needs_present && !context->has_presented_bitmap_awaiting_ack()) {
                        present_context_state = context;
                        should_present = true;
                        viewport_rect = context->pending_viewport_rect;
                        context->needs_present = false;
                        break;
                    }
                }
            }

            if (should_present_deferred_async_scroll)
                present_frame(*present_context_state, deferred_async_scroll_viewport_rect, PresentFrameDelivery::AsyncScroll);
            else if (should_present)
                present_frame(*present_context_state, viewport_rect, PresentFrameDelivery::MainThread);
        }
    }

private:
    enum class PresentFrameDelivery {
        MainThread,
        AsyncScroll,
    };

    void complete_async_scroll_operation(ContextState& context, Optional<AsyncScrollOperationID> operation_id)
    {
        if (!operation_id.has_value())
            return;

        {
            Sync::MutexLocker const locker { m_mutex };
            context.record_completed_async_scroll_operation(operation_id);
        }

        // No scroll offset was produced, so nothing else will wake the main thread; schedule a rendering update so it
        // drains the completed operation and resolves the awaiting promise.
        m_main_thread_client->request_rendering_update();
    }

    void queue_rendering_update_if_async_scroll_updates_pending(ContextState& context)
    {
        auto has_pending_updates = [this, &context] {
            Sync::MutexLocker const locker { m_mutex };
            return context.has_pending_async_scroll_updates();
        }();
        if (!has_pending_updates)
            return;

        m_main_thread_client->request_rendering_update();
    }

    void store_pending_async_scroll_offsets(ContextState& context, Vector<AsyncScrollOffset> const& scroll_offsets, Optional<AsyncScrollOperationID> operation_id = {})
    {
        Sync::MutexLocker const locker { m_mutex };
        context.store_pending_async_scroll_offsets(scroll_offsets, operation_id);
    }

    void paint_viewport_scrollbar_overlay(ContextState& context, Gfx::PaintingSurface& surface)
    {
        CompositorContextState::ViewportScrollbarOverlayState overlay_state;
        {
            Sync::MutexLocker const locker { context.state_mutex };
            overlay_state = context.viewport_scrollbar_overlay_state();
        }
        CompositorContextState::paint_viewport_scrollbar_overlay(surface, overlay_state, context.cached_scroll_state_snapshot);
    }

    void schedule_viewport_scrollbar_present(ContextState& context)
    {
        Sync::MutexLocker const locker { m_mutex };
        if (context.async_scrolling_viewport_rect.is_empty())
            return;
        context.has_deferred_async_scroll_present = true;
        context.deferred_async_scroll_present_viewport_rect = context.async_scrolling_viewport_rect;
        m_command_ready.signal();
    }

    void set_hovered_viewport_scrollbar(ContextState& context, Optional<size_t> scrollbar_index)
    {
        bool changed = false;
        {
            Sync::MutexLocker const locker { context.state_mutex };
            changed = context.set_hovered_viewport_scrollbar(scrollbar_index);
        }
        if (changed)
            schedule_viewport_scrollbar_present(context);
    }

    bool apply_viewport_scrollbar_drag(ContextState& context, size_t scrollbar_index, float primary_position, float thumb_grab_position)
    {
        auto drag = [&] {
            Sync::MutexLocker const locker { context.state_mutex };
            return context.apply_viewport_scrollbar_drag(scrollbar_index, primary_position, thumb_grab_position);
        }();
        if (!drag.has_value())
            return false;

        store_pending_async_scroll_offsets(context, drag->scroll_offsets);

        Gfx::IntRect async_scroll_viewport_rect;
        {
            Sync::MutexLocker const locker { m_mutex };
            async_scroll_viewport_rect = context.async_scrolling_viewport_rect;
            async_scroll_viewport_rect.set_location(drag->viewport_scroll_offset.to_type<int>());
            context.async_scrolling_viewport_rect = async_scroll_viewport_rect;
            context.has_deferred_async_scroll_present = true;
            context.deferred_async_scroll_present_viewport_rect = async_scroll_viewport_rect;
        }
        m_main_thread_client->request_rendering_update();
        return true;
    }

    bool has_presentable_work() const
    {
        for (auto const& [_, context] : m_contexts) {
            if ((context->has_deferred_async_scroll_present || context->needs_present)
                && !context->has_presented_bitmap_awaiting_ack()) {
                return true;
            }
        }
        return false;
    }

    bool can_present_deferred_async_scroll(ContextState& context) const
    {
        Sync::MutexLocker const locker { m_mutex };
        return context.has_deferred_async_scroll_present && !context.has_presented_bitmap_awaiting_ack();
    }

    bool route_compositor_surface_to_context(CompositorContextId context_id, Painting::CompositorSurfaceId surface_id, Gfx::SharedImage&& shared_image)
    {
        if (auto target_context = context_state(context_id); target_context) {
            target_context->display_list_resource_storage.update_compositor_surface(surface_id, move(shared_image));
            return true;
        }
        return false;
    }

    void present_frame(ContextState& context, Gfx::IntRect viewport_rect, PresentFrameDelivery delivery)
    {
        auto delivery_name = delivery == PresentFrameDelivery::AsyncScroll ? "compositor-thread"sv : "main-thread"sv;

        {
            Sync::MutexLocker const locker { m_mutex };
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Begin {} present (awaiting_bitmap={}, viewport={}x{} at {},{})",
                delivery_name, context.presented_bitmap_id_awaiting_ack.value_or(-1), viewport_rect.width(), viewport_rect.height(), viewport_rect.x(), viewport_rect.y());

            if (delivery == PresentFrameDelivery::AsyncScroll && context.has_presented_bitmap_awaiting_ack()) {
                context.has_deferred_async_scroll_present = true;
                context.deferred_async_scroll_present_viewport_rect = viewport_rect;
                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Deferring async scroll present until bitmap {} is ready to paint",
                    context.presented_bitmap_id_awaiting_ack.value());
                return;
            }
            VERIFY(!context.has_presented_bitmap_awaiting_ack());
            if (m_exit)
                return;
            context.is_rasterizing = true;
        }

        auto presentation_mode = [&] {
            Sync::MutexLocker const locker { m_mutex };
            return context.presentation_mode;
        }();

        if (context.cached_display_list && context.backing_store_manager.is_valid()) {
            auto should_clear_back_store = presentation_mode.visit(
                [](Empty) { return false; },
                [](PublishToCompositorSurface const&) { return true; });
            auto& back_store = context.backing_store_manager.back_store();
            if (should_clear_back_store) {
                // Embedded navigables leave their PaintConfig canvas unfilled, so double-buffered back stores must be
                // cleared before repainting.
                back_store.canvas().clear(SK_ColorTRANSPARENT);
            }
            m_skia_player->execute(*context.cached_display_list, context.display_list_resource_storage, context.cached_scroll_state_snapshot, back_store);
            paint_viewport_scrollbar_overlay(context, back_store);
            flush_surface(back_store);
            i32 rendered_bitmap_id = context.backing_store_manager.back_bitmap_id();
            context.backing_store_manager.swap();
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Finished {} display-list replay into bitmap {}",
                delivery_name, rendered_bitmap_id);

            presentation_mode.visit(
                [this, &context, viewport_rect, rendered_bitmap_id, delivery](Empty) {
                    if (context.presents_to_client) {
                        VERIFY(context.page_id.has_value());
                        if (finish_rasterizing(context, rendered_bitmap_id)) {
                            if (delivery == PresentFrameDelivery::AsyncScroll) {
                                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Finished async scroll raster into bitmap {}",
                                    rendered_bitmap_id);
                            }
                            present_frame_to_client(context, viewport_rect, rendered_bitmap_id);
                        }
                    } else {
                        Sync::MutexLocker const locker { m_mutex };
                        context.is_rasterizing = false;
                    }
                },
                [this, &context](PublishToCompositorSurface const& mode) {
                    {
                        Sync::MutexLocker const locker { m_mutex };
                        context.is_rasterizing = false;
                    }
                    if (context.has_async_scrolling_state)
                        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Publishing present to compositor surface");
                    auto& front_store = context.backing_store_manager.front_store();
                    VERIFY(route_compositor_surface_to_context(
                        mode.target_context_id, mode.surface_id, front_store.snapshot_into_shared_image()));
                });
        } else {
            {
                Sync::MutexLocker const locker { m_mutex };
                context.is_rasterizing = false;
            }
            if (context.has_async_scrolling_state) {
                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Skipping {} present: cached_display_list={}, backing_stores_valid={}",
                    delivery_name, !!context.cached_display_list, context.backing_store_manager.is_valid());
            }
        }

        queue_rendering_update_if_async_scroll_updates_pending(context);
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

    void publish_backing_store_pair(ContextState& context, BackingStoreManager::Publication&& publication)
    {
        if (!context.presents_to_client)
            return;
        VERIFY(context.page_id.has_value());

        present_backing_stores_to_client(context, publication.front_bitmap_id, move(publication.front_shared_image), publication.back_bitmap_id, move(publication.back_shared_image));
    }

    bool present_backing_stores_to_client(ContextState& context, i32 front_bitmap_id, Gfx::SharedImage&& front_shared_image, i32 back_bitmap_id, Gfx::SharedImage&& back_shared_image)
    {
        if (!context.presents_to_client || !context.page_id.has_value()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present backing stores front={} back={}: context is not presenting",
                front_bitmap_id, back_bitmap_id);
            return false;
        }

        RefPtr<CompositorUIPresentationClient> ui_presentation_client;
        {
            Sync::MutexLocker const locker { m_mutex };
            if (!m_ui_presentation_client) {
                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present backing stores front={} back={} for page {}: no UI presentation client",
                    front_bitmap_id, back_bitmap_id, *context.page_id);
                return false;
            }
            ui_presentation_client = m_ui_presentation_client;
        }

        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Publishing UI backing stores for page {} front={} back={}",
            *context.page_id, front_bitmap_id, back_bitmap_id);
        ui_presentation_client->publish_backing_stores(*context.page_id, front_bitmap_id, move(front_shared_image), back_bitmap_id, move(back_shared_image));
        return true;
    }

    bool present_frame_to_client(ContextState& context, Gfx::IntRect const& viewport_rect, i32 bitmap_id)
    {
        if (!context.presents_to_client || !context.page_id.has_value()) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present bitmap {}: context is not presenting", bitmap_id);
            return false;
        }

        RefPtr<CompositorUIPresentationClient> ui_presentation_client;
        {
            Sync::MutexLocker const locker { m_mutex };
            if (!m_ui_presentation_client) {
                dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Cannot present bitmap {} for page {}: no UI presentation client",
                    bitmap_id, *context.page_id);
                return false;
            }
            ui_presentation_client = m_ui_presentation_client;
        }

        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Publishing UI present for page {} bitmap {} viewport={}x{} at {},{}",
            *context.page_id, bitmap_id, viewport_rect.width(), viewport_rect.height(), viewport_rect.x(), viewport_rect.y());
        ui_presentation_client->present_frame_to_ui(*context.page_id, viewport_rect, bitmap_id);
        return true;
    }

    NonnullRefPtr<CompositorMainThreadClient> m_main_thread_client;

    mutable Sync::Mutex m_mutex;
    mutable Sync::ConditionVariable m_command_ready { m_mutex };
    Atomic<bool> m_exit { false };

    Queue<CompositorCommandEnvelope> m_command_queue;
    HashMap<CompositorContextId, NonnullRefPtr<ContextState>> m_contexts;
    RefPtr<CompositorUIPresentationClient> m_ui_presentation_client;

    OwnPtr<Painting::DisplayListPlayerSkia> m_skia_player;
    RefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;

public:
    bool finish_rasterizing(ContextState& context, i32 bitmap_id)
    {
        Sync::MutexLocker const locker { m_mutex };
        context.is_rasterizing = false;
        if (!m_ui_presentation_client) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rasterized bitmap {} without a UI presentation client",
                bitmap_id);
            return false;
        }
        VERIFY(!context.presented_bitmap_id_awaiting_ack.has_value());
        context.presented_bitmap_id_awaiting_ack = bitmap_id;
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Rasterized bitmap {} waiting for ready_to_paint",
            bitmap_id);
        return true;
    }

    void mark_presented_bitmap_ready_to_paint(u64 page_id, i32 bitmap_id)
    {
        auto context_state = this->context_state(compositor_context_id_for_page(page_id));
        if (!context_state)
            return;
        auto& context = *context_state;
        Sync::MutexLocker const locker { m_mutex };
        if (!context.presents_to_client)
            return;
        if (context.presented_bitmap_id_awaiting_ack != bitmap_id) {
            dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Ignoring stale ready_to_paint for bitmap {} while awaiting bitmap {}",
                bitmap_id, context.presented_bitmap_id_awaiting_ack.value_or(-1));
            return;
        }

        context.presented_bitmap_id_awaiting_ack.clear();
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Compositor ready_to_paint released bitmap {}",
            bitmap_id);
        m_command_ready.signal();
    }
};

CompositorThread::CompositorThread(NonnullRefPtr<CompositorMainThreadClient> main_thread_client)
    : m_thread_data(adopt_ref(*new ThreadData(move(main_thread_client))))
{
}

CompositorThread::~CompositorThread()
{
    m_thread_data->exit();
}

CompositorContextId CompositorThread::create_context(Optional<u64> page_id, PagePresentationRegistration page_presentation_registration)
{
    CompositorContextId context_id;
    if (page_presentation_registration == PagePresentationRegistration::Yes) {
        VERIFY(page_id.has_value());
        context_id = compositor_context_id_for_page(*page_id);
    } else {
        VERIFY(!page_id.has_value());
        context_id = allocate_compositor_context_id();
        VERIFY(!is_page_presenting_context_id(context_id));
    }
    m_thread_data->register_context(context_id, page_id, page_presentation_registration);
    return context_id;
}

void CompositorThread::destroy_context(CompositorContextId context_id)
{
    m_thread_data->unregister_context(context_id);
}

void CompositorThread::set_ui_presentation_client(NonnullRefPtr<CompositorUIPresentationClient> ui_presentation_client)
{
    m_thread_data->set_ui_presentation_client(move(ui_presentation_client));
}

void CompositorThread::clear_ui_presentation_client()
{
    m_thread_data->clear_ui_presentation_client();
}

void CompositorThread::presented_bitmap_ready_to_paint(u64 page_id, i32 bitmap_id)
{
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Received compositor ready_to_paint for page {} bitmap {}",
        page_id, bitmap_id);
    m_thread_data->mark_presented_bitmap_ready_to_paint(page_id, bitmap_id);
}

bool CompositorThread::async_scroll_by(u64 page_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_device_pixels)
{
    return m_thread_data->enqueue_async_scroll_by(page_id, position, delta_in_device_pixels);
}

bool CompositorThread::handle_mouse_event(u64 page_id, MouseEvent const& event)
{
    return m_thread_data->handle_viewport_scrollbar_mouse_event(page_id, event);
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

void CompositorThread::set_presentation_mode(CompositorContextId context_id, PresentationMode mode)
{
    m_thread_data->set_presentation_mode(context_id, move(mode));
}

void CompositorThread::stop_presenting_to_client(CompositorContextId context_id)
{
    m_thread_data->stop_presenting_to_client(context_id);
}

void CompositorThread::update_display_list(
    CompositorContextId context_id,
    NonnullRefPtr<Painting::DisplayList> display_list,
    Painting::DisplayListResourceTransaction&& resource_transaction,
    Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    m_thread_data->enqueue_command(context_id, UpdateDisplayListCommand { move(display_list), move(resource_transaction), move(scroll_state_snapshot) });
}

void CompositorThread::update_video_frame(CompositorContextId context_id, Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame)
{
    m_thread_data->enqueue_command(context_id, UpdateVideoFrameCommand { frame_id, move(frame) });
}

void CompositorThread::clear_video_frame(CompositorContextId context_id, Painting::VideoFrameResourceId frame_id)
{
    m_thread_data->enqueue_command(context_id, ClearVideoFrameCommand { frame_id });
}

void CompositorThread::update_compositor_surface(CompositorContextId context_id, Painting::CompositorSurfaceId surface_id, Gfx::SharedImage&& shared_image)
{
    m_thread_data->enqueue_command(context_id, UpdateCompositorSurfaceCommand { surface_id, move(shared_image) });
}

void CompositorThread::clear_compositor_surface(CompositorContextId context_id, Painting::CompositorSurfaceId surface_id)
{
    m_thread_data->enqueue_command(context_id, ClearCompositorSurfaceCommand { surface_id });
}

void CompositorThread::invalidate_wheel_event_listener_state(CompositorContextId context_id, u64 generation)
{
    m_thread_data->invalidate_wheel_event_listener_state(context_id, generation);
}

AsyncScrollEnqueueResult CompositorThread::async_scroll_by(CompositorContextId context_id, UniqueNodeID expected_document_id, Gfx::FloatPoint position,
    Gfx::FloatPoint delta_in_device_pixels, Gfx::IntRect viewport_rect, AsyncScrollOperationTracking operation_tracking)
{
    return m_thread_data->enqueue_async_scroll_by(context_id, expected_document_id, position, delta_in_device_pixels, viewport_rect, operation_tracking);
}

bool CompositorThread::should_defer_async_scroll_offset_adoption(CompositorContextId context_id) const
{
    return m_thread_data->should_defer_async_scroll_offset_adoption(context_id);
}

bool CompositorThread::should_defer_main_thread_present_for_async_scroll(CompositorContextId context_id) const
{
    return m_thread_data->should_defer_main_thread_present_for_async_scroll(context_id);
}

PendingAsyncScrollUpdates CompositorThread::take_pending_async_scroll_updates(CompositorContextId context_id)
{
    return m_thread_data->take_pending_async_scroll_updates(context_id);
}

void CompositorThread::update_scroll_state(CompositorContextId context_id, Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    m_thread_data->enqueue_command(context_id, UpdateScrollStateCommand { move(scroll_state_snapshot) });
}

void CompositorThread::viewport_size_updated(
    CompositorContextId context_id,
    Gfx::IntSize viewport_size, bool is_top_level_traversable, WindowResizingInProgress window_resize_in_progress)
{
    m_thread_data->enqueue_command(context_id,
        ViewportSizeUpdatedCommand { viewport_size, is_top_level_traversable, window_resize_in_progress });
}

void CompositorThread::present_frame(CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    m_thread_data->enqueue_present_frame(context_id, viewport_rect);
}

void CompositorThread::request_screenshot(CompositorContextId context_id, NonnullRefPtr<Gfx::PaintingSurface> target_surface, ScreenshotRequestId request_id)
{
    m_thread_data->enqueue_command(context_id, ScreenshotCommand { move(target_surface), request_id });
}

}
