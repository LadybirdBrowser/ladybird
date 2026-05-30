/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <Compositor/CompositorState.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Color.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/Page/InputEvent.h>

namespace Compositor {

static constexpr int gpu_completion_check_interval_ms = 1;

static bool presentation_mode_presents_to_client(Web::Compositor::PresentationMode const& presentation_mode)
{
    return presentation_mode.has<Web::Compositor::PresentToClient>();
}

static void set_or_append_pending_scroll_offset(Vector<Web::Compositor::AsyncScrollOffset>& pending_scroll_offsets, Web::Compositor::AsyncScrollOffset const& scroll_offset)
{
    for (auto& existing : pending_scroll_offsets) {
        if (existing.stable_node_id == scroll_offset.stable_node_id) {
            existing.compositor_scroll_offset = scroll_offset.compositor_scroll_offset;
            existing.unadopted_scroll_delta.translate_by(scroll_offset.unadopted_scroll_delta);
            return;
        }
    }
    pending_scroll_offsets.append(scroll_offset);
}

NonnullRefPtr<CompositorState> CompositorState::create(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, bool async_scrolling_enabled)
{
    return adopt_ref(*new CompositorState(move(skia_backend_context), async_scrolling_enabled));
}

CompositorState::CompositorState(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, bool async_scrolling_enabled)
    : m_skia_backend_context(move(skia_backend_context))
    , m_display_list_player(make<Web::Painting::DisplayListPlayerSkia>(m_skia_backend_context))
    , m_async_scrolling_enabled(async_scrolling_enabled)
{
}

CompositorState::~CompositorState()
{
    if (!m_gpu_completion_timer)
        return;
    m_gpu_completion_timer->on_timeout = {};
    m_gpu_completion_timer->stop();
}

CompositorState::ContextState::~ContextState()
{
    stop_backing_store_shrink_timer();
}

void CompositorState::ContextState::stop_backing_store_shrink_timer()
{
    if (!backing_store_shrink_timer)
        return;
    backing_store_shrink_timer->on_timeout = {};
    backing_store_shrink_timer->stop();
}

void CompositorState::set_client(CompositorStateClient& client)
{
    m_client = &client;
}

CompositorState::ContextOwnerCheckResult CompositorState::check_context_owner(Web::Compositor::CompositorContextId context_id, CompositorStateWebContentClient& client)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return ContextOwnerCheckResult::ContextUnavailable;
    VERIFY(context->web_content_client);
    if (context->web_content_client != &client)
        return ContextOwnerCheckResult::ConflictingOwner;

    return ContextOwnerCheckResult::OwnedByClient;
}

void CompositorState::destroy_contexts_for_web_content_client(CompositorStateWebContentClient& client)
{
    Vector<Web::Compositor::CompositorContextId> context_ids;
    for (auto& context : m_contexts) {
        if (context.value->web_content_client == &client)
            context_ids.append(context.key);
    }

    for (auto context_id : context_ids) {
        destroy_context(context_id);
    }
}

void CompositorState::create_context(Web::Compositor::CompositorContextId context_id, Optional<u64> page_id, CompositorStateWebContentClient& web_content_client)
{
    VERIFY(!m_contexts.contains(context_id));
    if (page_id.has_value())
        VERIFY(context_id == Web::Compositor::compositor_context_id_for_page(*page_id));

    auto& context = *m_contexts.ensure(context_id, [] {
        return make<ContextState>();
    });
    context.web_content_client = &web_content_client;
    context.page_id = page_id;
    if (page_id.has_value())
        context.presentation_mode = Web::Compositor::PresentToClient {};
    resize_backing_stores_if_needed(context_id, context);
}

void CompositorState::destroy_context(Web::Compositor::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    cancel_pending_async_presents_for_context(context_id);
    detach_from_parent_surface(context_id, *context);
    for (auto& child_context_entry : context->child_contexts_by_surface_id) {
        auto* child_context = context_if_present(child_context_entry.value);
        VERIFY(child_context);
        VERIFY(child_context->published_surface.has_value());
        VERIFY(child_context->published_surface->parent_context_id == context_id);
        child_context->published_surface.clear();
        child_context->presentation_mode = Empty {};
    }
    m_contexts.remove(context_id);
}

void CompositorState::set_presentation_mode(Web::Compositor::CompositorContextId context_id, Web::Compositor::PresentationMode presentation_mode)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    auto& context_state = *context;
    auto was_presenting_to_client = presentation_mode_presents_to_client(context_state.presentation_mode);
    auto will_present_to_client = presentation_mode_presents_to_client(presentation_mode);
    detach_from_parent_surface(context_id, context_state);

    presentation_mode.visit(
        [](Empty const&) {},
        [&](Web::Compositor::PresentToClient const&) {
            VERIFY(context_state.page_id.has_value());
        },
        [&](Web::Compositor::PublishToCompositorSurface const& mode) {
            auto* parent_context = context_if_present(mode.target_context_id);
            VERIFY(parent_context);
            parent_context->child_contexts_by_surface_id.set(mode.surface_id, context_id);
            context_state.published_surface = ContextState::PublishedSurface {
                .parent_context_id = mode.target_context_id,
                .surface_id = mode.surface_id,
            };
        });
    context_state.presentation_mode = move(presentation_mode);

    if (was_presenting_to_client && !will_present_to_client
        && context_state.gpu_present_bitmap_id_awaiting_completion.has_value()
        && context_state.presented_bitmap_id_awaiting_ack == context_state.gpu_present_bitmap_id_awaiting_completion)
        context_state.presented_bitmap_id_awaiting_ack.clear();
}

void CompositorState::update_display_list(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Web::Painting::DisplayList> display_list, Web::Painting::AccumulatedVisualContextTree visual_context_tree, Web::Painting::DisplayListResourceTransaction&& resource_transaction, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    context->display_list_resource_storage.apply_transaction(move(resource_transaction));
    install_display_list_update(*context, move(display_list), move(visual_context_tree), move(scroll_state_snapshot));
}

void CompositorState::update_scroll_state(Web::Compositor::CompositorContextId context_id, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    context->scroll_state_snapshot = move(scroll_state_snapshot);
    if (!context->has_async_scrolling_state)
        return;

    auto reconciled_viewport_scroll_offset = reapply_pending_async_scroll_offsets(*context, context->pending_async_scroll_offsets);
    context->async_scroll_tree.rebuild_wheel_hit_test_targets(context->display_list, context->visual_context_tree.has_value() ? &context->visual_context_tree.value() : nullptr, context->scroll_state_snapshot);
    if (reconciled_viewport_scroll_offset.has_value()) {
        auto reconciled_viewport_rect = context->async_scrolling_viewport_rect;
        reconciled_viewport_rect.set_location(reconciled_viewport_scroll_offset->to_type<int>());
        context->async_scrolling_viewport_rect = reconciled_viewport_rect;
    }
}

void CompositorState::update_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->display_list_resource_storage.update_video_frame(frame_id, move(frame));
    present_current_frame(context_id, *context);
}

void CompositorState::clear_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->display_list_resource_storage.clear_video_frame(frame_id);
    present_current_frame(context_id, *context);
}

void CompositorState::update_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id, Gfx::SharedImage&& shared_image)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->display_list_resource_storage.update_compositor_surface(surface_id, move(shared_image));
    present_current_frame(context_id, *context);
}

void CompositorState::clear_compositor_surface(Web::Compositor::CompositorContextId context_id, Web::Painting::CompositorSurfaceId surface_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->display_list_resource_storage.clear_compositor_surface(surface_id);
    remove_child_surface(*context, context_id, surface_id);
    present_current_frame(context_id, *context);
}

void CompositorState::invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId context_id, u64 generation)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->wheel_event_listener_state_generation = max(context->wheel_event_listener_state_generation, generation);
    context->wheel_routing_admission = Web::Compositor::WheelRoutingAdmission::StaleWheelEventListeners;
    context->can_accept_async_wheel_events = false;
}

bool CompositorState::handle_mouse_event(Web::Compositor::CompositorContextId context_id, Web::MouseEvent const& event)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return false;

    auto& context_state = *context;
    if (!presentation_mode_presents_to_client(context_state.presentation_mode))
        return false;

    auto position = Gfx::FloatPoint {
        static_cast<float>(event.position.x().value()),
        static_cast<float>(event.position.y().value()),
    };

    switch (event.type) {
    case Web::MouseEvent::Type::MouseDown: {
        if (event.button != Web::UIEvents::MouseButton::Primary)
            return false;

        auto drag = context_state.viewport_scrollbar_controller.begin_drag(context_state.async_scroll_tree, context_state.scroll_state_snapshot, position);
        if (!drag.has_value())
            return false;

        if (!context_state.async_scrolling_viewport_rect.is_empty())
            schedule_present_frame(context_id, context_state, context_state.async_scrolling_viewport_rect);
        apply_viewport_scrollbar_drag(context_id, context_state, *drag);
        return true;
    }
    case Web::MouseEvent::Type::MouseMove: {
        auto had_capture = context_state.viewport_scrollbar_controller.has_captured_scrollbar();
        if (had_capture) {
            auto drag = context_state.viewport_scrollbar_controller.captured_drag(position);
            if (!drag.has_value())
                return false;
            apply_viewport_scrollbar_drag(context_id, context_state, *drag);
            return true;
        }

        auto hovered_scrollbar_index = context_state.viewport_scrollbar_controller.hit_test(context_state.async_scroll_tree, context_state.scroll_state_snapshot, position);
        if (context_state.viewport_scrollbar_controller.set_hovered_scrollbar(hovered_scrollbar_index) && !context_state.async_scrolling_viewport_rect.is_empty())
            schedule_present_frame(context_id, context_state, context_state.async_scrolling_viewport_rect);
        return hovered_scrollbar_index.has_value();
    }
    case Web::MouseEvent::Type::MouseUp: {
        auto drag = context_state.viewport_scrollbar_controller.release_captured_drag(position);
        if (!drag.has_value())
            return false;

        if (!context_state.async_scrolling_viewport_rect.is_empty())
            schedule_present_frame(context_id, context_state, context_state.async_scrolling_viewport_rect);
        apply_viewport_scrollbar_drag(context_id, context_state, *drag);
        return true;
    }
    case Web::MouseEvent::Type::MouseLeave: {
        auto had_capture = context_state.viewport_scrollbar_controller.has_captured_scrollbar();
        if (context_state.viewport_scrollbar_controller.set_hovered_scrollbar({}) && !context_state.async_scrolling_viewport_rect.is_empty())
            schedule_present_frame(context_id, context_state, context_state.async_scrolling_viewport_rect);
        return had_capture;
    }
    case Web::MouseEvent::Type::MouseWheel:
        return false;
    }

    VERIFY_NOT_REACHED();
}

bool CompositorState::dispatch_mouse_event_to_web_content(Web::Compositor::CompositorContextId context_id, Web::MouseEvent const& event)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return false;

    VERIFY(context->web_content_client);

    auto page_id = context->page_id;
    VERIFY(page_id.has_value());

    context->web_content_client->dispatch_mouse_event_to_web_content(*page_id, event);
    return true;
}

Web::Compositor::AsyncScrollEnqueueResult CompositorState::async_scroll_by(Web::Compositor::CompositorContextId context_id, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking operation_tracking)
{
    if (!m_async_scrolling_enabled)
        return {};

    auto* context = context_if_present(context_id);
    VERIFY(context);

    auto& context_state = *context;
    if (!context_state.can_accept_async_wheel_events)
        return {};

    auto scroll_target = context_state.async_scroll_tree.hit_test_scroll_node_for_wheel(position, delta);
    if (scroll_target.blocked_by_main_thread_region || scroll_target.blocked_by_wheel_event_region || !scroll_target.node_id.has_value())
        return {};
    if (scroll_target.node_id->document_id != expected_document_id)
        return {};

    Optional<Web::Compositor::AsyncScrollOperationID> operation_id;
    if (operation_tracking == Web::Compositor::AsyncScrollOperationTracking::Yes)
        operation_id = ++context_state.next_async_scroll_operation_id;

    auto async_scroll_viewport_rect = viewport_rect;
    auto scroll_offsets = context_state.async_scroll_tree.apply_scroll_delta(*scroll_target.node_id, delta, context_state.scroll_state_snapshot);
    if (scroll_offsets.is_empty()) {
        if (operation_id.has_value())
            context_state.completed_async_scroll_operation_ids.append(*operation_id);
        return { true, operation_id };
    }

    context_state.async_scroll_tree.rebuild_wheel_hit_test_targets(context_state.display_list, context_state.visual_context_tree.has_value() ? &context_state.visual_context_tree.value() : nullptr, context_state.scroll_state_snapshot);
    if (auto viewport_scroll_offset = viewport_scroll_offset_from(context_state, scroll_offsets); viewport_scroll_offset.has_value())
        async_scroll_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
    store_pending_async_scroll_offsets(context_state, scroll_offsets, operation_id);
    context_state.async_scrolling_viewport_rect = async_scroll_viewport_rect;
    schedule_present_frame(context_id, context_state, async_scroll_viewport_rect);
    return { true, operation_id };
}

bool CompositorState::async_scroll_by(Web::Compositor::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta)
{
    if (!m_async_scrolling_enabled)
        return false;

    auto* context = context_if_present(context_id);
    if (!context)
        return false;

    auto& context_state = *context;
    if (!presentation_mode_presents_to_client(context_state.presentation_mode))
        return false;
    VERIFY(context_state.web_content_client);
    if (!context_state.can_accept_async_wheel_events)
        return false;

    auto scroll_target = context_state.async_scroll_tree.hit_test_scroll_node_for_wheel(position, delta);
    if (scroll_target.blocked_by_main_thread_region || scroll_target.blocked_by_wheel_event_region || !scroll_target.node_id.has_value())
        return false;

    auto async_scroll_viewport_rect = context_state.async_scrolling_viewport_rect;
    auto scroll_offsets = context_state.async_scroll_tree.apply_scroll_delta(*scroll_target.node_id, delta, context_state.scroll_state_snapshot);
    if (scroll_offsets.is_empty())
        return true;

    context_state.async_scroll_tree.rebuild_wheel_hit_test_targets(context_state.display_list, context_state.visual_context_tree.has_value() ? &context_state.visual_context_tree.value() : nullptr, context_state.scroll_state_snapshot);
    if (auto viewport_scroll_offset = viewport_scroll_offset_from(context_state, scroll_offsets); viewport_scroll_offset.has_value())
        async_scroll_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
    store_pending_async_scroll_offsets(context_state, scroll_offsets);
    context_state.async_scrolling_viewport_rect = async_scroll_viewport_rect;
    schedule_present_frame(context_id, context_state, async_scroll_viewport_rect);
    context_state.web_content_client->request_rendering_update();
    return true;
}

bool CompositorState::should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId context_id) const
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    if (context->pending_async_scroll_offsets.is_empty())
        return false;

    return context->pending_present_frame.has_value()
        || context->gpu_present_bitmap_id_awaiting_completion.has_value()
        || context->presented_bitmap_id_awaiting_ack.has_value();
}

Web::Compositor::PendingAsyncScrollUpdates CompositorState::take_pending_async_scroll_updates(Web::Compositor::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    Web::Compositor::PendingAsyncScrollUpdates updates;
    AK::swap(updates.scroll_offsets, context->pending_async_scroll_offsets);
    AK::swap(updates.completed_operation_ids, context->completed_async_scroll_operation_ids);
    return updates;
}

void CompositorState::viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress window_resize_in_progress)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    context->viewport_size = viewport_size;
    auto is_page_presentation_context = context->page_id.has_value();
    context->window_resize_in_progress = is_page_presentation_context
        ? window_resize_in_progress
        : Web::Compositor::WindowResizingInProgress::No;
    resize_backing_stores_if_needed(context_id, *context);
    if (context->window_resize_in_progress == Web::Compositor::WindowResizingInProgress::Yes)
        schedule_backing_store_shrink(context_id, *context);
}

void CompositorState::set_display_metadata(Web::Compositor::CompositorContextId context_id, Optional<u64> display_id, double refresh_rate)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    VERIFY(refresh_rate == refresh_rate);
    VERIFY(refresh_rate > 0);
    VERIFY(refresh_rate < AK::Infinity<double>);

    context->display_id = display_id;
    context->display_refresh_rate = refresh_rate;
    if (context->pending_present_frame_scheduled)
        schedule_pending_present_frame_on_vsync(context_id, *context);
}

void CompositorState::present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    schedule_present_frame(context_id, *context, viewport_rect);
}

void CompositorState::present_frame(Web::Compositor::CompositorContextId context_id, ContextState& context, Gfx::IntRect viewport_rect)
{
    if (context.gpu_present_bitmap_id_awaiting_completion.has_value() || context.presented_bitmap_id_awaiting_ack.has_value()) {
        context.pending_present_frame = viewport_rect;
        return;
    }

    if (!context.display_list || !context.visual_context_tree.has_value() || !context.backing_store_manager.is_valid()) {
        context.presented_frame = viewport_rect;
        return;
    }

    auto& back_store = context.backing_store_manager.back_store();
    context.presentation_mode.visit(
        [](Empty const&) {},
        [](Web::Compositor::PresentToClient const&) {},
        [&](Web::Compositor::PublishToCompositorSurface const&) {
            Gfx::PainterSkia painter { NonnullRefPtr<Gfx::PaintingSurface> { back_store } };
            painter.clear_rect(back_store.rect().to_type<float>(), Gfx::Color::Transparent);
        });
    m_display_list_player->execute(*context.display_list, context.visual_context_tree.value(), context.display_list_resource_storage, context.scroll_state_snapshot, back_store);
    context.viewport_scrollbar_controller.paint(back_store, *m_display_list_player, context.scroll_state_snapshot);
    auto rendered_bitmap_id = context.backing_store_manager.back_bitmap_id();
    context.gpu_present_bitmap_id_awaiting_completion = rendered_bitmap_id;
    if (presentation_mode_presents_to_client(context.presentation_mode))
        context.presented_bitmap_id_awaiting_ack = rendered_bitmap_id;
    m_pending_async_presents.append(context_id, viewport_rect, rendered_bitmap_id);
    auto* pending_present = &m_pending_async_presents.last();

    auto event_loop_reference = Core::EventLoop::current_weak();
    auto self = NonnullRefPtr { *this };
    m_display_list_player->flush_async(back_store, [self = move(self), event_loop_reference = move(event_loop_reference), pending_present] {
        auto event_loop = event_loop_reference->take();
        if (!event_loop.is_alive())
            return;
        event_loop->deferred_invoke([self = move(self), pending_present] {
            self->did_finish_async_present(*pending_present);
        });
    });
    context.backing_store_manager.swap();
    context.presented_frame = viewport_rect;
    schedule_gpu_completion_check();
}

void CompositorState::schedule_present_frame(Web::Compositor::CompositorContextId context_id, ContextState& context, Gfx::IntRect viewport_rect)
{
    context.pending_present_frame = viewport_rect;
    schedule_pending_present_frame_on_vsync(context_id, context);
}

void CompositorState::schedule_pending_present_frame_on_vsync(Web::Compositor::CompositorContextId, ContextState& context)
{
    context.pending_present_frame_scheduled = true;
    vsync_scheduler_for_display(context.display_id).schedule(context.display_refresh_rate);
}

void CompositorState::schedule_pending_present_frame_if_unblocked(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    if (context.gpu_present_bitmap_id_awaiting_completion.has_value() || context.presented_bitmap_id_awaiting_ack.has_value())
        return;
    if (!context.pending_present_frame.has_value())
        return;
    if (context.pending_present_frame_scheduled)
        return;

    schedule_pending_present_frame_on_vsync(context_id, context);
}

VSyncScheduler& CompositorState::vsync_scheduler_for_display(Optional<u64> display_id)
{
    return *m_vsync_schedulers_by_display.ensure(display_id, [this, display_id] {
        return create_vsync_scheduler(display_id, [this, display_id] {
            present_pending_frames_on_vsync(display_id);
        });
    });
}

void CompositorState::present_pending_frames_on_vsync(Optional<u64> display_id)
{
    for (auto& context_entry : m_contexts) {
        auto context_id = context_entry.key;
        auto& context = *context_entry.value;
        if (!context.pending_present_frame_scheduled)
            continue;
        if (context.display_id != display_id)
            continue;

        context.pending_present_frame_scheduled = false;
        if (context.gpu_present_bitmap_id_awaiting_completion.has_value() || context.presented_bitmap_id_awaiting_ack.has_value())
            continue;
        if (!context.pending_present_frame.has_value())
            continue;

        auto pending_present_frame = context.pending_present_frame.release_value();
        present_frame(context_id, context, pending_present_frame);
    }
}

void CompositorState::flush_descendant_surfaces_for_screenshot(Web::Compositor::CompositorContextId context_id)
{
    // Presents are scheduled on a vsync timer rather than performed synchronously — so a nested navigable's latest
    // paint may not yet be published to its parent's compositor surface. A screenshot re-executes the top-level display
    // list, whose embedded-content commands read those child surfaces. So, flush any descendant with a deferred
    // present synchronously (deepest-first) — to capture a complete frame instead of a stale/blank iframe.
    auto* context = context_if_present(context_id);
    if (!context)
        return;
    for (auto& child : context->child_contexts_by_surface_id)
        present_subtree_for_screenshot(child.value);
}

bool CompositorState::present_subtree_for_screenshot(Web::Compositor::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return false;

    bool needs_present = context->pending_present_frame.has_value() || context->pending_present_frame_scheduled;
    for (auto& child : context->child_contexts_by_surface_id) {
        if (present_subtree_for_screenshot(child.value))
            needs_present = true;
    }

    if (!needs_present || !context->presentation_mode.has<Web::Compositor::PublishToCompositorSurface>())
        return false;

    present_context_synchronously(*context);
    return true;
}

void CompositorState::present_context_synchronously(ContextState& context)
{
    auto* publish_mode = context.presentation_mode.get_pointer<Web::Compositor::PublishToCompositorSurface>();
    if (!publish_mode)
        return;
    if (!context.display_list || !context.visual_context_tree.has_value() || !context.backing_store_manager.is_valid())
        return;
    // Don't race an async present already in flight for this context; its own completion will publish.
    if (context.gpu_present_bitmap_id_awaiting_completion.has_value() || context.presented_bitmap_id_awaiting_ack.has_value())
        return;

    auto viewport_rect = context.pending_present_frame;
    if (!viewport_rect.has_value())
        viewport_rect = context.presented_frame;
    if (!viewport_rect.has_value())
        return;

    auto& back_store = context.backing_store_manager.back_store();
    {
        Gfx::PainterSkia painter { NonnullRefPtr<Gfx::PaintingSurface> { back_store } };
        painter.clear_rect(back_store.rect().to_type<float>(), Gfx::Color::Transparent);
    }
    m_display_list_player->execute(*context.display_list, context.visual_context_tree.value(), context.display_list_resource_storage, context.scroll_state_snapshot, back_store);
    context.viewport_scrollbar_controller.paint(back_store, *m_display_list_player, context.scroll_state_snapshot);
    m_display_list_player->flush(back_store);
    context.backing_store_manager.swap();
    context.presented_frame = viewport_rect;
    context.pending_present_frame.clear();
    context.pending_present_frame_scheduled = false;
    publish_to_parent_surface(context, *publish_mode);
}

bool CompositorState::request_screenshot(Web::Compositor::CompositorContextId context_id, Gfx::ShareableBitmap& target_bitmap)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    if (!context->display_list || !context->visual_context_tree.has_value() || !target_bitmap.is_valid() || !target_bitmap.bitmap())
        return false;

    flush_descendant_surfaces_for_screenshot(context_id);

    auto target_surface = Gfx::PaintingSurface::wrap_bitmap(*target_bitmap.bitmap());
    m_display_list_player->execute(*context->display_list, context->visual_context_tree.value(), context->display_list_resource_storage, context->scroll_state_snapshot, *target_surface);
    context->viewport_scrollbar_controller.paint(*target_surface, *m_display_list_player, context->scroll_state_snapshot);
    m_display_list_player->flush(*target_surface);
    return true;
}

void CompositorState::presented_bitmap_ready_to_paint(Web::Compositor::CompositorContextId context_id, i32 bitmap_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    if (context->presented_bitmap_id_awaiting_ack != bitmap_id)
        return;

    context->presented_bitmap_id_awaiting_ack.clear();
    schedule_pending_present_frame_if_unblocked(context_id, *context);
}

void CompositorState::did_finish_async_present(PendingAsyncPresent& pending_present)
{
    auto pending_present_iterator = m_pending_async_presents.begin();
    for (; pending_present_iterator != m_pending_async_presents.end(); ++pending_present_iterator) {
        if (&*pending_present_iterator == &pending_present)
            break;
    }
    VERIFY(pending_present_iterator != m_pending_async_presents.end());

    auto context_id = pending_present.context_id;
    auto viewport_rect = pending_present.viewport_rect;
    auto bitmap_id = pending_present.bitmap_id;
    auto was_cancelled = pending_present.was_cancelled;
    (void)m_pending_async_presents.remove(pending_present_iterator);
    if (m_pending_async_presents.is_empty() && m_gpu_completion_timer)
        m_gpu_completion_timer->stop();

    if (was_cancelled)
        return;

    auto* context = context_if_present(context_id);
    VERIFY(context);
    VERIFY(context->gpu_present_bitmap_id_awaiting_completion == bitmap_id);

    context->gpu_present_bitmap_id_awaiting_completion.clear();
    context->presentation_mode.visit(
        [](Empty const&) {},
        [&](Web::Compositor::PresentToClient const&) {
            VERIFY(m_client);
            VERIFY(context->presented_bitmap_id_awaiting_ack == bitmap_id);
            m_client->did_present_frame(context_id, viewport_rect, bitmap_id);
        },
        [&](Web::Compositor::PublishToCompositorSurface const& mode) {
            publish_to_parent_surface(*context, mode);
        });

    schedule_pending_present_frame_if_unblocked(context_id, *context);
}

void CompositorState::cancel_pending_async_presents_for_context(Web::Compositor::CompositorContextId context_id)
{
    for (auto& pending_present : m_pending_async_presents) {
        if (pending_present.context_id == context_id)
            pending_present.was_cancelled = true;
    }
}

void CompositorState::schedule_gpu_completion_check()
{
    if (!m_skia_backend_context || m_pending_async_presents.is_empty())
        return;

    if (!m_gpu_completion_timer) {
        m_gpu_completion_timer = Core::Timer::create_repeating(gpu_completion_check_interval_ms, [this] {
            check_gpu_completions();
        });
    }
    if (!m_gpu_completion_timer->is_active())
        m_gpu_completion_timer->start();
}

void CompositorState::check_gpu_completions()
{
    if (m_pending_async_presents.is_empty()) {
        if (m_gpu_completion_timer)
            m_gpu_completion_timer->stop();
        return;
    }

    if (m_skia_backend_context)
        m_skia_backend_context->check_async_work_completion();

    if (m_pending_async_presents.is_empty() && m_gpu_completion_timer)
        m_gpu_completion_timer->stop();
}

CompositorState::ContextState* CompositorState::context_if_present(Web::Compositor::CompositorContextId context_id)
{
    auto it = m_contexts.find(context_id);
    if (it == m_contexts.end())
        return nullptr;
    return it->value.ptr();
}

CompositorState::ContextState const* CompositorState::context_if_present(Web::Compositor::CompositorContextId context_id) const
{
    auto it = m_contexts.find(context_id);
    if (it == m_contexts.end())
        return nullptr;
    return it->value.ptr();
}

void CompositorState::detach_from_parent_surface(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    if (!context.published_surface.has_value())
        return;

    auto published_surface = context.published_surface.release_value();
    auto* parent_context = context_if_present(published_surface.parent_context_id);
    VERIFY(parent_context);
    auto child_context_id = parent_context->child_contexts_by_surface_id.get(published_surface.surface_id);
    VERIFY(child_context_id.has_value());
    VERIFY(*child_context_id == context_id);
    parent_context->child_contexts_by_surface_id.remove(published_surface.surface_id);
    parent_context->display_list_resource_storage.clear_compositor_surface(published_surface.surface_id);
    present_current_frame(published_surface.parent_context_id, *parent_context);
}

void CompositorState::remove_child_surface(ContextState& context, Web::Compositor::CompositorContextId parent_context_id, Web::Painting::CompositorSurfaceId surface_id)
{
    auto child_context_id = context.child_contexts_by_surface_id.take(surface_id);
    if (!child_context_id.has_value())
        return;

    auto* child_context = context_if_present(*child_context_id);
    VERIFY(child_context);
    VERIFY(child_context->published_surface.has_value());
    VERIFY(child_context->published_surface->parent_context_id == parent_context_id);
    VERIFY(child_context->published_surface->surface_id == surface_id);
    child_context->published_surface.clear();
    child_context->presentation_mode = Empty {};
}

void CompositorState::install_display_list_update(ContextState& context, NonnullRefPtr<Web::Painting::DisplayList> display_list, Web::Painting::AccumulatedVisualContextTree visual_context_tree, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    VERIFY(display_list->compatible_visual_context_tree_version() == visual_context_tree.version());
    context.display_list = move(display_list);
    context.visual_context_tree = move(visual_context_tree);
    context.scroll_state_snapshot = move(scroll_state_snapshot);

    if (!m_async_scrolling_enabled) {
        context.async_scroll_tree.set_state({});
        context.viewport_scrollbar_controller.clear();
        context.pending_async_scroll_offsets.clear();
        context.completed_async_scroll_operation_ids.clear();
        context.wheel_routing_admission = Web::Compositor::WheelRoutingAdmission::NoAsyncScrollingState;
        context.can_accept_async_wheel_events = false;
        context.async_scrolling_viewport_rect = {};
        context.has_async_scrolling_state = false;
        return;
    }

    auto async_scrolling_state = Web::Compositor::async_scrolling_state_from_display_list(*context.display_list);
    auto async_scrolling_viewport_rect = async_scrolling_state.viewport_rect;
    auto wheel_event_listener_state_generation = async_scrolling_state.wheel_event_listener_state_generation;
    auto wheel_routing_admission = Web::Compositor::wheel_routing_admission_for(async_scrolling_state);
    if (wheel_event_listener_state_generation < context.wheel_event_listener_state_generation)
        wheel_routing_admission = Web::Compositor::WheelRoutingAdmission::StaleWheelEventListeners;
    else
        context.wheel_event_listener_state_generation = wheel_event_listener_state_generation;

    context.wheel_routing_admission = wheel_routing_admission;
    context.can_accept_async_wheel_events = wheel_routing_admission == Web::Compositor::WheelRoutingAdmission::Accepted;

    context.viewport_scrollbar_controller.set_scrollbars(async_scrolling_state.viewport_scrollbars);
    context.async_scroll_tree.set_state(move(async_scrolling_state));
    if (!context.pending_async_scroll_offsets.is_empty()) {
        if (auto viewport_scroll_offset = reapply_pending_async_scroll_offsets(context, context.pending_async_scroll_offsets); viewport_scroll_offset.has_value())
            async_scrolling_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
    }
    context.async_scroll_tree.rebuild_wheel_hit_test_targets(context.display_list, context.visual_context_tree.has_value() ? &context.visual_context_tree.value() : nullptr, context.scroll_state_snapshot);
    context.async_scrolling_viewport_rect = async_scrolling_viewport_rect;
    context.has_async_scrolling_state = true;
}

Optional<Gfx::FloatPoint> CompositorState::viewport_scroll_offset_from(ContextState& context, Vector<Web::Compositor::AsyncScrollOffset> const& scroll_offsets) const
{
    Optional<Gfx::FloatPoint> viewport_scroll_offset;
    for (auto const& scroll_offset : scroll_offsets) {
        auto node_id = context.async_scroll_tree.scroll_node_id_for_stable_id(scroll_offset.stable_node_id);
        if (node_id.has_value() && context.async_scroll_tree.scroll_node_is_viewport(*node_id))
            viewport_scroll_offset = scroll_offset.compositor_scroll_offset;
    }
    return viewport_scroll_offset;
}

Optional<Gfx::FloatPoint> CompositorState::reapply_pending_async_scroll_offsets(ContextState& context, Vector<Web::Compositor::AsyncScrollOffset> const& pending_scroll_offsets)
{
    Optional<Gfx::FloatPoint> viewport_scroll_offset;
    for (auto const& pending_scroll_offset : pending_scroll_offsets) {
        auto node_id = context.async_scroll_tree.scroll_node_id_for_stable_id(pending_scroll_offset.stable_node_id);
        if (!node_id.has_value())
            continue;
        auto reconciled_scroll_offset = context.async_scroll_tree.set_scroll_offset(*node_id, pending_scroll_offset.compositor_scroll_offset, context.scroll_state_snapshot);
        if (reconciled_scroll_offset.has_value() && context.async_scroll_tree.scroll_node_is_viewport(*node_id))
            viewport_scroll_offset = *reconciled_scroll_offset;
    }
    return viewport_scroll_offset;
}

void CompositorState::store_pending_async_scroll_offsets(ContextState& context, Vector<Web::Compositor::AsyncScrollOffset> const& scroll_offsets, Optional<Web::Compositor::AsyncScrollOperationID> operation_id)
{
    for (auto const& scroll_offset : scroll_offsets)
        set_or_append_pending_scroll_offset(context.pending_async_scroll_offsets, scroll_offset);
    if (operation_id.has_value())
        context.completed_async_scroll_operation_ids.append(*operation_id);
}

bool CompositorState::apply_viewport_scrollbar_drag(Web::Compositor::CompositorContextId context_id, ContextState& context, ViewportScrollbarController::Drag const& drag)
{
    auto scroll_delta = context.viewport_scrollbar_controller.scroll_delta_for_drag(context.async_scroll_tree, context.scroll_state_snapshot, drag);
    if (!scroll_delta.has_value())
        return false;

    auto scroll_offsets = context.async_scroll_tree.apply_scroll_delta(scroll_delta->scroll_node_id, scroll_delta->delta, context.scroll_state_snapshot);
    if (scroll_offsets.is_empty())
        return false;
    context.async_scroll_tree.rebuild_wheel_hit_test_targets(context.display_list, context.visual_context_tree.has_value() ? &context.visual_context_tree.value() : nullptr, context.scroll_state_snapshot);

    auto viewport_scroll_offset = viewport_scroll_offset_from(context, scroll_offsets);
    if (!viewport_scroll_offset.has_value())
        return false;

    store_pending_async_scroll_offsets(context, scroll_offsets);
    auto async_scroll_viewport_rect = context.async_scrolling_viewport_rect;
    async_scroll_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
    context.async_scrolling_viewport_rect = async_scroll_viewport_rect;
    schedule_present_frame(context_id, context, async_scroll_viewport_rect);
    VERIFY(context.web_content_client);
    context.web_content_client->request_rendering_update();
    return true;
}

void CompositorState::resize_backing_stores_if_needed(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    auto allocation = context.backing_store_manager.resize_backing_stores_if_needed(context.viewport_size, context.window_resize_in_progress);
    if (!allocation.has_value())
        return;
    if (auto publication = context.backing_store_manager.allocate_backing_stores(
            *allocation, m_skia_backend_context, presentation_mode_presents_to_client(context.presentation_mode));
        publication.has_value()) {
        publish_backing_stores(context_id, context, publication.release_value());
        present_current_frame(context_id, context);
    }
}

void CompositorState::schedule_backing_store_shrink(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    if (!context.backing_store_shrink_timer) {
        context.backing_store_shrink_timer = Core::Timer::create_single_shot(3000, [this, context_id] {
            shrink_backing_stores_after_resize(context_id);
        });
    }
    context.backing_store_shrink_timer->restart();
}

void CompositorState::shrink_backing_stores_after_resize(Web::Compositor::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    context->window_resize_in_progress = Web::Compositor::WindowResizingInProgress::No;
    resize_backing_stores_if_needed(context_id, *context);
}

void CompositorState::present_current_frame(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    // A queued frame already captures the newest viewport rect and will pick up
    // the updated resource when the outstanding bitmap is acknowledged.
    if (context.pending_present_frame.has_value())
        return;
    if (!context.presented_frame.has_value())
        return;
    schedule_present_frame(context_id, context, *context.presented_frame);
}

void CompositorState::publish_to_parent_surface(ContextState& context, Web::Compositor::PublishToCompositorSurface const& mode)
{
    auto* parent_context = context_if_present(mode.target_context_id);
    VERIFY(parent_context);

    parent_context->display_list_resource_storage.update_compositor_surface(
        mode.surface_id,
        context.backing_store_manager.front_store().snapshot_into_shared_image());
    present_current_frame(mode.target_context_id, *parent_context);
}

void CompositorState::publish_backing_stores(Web::Compositor::CompositorContextId context_id, ContextState& context, BackingStoreManager::Publication&& publication)
{
    VERIFY(m_client);
    if (!presentation_mode_presents_to_client(context.presentation_mode))
        return;

    m_client->did_allocate_backing_stores(context_id, publication.front_bitmap_id, move(publication.front_shared_image), publication.back_bitmap_id, move(publication.back_shared_image));
}

}
