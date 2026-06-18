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
#include <LibGfx/Bitmap.h>
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

static Web::Painting::TransformData const& visual_viewport_transform(Web::Painting::AccumulatedVisualContextTree const& visual_context_tree)
{
    auto const& visual_viewport_node = visual_context_tree.node_at(Web::Painting::VISUAL_VIEWPORT_NODE_INDEX);
    auto const* transform = visual_viewport_node.data.get_pointer<Web::Painting::TransformData>();
    VERIFY(transform);
    return *transform;
}

static bool visual_viewport_transforms_match(Web::Painting::TransformData const& a, Web::Painting::TransformData const& b)
{
    static constexpr float transform_epsilon = 0.01f;
    static constexpr float translation_epsilon = 0.5f;

    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column) {
            auto epsilon = (column == 3 && (row == 0 || row == 1)) ? translation_epsilon : transform_epsilon;
            if (AK::fabs(a.matrix[row, column] - b.matrix[row, column]) > epsilon)
                return false;
        }
    }

    return AK::fabs(a.origin.x() - b.origin.x()) <= translation_epsilon
        && AK::fabs(a.origin.y() - b.origin.y()) <= translation_epsilon;
}

static Web::Compositor::AsyncScrollNodeStableID viewport_stable_id_from(Web::Compositor::AsyncScrollNodeID node_id)
{
    return {
        .node_id = node_id.document_id,
        .kind = Web::Compositor::AsyncScrollNodeKind::Viewport,
    };
}

static void clamp_visual_viewport_transform_to_viewport(Web::Painting::TransformData& transform, Gfx::IntRect viewport_rect)
{
    auto scale = transform.matrix[0, 0];
    if (scale <= 1.0f) {
        transform.matrix[0, 3] = 0;
        transform.matrix[1, 3] = 0;
        return;
    }

    auto min_x = -static_cast<float>(viewport_rect.width()) * (scale - 1.0f);
    auto min_y = -static_cast<float>(viewport_rect.height()) * (scale - 1.0f);
    transform.matrix[0, 3] = clamp(transform.matrix[0, 3], min_x, 0.0f);
    transform.matrix[1, 3] = clamp(transform.matrix[1, 3], min_y, 0.0f);
}

ContextState::ContextState(Optional<u64> page_id, CompositorStateWebContentClient& web_content_client, Web::Painting::CanvasSurfaceRegistry const& canvas_surface_registry, bool async_scrolling_enabled)
    : m_web_content_client(web_content_client)
    , m_canvas_surface_registry(canvas_surface_registry)
    , m_page_id(page_id)
    , m_async_scrolling_enabled(async_scrolling_enabled)
{
    if (page_id.has_value())
        m_presents_to_client = true;
}

ContextState::~ContextState()
{
    stop_backing_store_shrink_timer();
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

void ContextState::stop_presenting_to_client()
{
    auto was_presenting_to_client = m_presents_to_client;
    m_presents_to_client = false;
    did_stop_presenting_to_client_if_needed(was_presenting_to_client, m_presents_to_client);
}

void ContextState::did_stop_presenting_to_client_if_needed(bool was_presenting_to_client, bool will_present_to_client)
{
    if (was_presenting_to_client && !will_present_to_client
        && m_gpu_present_bitmap_id_awaiting_completion.has_value()
        && m_presented_bitmap_id_awaiting_ack == m_gpu_present_bitmap_id_awaiting_completion)
        m_presented_bitmap_id_awaiting_ack.clear();
}

void ContextState::set_parent_context(Optional<Web::Compositor::CompositorContextId> parent_context_id)
{
    m_parent_context_id = parent_context_id;
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
    m_visual_context_tree_for_compositing.clear();
    m_scroll_state_snapshot = move(scroll_state_snapshot);
    if (m_async_visual_viewport_transform.has_value() && visual_viewport_transforms_match(visual_viewport_transform(*m_visual_context_tree), *m_async_visual_viewport_transform))
        m_async_visual_viewport_transform.clear();

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
    m_has_blocking_wheel_event_listeners = async_scrolling_state.has_blocking_wheel_event_listeners;
    if (m_async_visual_viewport_transform.has_value() && (!m_can_accept_async_wheel_events || m_has_blocking_wheel_event_listeners)) {
        m_async_visual_viewport_transform.clear();
        m_visual_context_tree_for_compositing.clear();
    }

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

void ContextState::update_visual_context_tree(Web::Painting::AccumulatedVisualContextTree visual_context_tree)
{
    VERIFY(m_display_list);
    VERIFY(m_display_list->compatible_visual_context_tree_version() == visual_context_tree.version());
    m_visual_context_tree = move(visual_context_tree);
    m_visual_context_tree_for_compositing.clear();
    if (m_async_visual_viewport_transform.has_value() && visual_viewport_transforms_match(visual_viewport_transform(*m_visual_context_tree), *m_async_visual_viewport_transform))
        m_async_visual_viewport_transform.clear();

    if (m_has_async_scrolling_state)
        rebuild_wheel_hit_test_targets();
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

void ContextState::invalidate_wheel_event_listener_state(u64 generation)
{
    m_wheel_event_listener_state_generation = max(m_wheel_event_listener_state_generation, generation);
    m_wheel_routing_admission = Web::Compositor::WheelRoutingAdmission::StaleWheelEventListeners;
    m_can_accept_async_wheel_events = false;
    m_has_blocking_wheel_event_listeners = true;
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

ContextState::ContextUpdateResult ContextState::handle_pinch_event(Web::PinchEvent const& event)
{
    if (!presents_to_client())
        return {};
    if (!m_can_accept_async_wheel_events)
        return {};
    if (m_has_blocking_wheel_event_listeners)
        return {};
    if (!m_visual_context_tree.has_value())
        return {};

    auto scale = 1.0 + event.scale_delta;
    if (scale == 1.0)
        return {};

    auto transform = m_async_visual_viewport_transform.value_or(visual_viewport_transform(*m_visual_context_tree));
    auto old_scale = transform.matrix[0, 0];
    if (old_scale <= 0)
        return {};

    auto new_scale = clamp(static_cast<double>(old_scale) * scale, 1.0, 5.0);
    auto applied_scale = static_cast<float>(new_scale / old_scale);
    if (applied_scale == 1.0f)
        return {};

    auto position = Gfx::FloatPoint {
        static_cast<float>(event.position.x().value()),
        static_cast<float>(event.position.y().value()),
    };
    auto gesture_transform = Gfx::translation_matrix(Gfx::FloatVector3 { position.x(), position.y(), 0 })
        * Gfx::scale_matrix(Gfx::FloatVector3 { applied_scale, applied_scale, 1 })
        * Gfx::translation_matrix(Gfx::FloatVector3 { -position.x(), -position.y(), 0 });

    transform.matrix = gesture_transform * transform.matrix;
    clamp_visual_viewport_transform_to_viewport(transform, m_async_scrolling_viewport_rect);
    m_async_visual_viewport_transform = transform;
    m_visual_context_tree_for_compositing.clear();
    rebuild_wheel_hit_test_targets();

    return {
        .accepted = true,
        .frame_to_present = m_async_scrolling_viewport_rect,
        .should_request_rendering_update = false,
    };
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

    auto initial_scroll_target = m_async_scroll_tree.hit_test_scroll_node_for_wheel(position, delta);
    if (initial_scroll_target.blocked_by_main_thread_region || initial_scroll_target.blocked_by_wheel_event_region)
        return {};

    Optional<Gfx::IntRect> frame_to_present;
    auto remaining_delta = delta;
    if (auto visual_viewport_scroll_delta = apply_visual_viewport_scroll_delta(delta); visual_viewport_scroll_delta.has_value()) {
        Vector<Web::Compositor::AsyncScrollOffset> scroll_offsets;
        scroll_offsets.append(visual_viewport_scroll_delta->scroll_offset);
        store_pending_async_scroll_offsets(scroll_offsets);
        remaining_delta.translate_by(-visual_viewport_scroll_delta->consumed_delta.x(), -visual_viewport_scroll_delta->consumed_delta.y());
        frame_to_present = m_async_scrolling_viewport_rect;
    }

    if (remaining_delta.is_zero())
        return {
            .accepted = true,
            .frame_to_present = frame_to_present,
            .should_request_rendering_update = frame_to_present.has_value(),
        };

    auto async_scroll_delta = remaining_delta;
    if (auto scale = visual_viewport_scale_for_compositing(); scale.has_value() && *scale > 1.0f)
        async_scroll_delta.scale_by(1.0f / *scale);

    auto scroll_target = m_async_scroll_tree.hit_test_scroll_node_for_wheel(position, async_scroll_delta);
    if (scroll_target.blocked_by_main_thread_region || scroll_target.blocked_by_wheel_event_region || !scroll_target.node_id.has_value()) {
        if (frame_to_present.has_value())
            return {
                .accepted = true,
                .frame_to_present = frame_to_present,
                .should_request_rendering_update = true,
            };
        return {};
    }

    auto async_scroll_viewport_rect = m_async_scrolling_viewport_rect;
    auto scroll_offsets = m_async_scroll_tree.apply_scroll_delta(*scroll_target.node_id, async_scroll_delta, m_scroll_state_snapshot);
    if (scroll_offsets.is_empty())
        return {
            .accepted = true,
            .frame_to_present = frame_to_present,
            .should_request_rendering_update = frame_to_present.has_value(),
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

Optional<ContextState::PreparedFrame> ContextState::prepare_frame(Web::Painting::DisplayListPlayerSkia& display_list_player, Gfx::IntRect viewport_rect, CompositedContextResolver const* composited_context_resolver)
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
    if (!presents_to_client()) {
        Gfx::PainterSkia painter { NonnullRefPtr<Gfx::PaintingSurface> { back_store } };
        painter.clear_rect(back_store.rect().to_type<float>(), Gfx::Color::Transparent);
    }
    paint_current_display_list(display_list_player, back_store, composited_context_resolver);

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

bool ContextState::present_synchronously(Web::Painting::DisplayListPlayerSkia& display_list_player, CompositedContextResolver const* composited_context_resolver)
{
    if (!can_render_frame())
        return false;
    // Don't race an async present already in flight for this context; its own completion will update the output.
    if (is_present_blocked())
        return false;

    auto viewport_rect = m_pending_present_frame;
    if (!viewport_rect.has_value())
        viewport_rect = m_presented_frame;
    if (!viewport_rect.has_value())
        return false;

    auto& back_store = m_backing_store_manager.back_store();
    if (!presents_to_client()) {
        Gfx::PainterSkia painter { NonnullRefPtr<Gfx::PaintingSurface> { back_store } };
        painter.clear_rect(back_store.rect().to_type<float>(), Gfx::Color::Transparent);
    }
    paint_current_display_list(display_list_player, back_store, composited_context_resolver);
    display_list_player.flush(back_store);
    m_backing_store_manager.swap();
    m_latest_rendered_surface = m_backing_store_manager.front_store_if_present();
    m_presented_frame = viewport_rect;
    m_pending_present_frame.clear();
    m_pending_present_frame_scheduled = false;
    return true;
}

bool ContextState::can_paint_screenshot(Gfx::ShareableBitmap& target_bitmap) const
{
    return m_display_list && target_bitmap.is_valid() && target_bitmap.bitmap();
}

void ContextState::paint_screenshot(Web::Painting::DisplayListPlayerSkia& display_list_player, Gfx::ShareableBitmap& target_bitmap, CompositedContextResolver const* composited_context_resolver)
{
    VERIFY(can_paint_screenshot(target_bitmap));

    auto target_surface = Gfx::PaintingSurface::wrap_bitmap(*target_bitmap.bitmap());
    paint_current_display_list(display_list_player, *target_surface, composited_context_resolver);
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
    m_latest_rendered_surface = m_backing_store_manager.front_store_if_present();
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

Optional<float> ContextState::visual_viewport_scale_for_compositing() const
{
    if (!m_visual_context_tree.has_value())
        return {};

    if (m_async_visual_viewport_transform.has_value())
        return m_async_visual_viewport_transform->matrix[0, 0];

    return visual_viewport_transform(*m_visual_context_tree).matrix[0, 0];
}

Optional<ContextState::VisualViewportScrollDelta> ContextState::apply_visual_viewport_scroll_delta(Gfx::FloatPoint delta)
{
    if (delta.is_zero())
        return {};
    if (!m_visual_context_tree.has_value())
        return {};

    auto viewport_node_id = m_async_scroll_tree.viewport_scroll_node_id();
    if (!viewport_node_id.has_value())
        return {};

    auto viewport_scroll_offset = m_async_scroll_tree.scroll_offset_for_node(*viewport_node_id, m_scroll_state_snapshot);
    if (!viewport_scroll_offset.has_value())
        return {};

    auto transform = m_async_visual_viewport_transform.value_or(visual_viewport_transform(*m_visual_context_tree));
    auto scale = transform.matrix[0, 0];
    if (scale <= 1.0f)
        return {};

    auto min_x = -static_cast<float>(m_async_scrolling_viewport_rect.width()) * (scale - 1.0f);
    auto min_y = -static_cast<float>(m_async_scrolling_viewport_rect.height()) * (scale - 1.0f);

    auto old_x = transform.matrix[0, 3];
    auto old_y = transform.matrix[1, 3];
    auto new_x = clamp(old_x - delta.x(), min_x, 0.0f);
    auto new_y = clamp(old_y - delta.y(), min_y, 0.0f);
    Gfx::FloatPoint consumed_delta { old_x - new_x, old_y - new_y };
    if (consumed_delta.is_zero())
        return {};

    transform.matrix[0, 3] = new_x;
    transform.matrix[1, 3] = new_y;
    m_async_visual_viewport_transform = transform;
    m_visual_context_tree_for_compositing.clear();
    rebuild_wheel_hit_test_targets();

    return VisualViewportScrollDelta {
        .scroll_offset = {
            .stable_node_id = viewport_stable_id_from(*viewport_node_id),
            .compositor_scroll_offset = *viewport_scroll_offset,
            .unadopted_scroll_delta = consumed_delta.scaled(1.0f / scale),
        },
        .consumed_delta = consumed_delta,
    };
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
        &visual_context_tree_for_compositing(),
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

Web::Painting::AccumulatedVisualContextTree const& ContextState::visual_context_tree_for_compositing() const
{
    if (!m_async_visual_viewport_transform.has_value())
        return current_visual_context_tree();

    m_visual_context_tree_for_compositing = current_visual_context_tree();
    m_visual_context_tree_for_compositing->set_visual_viewport_transform(*m_async_visual_viewport_transform);
    return *m_visual_context_tree_for_compositing;
}

void ContextState::paint_current_display_list(Web::Painting::DisplayListPlayerSkia& display_list_player, Gfx::PaintingSurface& surface, CompositedContextResolver const* composited_context_resolver)
{
    VERIFY(m_display_list);
    display_list_player.execute(
        *m_display_list,
        visual_context_tree_for_compositing(),
        m_display_list_resource_storage,
        m_scroll_state_snapshot,
        surface,
        &m_canvas_surface_registry,
        composited_context_resolver);
    m_viewport_scrollbar_controller.paint(surface, display_list_player, m_scroll_state_snapshot);
}

}
