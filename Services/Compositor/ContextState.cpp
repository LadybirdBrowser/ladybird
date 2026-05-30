/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <Compositor/CompositorState.h>
#include <Compositor/ContextState.h>
#include <LibCore/Timer.h>
#include <LibGfx/Color.h>
#include <LibGfx/PainterSkia.h>
#include <LibGfx/PaintingSurface.h>
#include <LibWeb/Page/InputEvent.h>
#include <LibWeb/Painting/DisplayListPlayerSkia.h>

namespace Compositor {

static void set_or_append_pending_scroll_offset(
    Vector<Web::Compositor::AsyncScrollOffset>& pending_scroll_offsets,
    Web::Compositor::AsyncScrollOffset const& scroll_offset)
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

ContextState::ContextState(Optional<u64> page_id, CompositorStateWebContentClient& web_content_client, bool async_scrolling_enabled)
    : m_web_content_client(web_content_client)
    , m_page_id(page_id)
    , m_async_scrolling_enabled(async_scrolling_enabled)
{
    if (page_id.has_value())
        m_presentation_mode = Web::Compositor::PresentToClient {};
}

ContextState::~ContextState()
{
    stop_backing_store_shrink_timer();
}

bool ContextState::presentation_mode_presents_to_client(Web::Compositor::PresentationMode const& presentation_mode)
{
    return presentation_mode.has<Web::Compositor::PresentToClient>();
}

bool ContextState::is_owned_by(CompositorStateWebContentClient const& web_content_client) const
{
    return &m_web_content_client == &web_content_client;
}

void ContextState::request_rendering_update()
{
    m_web_content_client.request_rendering_update();
}

void ContextState::dispatch_mouse_event_to_web_content(Web::MouseEvent const& event)
{
    VERIFY(m_page_id.has_value());
    m_web_content_client.dispatch_mouse_event_to_web_content(*m_page_id, event);
}

void ContextState::set_presentation_mode(Web::Compositor::PresentationMode presentation_mode)
{
    if (presentation_mode_presents_to_client(presentation_mode))
        VERIFY(m_page_id.has_value());
    m_presentation_mode = move(presentation_mode);
}

void ContextState::did_stop_presenting_to_client_if_needed(bool was_presenting_to_client, bool will_present_to_client)
{
    if (was_presenting_to_client && !will_present_to_client
        && m_gpu_present_bitmap_id_awaiting_completion.has_value()
        && m_presented_bitmap_id_awaiting_ack == m_gpu_present_bitmap_id_awaiting_completion)
        m_presented_bitmap_id_awaiting_ack.clear();
}

void ContextState::set_published_surface(PublishedSurface published_surface)
{
    m_published_surface = published_surface;
}

Optional<ContextState::PublishedSurface> ContextState::take_published_surface()
{
    if (!m_published_surface.has_value())
        return {};
    return m_published_surface.release_value();
}

void ContextState::did_detach_from_parent_surface(Web::Compositor::CompositorContextId parent_context_id, Web::Painting::CompositorSurfaceId surface_id)
{
    VERIFY(m_published_surface.has_value());
    VERIFY(m_published_surface->parent_context_id == parent_context_id);
    VERIFY(m_published_surface->surface_id == surface_id);
    m_published_surface.clear();
    m_presentation_mode = Empty {};
}

void ContextState::attach_child_surface(Web::Painting::CompositorSurfaceId surface_id, Web::Compositor::CompositorContextId child_context_id)
{
    m_child_contexts_by_surface_id.set(surface_id, child_context_id);
}

Optional<Web::Compositor::CompositorContextId> ContextState::take_child_context_for_surface(Web::Painting::CompositorSurfaceId surface_id)
{
    return m_child_contexts_by_surface_id.take(surface_id);
}

Vector<ContextState::ChildSurface> ContextState::child_contexts() const
{
    Vector<ChildSurface> child_contexts;
    child_contexts.ensure_capacity(m_child_contexts_by_surface_id.size());
    for (auto& child_context : m_child_contexts_by_surface_id)
        child_contexts.unchecked_append({ child_context.key, child_context.value });
    return child_contexts;
}

void ContextState::apply_display_list_resource_transaction(Web::Painting::DisplayListResourceTransaction&& resource_transaction)
{
    m_display_list_resource_storage.apply_transaction(move(resource_transaction));
}

void ContextState::install_display_list_update(
    NonnullRefPtr<Web::Painting::DisplayList> display_list,
    Web::Painting::AccumulatedVisualContextTree visual_context_tree,
    Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    VERIFY(display_list->compatible_visual_context_tree_version() == visual_context_tree.version());
    m_display_list = move(display_list);
    m_visual_context_tree = move(visual_context_tree);
    m_scroll_state_snapshot = move(scroll_state_snapshot);

    if (!m_async_scrolling_enabled)
        return;

    auto async_scrolling_state = Web::Compositor::async_scrolling_state_from_display_list(*m_display_list);
    auto async_scrolling_viewport_rect = async_scrolling_state.viewport_rect;
    auto wheel_event_listener_state_generation = async_scrolling_state.wheel_event_listener_state_generation;
    auto wheel_routing_admission = Web::Compositor::wheel_routing_admission_for(async_scrolling_state);
    if (wheel_event_listener_state_generation < m_wheel_event_listener_state_generation)
        wheel_routing_admission = Web::Compositor::WheelRoutingAdmission::StaleWheelEventListeners;
    else
        m_wheel_event_listener_state_generation = wheel_event_listener_state_generation;

    m_wheel_routing_admission = wheel_routing_admission;
    m_can_accept_async_wheel_events = wheel_routing_admission == Web::Compositor::WheelRoutingAdmission::Accepted;

    m_viewport_scrollbar_controller.set_scrollbars(async_scrolling_state.viewport_scrollbars);
    m_async_scroll_tree.set_state(move(async_scrolling_state));
    if (!m_pending_async_scroll_offsets.is_empty()) {
        if (auto viewport_scroll_offset = reapply_pending_async_scroll_offsets(m_pending_async_scroll_offsets); viewport_scroll_offset.has_value())
            async_scrolling_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
    }
    rebuild_wheel_hit_test_targets();
    m_async_scrolling_viewport_rect = async_scrolling_viewport_rect;
    m_has_async_scrolling_state = true;
}

void ContextState::update_scroll_state(Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    m_scroll_state_snapshot = move(scroll_state_snapshot);
    if (!m_has_async_scrolling_state)
        return;

    auto reconciled_viewport_scroll_offset = reapply_pending_async_scroll_offsets(m_pending_async_scroll_offsets);
    rebuild_wheel_hit_test_targets();
    if (reconciled_viewport_scroll_offset.has_value()) {
        auto reconciled_viewport_rect = m_async_scrolling_viewport_rect;
        reconciled_viewport_rect.set_location(reconciled_viewport_scroll_offset->to_type<int>());
        m_async_scrolling_viewport_rect = reconciled_viewport_rect;
    }
}

void ContextState::update_video_frame(Web::Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame)
{
    m_display_list_resource_storage.update_video_frame(frame_id, move(frame));
}

void ContextState::clear_video_frame(Web::Painting::VideoFrameResourceId frame_id)
{
    m_display_list_resource_storage.clear_video_frame(frame_id);
}

void ContextState::update_compositor_surface(Web::Painting::CompositorSurfaceId surface_id, Gfx::SharedImage&& shared_image)
{
    m_display_list_resource_storage.update_compositor_surface(surface_id, move(shared_image));
}

void ContextState::clear_compositor_surface(Web::Painting::CompositorSurfaceId surface_id)
{
    m_display_list_resource_storage.clear_compositor_surface(surface_id);
}

Gfx::SharedImage ContextState::snapshot_front_store()
{
    return m_backing_store_manager.front_store().snapshot_into_shared_image();
}

void ContextState::invalidate_wheel_event_listener_state(u64 generation)
{
    m_wheel_event_listener_state_generation = max(m_wheel_event_listener_state_generation, generation);
    m_wheel_routing_admission = Web::Compositor::WheelRoutingAdmission::StaleWheelEventListeners;
    m_can_accept_async_wheel_events = false;
}

ContextState::ContextUpdateResult ContextState::handle_mouse_event(Web::MouseEvent const& event)
{
    if (!presents_to_client())
        return {};

    auto position = Gfx::FloatPoint {
        static_cast<float>(event.position.x().value()),
        static_cast<float>(event.position.y().value()),
    };

    switch (event.type) {
    case Web::MouseEvent::Type::MouseDown: {
        if (event.button != Web::UIEvents::MouseButton::Primary)
            return {};

        auto drag = m_viewport_scrollbar_controller.begin_drag(m_async_scroll_tree, m_scroll_state_snapshot, position);
        if (!drag.has_value())
            return {};

        ContextUpdateResult result;
        result.accepted = true;
        if (!m_async_scrolling_viewport_rect.is_empty())
            result.frame_to_present = m_async_scrolling_viewport_rect;
        if (auto frame_to_present = apply_viewport_scrollbar_drag(*drag); frame_to_present.has_value()) {
            result.frame_to_present = *frame_to_present;
            result.should_request_rendering_update = true;
        }
        return result;
    }
    case Web::MouseEvent::Type::MouseMove: {
        auto had_capture = m_viewport_scrollbar_controller.has_captured_scrollbar();
        if (had_capture) {
            auto drag = m_viewport_scrollbar_controller.captured_drag(position);
            if (!drag.has_value())
                return {};
            auto frame_to_present = apply_viewport_scrollbar_drag(*drag);
            return { .accepted = true, .frame_to_present = frame_to_present, .should_request_rendering_update = frame_to_present.has_value() };
        }

        auto hovered_scrollbar_index = m_viewport_scrollbar_controller.hit_test(m_async_scroll_tree, m_scroll_state_snapshot, position);
        Optional<Gfx::IntRect> frame_to_present;
        if (m_viewport_scrollbar_controller.set_hovered_scrollbar(hovered_scrollbar_index) && !m_async_scrolling_viewport_rect.is_empty())
            frame_to_present = m_async_scrolling_viewport_rect;
        return {
            .accepted = hovered_scrollbar_index.has_value(),
            .frame_to_present = frame_to_present,
            .should_request_rendering_update = false,
        };
    }
    case Web::MouseEvent::Type::MouseUp: {
        auto drag = m_viewport_scrollbar_controller.release_captured_drag(position);
        if (!drag.has_value())
            return {};

        ContextUpdateResult result;
        result.accepted = true;
        if (!m_async_scrolling_viewport_rect.is_empty())
            result.frame_to_present = m_async_scrolling_viewport_rect;
        if (auto frame_to_present = apply_viewport_scrollbar_drag(*drag); frame_to_present.has_value()) {
            result.frame_to_present = *frame_to_present;
            result.should_request_rendering_update = true;
        }
        return result;
    }
    case Web::MouseEvent::Type::MouseLeave: {
        auto had_capture = m_viewport_scrollbar_controller.has_captured_scrollbar();
        Optional<Gfx::IntRect> frame_to_present;
        if (m_viewport_scrollbar_controller.set_hovered_scrollbar({}) && !m_async_scrolling_viewport_rect.is_empty())
            frame_to_present = m_async_scrolling_viewport_rect;
        return {
            .accepted = had_capture,
            .frame_to_present = frame_to_present,
            .should_request_rendering_update = false,
        };
    }
    case Web::MouseEvent::Type::MouseWheel:
        return {};
    }

    VERIFY_NOT_REACHED();
}

ContextState::AsyncScrollResult ContextState::async_scroll_by(
    Web::UniqueNodeID expected_document_id,
    Gfx::FloatPoint position,
    Gfx::FloatPoint delta,
    Gfx::IntRect viewport_rect,
    Web::Compositor::AsyncScrollOperationTracking operation_tracking)
{
    if (!m_can_accept_async_wheel_events)
        return {};

    auto scroll_target = m_async_scroll_tree.hit_test_scroll_node_for_wheel(position, delta);
    if (scroll_target.blocked_by_main_thread_region || scroll_target.blocked_by_wheel_event_region || !scroll_target.node_id.has_value())
        return {};
    if (scroll_target.node_id->document_id != expected_document_id)
        return {};

    Optional<Web::Compositor::AsyncScrollOperationID> operation_id;
    if (operation_tracking == Web::Compositor::AsyncScrollOperationTracking::Yes)
        operation_id = ++m_next_async_scroll_operation_id;

    auto async_scroll_viewport_rect = viewport_rect;
    auto scroll_offsets = m_async_scroll_tree.apply_scroll_delta(*scroll_target.node_id, delta, m_scroll_state_snapshot);
    if (scroll_offsets.is_empty()) {
        if (operation_id.has_value())
            m_completed_async_scroll_operation_ids.append(*operation_id);
        return {
            .enqueue_result = { true, operation_id },
            .frame_to_present = {},
        };
    }

    rebuild_wheel_hit_test_targets();
    if (auto viewport_scroll_offset = viewport_scroll_offset_from(scroll_offsets); viewport_scroll_offset.has_value())
        async_scroll_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
    store_pending_async_scroll_offsets(scroll_offsets, operation_id);
    m_async_scrolling_viewport_rect = async_scroll_viewport_rect;
    return { .enqueue_result = { true, operation_id }, .frame_to_present = async_scroll_viewport_rect };
}

ContextState::ContextUpdateResult ContextState::async_scroll_by(Gfx::FloatPoint position, Gfx::FloatPoint delta)
{
    if (!presents_to_client())
        return {};
    if (!m_can_accept_async_wheel_events)
        return {};

    auto scroll_target = m_async_scroll_tree.hit_test_scroll_node_for_wheel(position, delta);
    if (scroll_target.blocked_by_main_thread_region || scroll_target.blocked_by_wheel_event_region || !scroll_target.node_id.has_value())
        return {};

    auto async_scroll_viewport_rect = m_async_scrolling_viewport_rect;
    auto scroll_offsets = m_async_scroll_tree.apply_scroll_delta(*scroll_target.node_id, delta, m_scroll_state_snapshot);
    if (scroll_offsets.is_empty())
        return {
            .accepted = true,
            .frame_to_present = {},
            .should_request_rendering_update = false,
        };

    rebuild_wheel_hit_test_targets();
    if (auto viewport_scroll_offset = viewport_scroll_offset_from(scroll_offsets); viewport_scroll_offset.has_value())
        async_scroll_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
    store_pending_async_scroll_offsets(scroll_offsets);
    m_async_scrolling_viewport_rect = async_scroll_viewport_rect;
    return { .accepted = true, .frame_to_present = async_scroll_viewport_rect, .should_request_rendering_update = true };
}

bool ContextState::should_defer_main_thread_present_for_async_scroll() const
{
    if (m_pending_async_scroll_offsets.is_empty())
        return false;
    return m_pending_present_frame.has_value()
        || m_gpu_present_bitmap_id_awaiting_completion.has_value()
        || m_presented_bitmap_id_awaiting_ack.has_value();
}

Web::Compositor::PendingAsyncScrollUpdates ContextState::take_pending_async_scroll_updates()
{
    Web::Compositor::PendingAsyncScrollUpdates updates;
    AK::swap(updates.scroll_offsets, m_pending_async_scroll_offsets);
    AK::swap(updates.completed_operation_ids, m_completed_async_scroll_operation_ids);
    return updates;
}

void ContextState::viewport_size_updated(Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress window_resize_in_progress)
{
    m_viewport_size = viewport_size;
    auto is_page_presentation_context = m_page_id.has_value();
    m_window_resize_in_progress = is_page_presentation_context
        ? window_resize_in_progress
        : Web::Compositor::WindowResizingInProgress::No;
}

bool ContextState::should_shrink_backing_stores_after_resize() const
{
    return m_window_resize_in_progress == Web::Compositor::WindowResizingInProgress::Yes;
}

void ContextState::schedule_backing_store_shrink(Function<void()> on_timeout)
{
    if (!m_backing_store_shrink_timer)
        m_backing_store_shrink_timer = Core::Timer::create_single_shot(3000, move(on_timeout));
    m_backing_store_shrink_timer->restart();
}

void ContextState::finish_window_resize()
{
    m_window_resize_in_progress = Web::Compositor::WindowResizingInProgress::No;
}

Optional<BackingStoreManager::Publication> ContextState::resize_backing_stores_if_needed(RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    auto allocation = m_backing_store_manager.resize_backing_stores_if_needed(m_viewport_size, m_window_resize_in_progress);
    if (!allocation.has_value())
        return {};
    return m_backing_store_manager.allocate_backing_stores(*allocation, skia_backend_context, presents_to_client());
}

bool ContextState::set_display_metadata(Optional<u64> display_id, double refresh_rate)
{
    m_display_id = display_id;
    m_display_refresh_rate = refresh_rate;
    return m_pending_present_frame_scheduled;
}

void ContextState::queue_present_frame(Gfx::IntRect viewport_rect)
{
    m_pending_present_frame = viewport_rect;
}

void ContextState::mark_pending_present_frame_scheduled()
{
    m_pending_present_frame_scheduled = true;
}

bool ContextState::has_pending_present_frame_scheduled_on(Optional<u64> display_id) const
{
    return m_pending_present_frame_scheduled && m_display_id == display_id;
}

bool ContextState::can_schedule_pending_present_frame_if_unblocked() const
{
    if (is_present_blocked())
        return false;
    if (!m_pending_present_frame.has_value())
        return false;
    if (m_pending_present_frame_scheduled)
        return false;
    return true;
}

Optional<Gfx::IntRect> ContextState::take_pending_present_frame_if_unblocked()
{
    m_pending_present_frame_scheduled = false;
    if (is_present_blocked())
        return {};
    if (!m_pending_present_frame.has_value())
        return {};
    return m_pending_present_frame.release_value();
}

bool ContextState::needs_synchronous_present_for_screenshot() const
{
    return m_pending_present_frame.has_value() || m_pending_present_frame_scheduled;
}

Optional<Gfx::IntRect> ContextState::current_frame_rect_to_present() const
{
    if (m_pending_present_frame.has_value())
        return {};
    if (!m_presented_frame.has_value())
        return {};
    return m_presented_frame;
}

Optional<ContextState::PreparedFrame> ContextState::prepare_frame(Web::Painting::DisplayListPlayerSkia& display_list_player, Gfx::IntRect viewport_rect)
{
    if (is_present_blocked()) {
        m_pending_present_frame = viewport_rect;
        return {};
    }

    if (!can_render_frame()) {
        m_presented_frame = viewport_rect;
        return {};
    }

    auto& back_store = m_backing_store_manager.back_store();
    m_presentation_mode.visit(
        [](Empty const&) {},
        [](Web::Compositor::PresentToClient const&) {},
        [&](Web::Compositor::PublishToCompositorSurface const&) {
            Gfx::PainterSkia painter { NonnullRefPtr<Gfx::PaintingSurface> { back_store } };
            painter.clear_rect(back_store.rect().to_type<float>(), Gfx::Color::Transparent);
        });
    paint_current_display_list(display_list_player, back_store);

    auto rendered_bitmap_id = m_backing_store_manager.back_bitmap_id();
    m_gpu_present_bitmap_id_awaiting_completion = rendered_bitmap_id;
    if (presents_to_client())
        m_presented_bitmap_id_awaiting_ack = rendered_bitmap_id;
    return PreparedFrame {
        .rendered_surface = &back_store,
        .bitmap_id = rendered_bitmap_id,
    };
}

void ContextState::did_submit_prepared_frame(Gfx::IntRect viewport_rect)
{
    m_backing_store_manager.swap();
    m_presented_frame = viewport_rect;
}

Optional<Web::Compositor::PublishToCompositorSurface> ContextState::present_synchronously(Web::Painting::DisplayListPlayerSkia& display_list_player)
{
    auto* publish_mode = m_presentation_mode.get_pointer<Web::Compositor::PublishToCompositorSurface>();
    VERIFY(publish_mode);
    if (!can_render_frame())
        return {};
    // Don't race an async present already in flight for this context; its own completion will publish.
    if (is_present_blocked())
        return {};

    auto viewport_rect = m_pending_present_frame;
    if (!viewport_rect.has_value())
        viewport_rect = m_presented_frame;
    if (!viewport_rect.has_value())
        return {};

    auto& back_store = m_backing_store_manager.back_store();
    {
        Gfx::PainterSkia painter { NonnullRefPtr<Gfx::PaintingSurface> { back_store } };
        painter.clear_rect(back_store.rect().to_type<float>(), Gfx::Color::Transparent);
    }
    paint_current_display_list(display_list_player, back_store);
    display_list_player.flush(back_store);
    m_backing_store_manager.swap();
    m_presented_frame = viewport_rect;
    m_pending_present_frame.clear();
    m_pending_present_frame_scheduled = false;
    return *publish_mode;
}

bool ContextState::can_paint_screenshot(Gfx::ShareableBitmap& target_bitmap) const
{
    return m_display_list && target_bitmap.is_valid() && target_bitmap.bitmap();
}

void ContextState::paint_screenshot(Web::Painting::DisplayListPlayerSkia& display_list_player, Gfx::ShareableBitmap& target_bitmap)
{
    VERIFY(can_paint_screenshot(target_bitmap));

    auto target_surface = Gfx::PaintingSurface::wrap_bitmap(*target_bitmap.bitmap());
    paint_current_display_list(display_list_player, *target_surface);
    display_list_player.flush(*target_surface);
}

bool ContextState::acknowledge_presented_bitmap(i32 bitmap_id)
{
    if (m_presented_bitmap_id_awaiting_ack != bitmap_id)
        return false;

    m_presented_bitmap_id_awaiting_ack.clear();
    return true;
}

void ContextState::did_finish_gpu_present(i32 bitmap_id)
{
    VERIFY(m_gpu_present_bitmap_id_awaiting_completion == bitmap_id);
    m_gpu_present_bitmap_id_awaiting_completion.clear();
}

void ContextState::stop_backing_store_shrink_timer()
{
    if (!m_backing_store_shrink_timer)
        return;
    m_backing_store_shrink_timer->on_timeout = {};
    m_backing_store_shrink_timer->stop();
}

Web::Painting::AccumulatedVisualContextTree const& ContextState::current_visual_context_tree() const
{
    VERIFY(m_display_list);
    VERIFY(m_visual_context_tree.has_value());
    return m_visual_context_tree.value();
}

Optional<Gfx::FloatPoint> ContextState::viewport_scroll_offset_from(Vector<Web::Compositor::AsyncScrollOffset> const& scroll_offsets) const
{
    Optional<Gfx::FloatPoint> viewport_scroll_offset;
    for (auto const& scroll_offset : scroll_offsets) {
        auto node_id = m_async_scroll_tree.scroll_node_id_for_stable_id(scroll_offset.stable_node_id);
        if (node_id.has_value() && m_async_scroll_tree.scroll_node_is_viewport(*node_id))
            viewport_scroll_offset = scroll_offset.compositor_scroll_offset;
    }
    return viewport_scroll_offset;
}

Optional<Gfx::FloatPoint> ContextState::reapply_pending_async_scroll_offsets(Vector<Web::Compositor::AsyncScrollOffset> const& pending_scroll_offsets)
{
    Optional<Gfx::FloatPoint> viewport_scroll_offset;
    for (auto const& pending_scroll_offset : pending_scroll_offsets) {
        auto node_id = m_async_scroll_tree.scroll_node_id_for_stable_id(pending_scroll_offset.stable_node_id);
        if (!node_id.has_value())
            continue;
        auto reconciled_scroll_offset = m_async_scroll_tree.set_scroll_offset(
            *node_id,
            pending_scroll_offset.compositor_scroll_offset,
            m_scroll_state_snapshot);
        if (reconciled_scroll_offset.has_value() && m_async_scroll_tree.scroll_node_is_viewport(*node_id))
            viewport_scroll_offset = *reconciled_scroll_offset;
    }
    return viewport_scroll_offset;
}

void ContextState::store_pending_async_scroll_offsets(
    Vector<Web::Compositor::AsyncScrollOffset> const& scroll_offsets,
    Optional<Web::Compositor::AsyncScrollOperationID> operation_id)
{
    for (auto const& scroll_offset : scroll_offsets)
        set_or_append_pending_scroll_offset(m_pending_async_scroll_offsets, scroll_offset);
    if (operation_id.has_value())
        m_completed_async_scroll_operation_ids.append(*operation_id);
}

Optional<Gfx::IntRect> ContextState::apply_viewport_scrollbar_drag(ViewportScrollbarController::Drag const& drag)
{
    auto scroll_delta = m_viewport_scrollbar_controller.scroll_delta_for_drag(m_async_scroll_tree, m_scroll_state_snapshot, drag);
    if (!scroll_delta.has_value())
        return {};

    auto scroll_offsets = m_async_scroll_tree.apply_scroll_delta(scroll_delta->scroll_node_id, scroll_delta->delta, m_scroll_state_snapshot);
    if (scroll_offsets.is_empty())
        return {};
    rebuild_wheel_hit_test_targets();

    auto viewport_scroll_offset = viewport_scroll_offset_from(scroll_offsets);
    if (!viewport_scroll_offset.has_value())
        return {};

    store_pending_async_scroll_offsets(scroll_offsets);
    auto async_scroll_viewport_rect = m_async_scrolling_viewport_rect;
    async_scroll_viewport_rect.set_location(viewport_scroll_offset->to_type<int>());
    m_async_scrolling_viewport_rect = async_scroll_viewport_rect;
    return async_scroll_viewport_rect;
}

void ContextState::rebuild_wheel_hit_test_targets()
{
    VERIFY(m_display_list);
    m_async_scroll_tree.rebuild_wheel_hit_test_targets(
        m_display_list,
        &current_visual_context_tree(),
        m_scroll_state_snapshot);
}

bool ContextState::is_present_blocked() const
{
    return m_gpu_present_bitmap_id_awaiting_completion.has_value()
        || m_presented_bitmap_id_awaiting_ack.has_value();
}

bool ContextState::can_render_frame() const
{
    return m_display_list && m_backing_store_manager.is_valid();
}

void ContextState::paint_current_display_list(Web::Painting::DisplayListPlayerSkia& display_list_player, Gfx::PaintingSurface& surface)
{
    VERIFY(m_display_list);
    display_list_player.execute(*m_display_list, current_visual_context_tree(), m_display_list_resource_storage, m_scroll_state_snapshot, surface);
    m_viewport_scrollbar_controller.paint(surface, display_list_player, m_scroll_state_snapshot);
}

}
