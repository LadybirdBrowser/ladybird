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

namespace Compositor {

static constexpr int gpu_completion_check_interval_ms = 1;

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

void CompositorState::set_client(CompositorStateClient& client)
{
    m_client = &client;
}

CompositorState::ContextOwnerCheckResult CompositorState::check_context_owner(Web::Compositor::CompositorContextId context_id, CompositorStateWebContentClient& client)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return ContextOwnerCheckResult::ContextUnavailable;
    if (!context->is_owned_by(client))
        return ContextOwnerCheckResult::ConflictingOwner;

    return ContextOwnerCheckResult::OwnedByClient;
}

void CompositorState::destroy_contexts_for_web_content_client(CompositorStateWebContentClient& client)
{
    Vector<Web::Compositor::CompositorContextId> context_ids;
    for (auto& context : m_contexts) {
        if (context.value->is_owned_by(client))
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

    auto& context = *m_contexts.ensure(context_id, [&] {
        return make<ContextState>(page_id, web_content_client, m_canvas_surface_registry, m_async_scrolling_enabled);
    });
    resize_backing_stores_if_needed(context_id, context);
}

void CompositorState::destroy_context(Web::Compositor::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    cancel_pending_async_presents_for_context(context_id);
    clear_parent_context(*context);
    for (auto& context_entry : m_contexts) {
        if (context_entry.key == context_id)
            continue;
        auto& possible_child_context = *context_entry.value;
        auto parent_context_id = possible_child_context.parent_context_id();
        if (parent_context_id.has_value() && *parent_context_id == context_id)
            possible_child_context.set_parent_context({});
    }
    m_contexts.remove(context_id);
}

void CompositorState::set_parent_context(Web::Compositor::CompositorContextId context_id, Optional<Web::Compositor::CompositorContextId> parent_context_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    if (parent_context_id.has_value()) {
        VERIFY(!context->presents_to_client());
        VERIFY(*parent_context_id != context_id);
        VERIFY(context_if_present(*parent_context_id));
    }

    auto current_parent_context_id = context->parent_context_id();
    if (current_parent_context_id.has_value() == parent_context_id.has_value()
        && (!current_parent_context_id.has_value() || *current_parent_context_id == *parent_context_id))
        return;

    clear_parent_context(*context);
    context->set_parent_context(parent_context_id);

    if (!parent_context_id.has_value())
        return;

    if (!context->latest_rendered_surface() && !context->needs_rasterization())
        return;

    auto* parent_context = context_if_present(*parent_context_id);
    VERIFY(parent_context);
    present_current_frame(*parent_context_id, *parent_context);
}

void CompositorState::stop_presenting_to_client(Web::Compositor::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->stop_presenting_to_client();
}

void CompositorState::update_display_list(Web::Compositor::CompositorContextId context_id, NonnullRefPtr<Web::Painting::DisplayList> display_list, Web::Painting::AccumulatedVisualContextTree visual_context_tree, Web::Painting::DisplayListResourceTransaction&& resource_transaction, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    context->apply_display_list_resource_transaction(move(resource_transaction));
    context->install_display_list_update(move(display_list), move(visual_context_tree), move(scroll_state_snapshot));
}

void CompositorState::update_visual_context_tree(Web::Compositor::CompositorContextId context_id, Web::Painting::AccumulatedVisualContextTree visual_context_tree)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    context->update_visual_context_tree(move(visual_context_tree));
}

void CompositorState::update_scroll_state(Web::Compositor::CompositorContextId context_id, Web::Painting::ScrollStateSnapshot&& scroll_state_snapshot)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    context->update_scroll_state(move(scroll_state_snapshot));
}

void CompositorState::update_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id, NonnullRefPtr<Media::VideoFrame const> frame)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->update_video_frame(frame_id, move(frame));
    present_current_frame(context_id, *context);
}

void CompositorState::clear_video_frame(Web::Compositor::CompositorContextId context_id, Web::Painting::VideoFrameResourceId frame_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->clear_video_frame(frame_id);
    present_current_frame(context_id, *context);
}

void CompositorState::invalidate_wheel_event_listener_state(Web::Compositor::CompositorContextId context_id, u64 generation)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->invalidate_wheel_event_listener_state(generation);
}

bool CompositorState::handle_mouse_event(Web::Compositor::CompositorContextId context_id, Web::MouseEvent const& event)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return false;

    return apply_context_update_result(context_id, *context, context->handle_mouse_event(event));
}

bool CompositorState::dispatch_mouse_event_to_web_content(Web::Compositor::CompositorContextId context_id, Web::MouseEvent const& event)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return false;

    context->dispatch_mouse_event_to_web_content(event);
    return true;
}

bool CompositorState::handle_pinch_event(Web::Compositor::CompositorContextId context_id, Web::PinchEvent const& event)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return false;

    return apply_context_update_result(context_id, *context, context->handle_pinch_event(event));
}

Web::Compositor::AsyncScrollEnqueueResult CompositorState::async_scroll_by(Web::Compositor::CompositorContextId context_id, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::AsyncScrollOperationTracking operation_tracking)
{
    if (!m_async_scrolling_enabled)
        return {};

    auto* context = context_if_present(context_id);
    VERIFY(context);

    auto result = context->async_scroll_by(expected_document_id, position, delta, viewport_rect, operation_tracking);
    if (result.frame_to_present.has_value())
        schedule_present_frame(context_id, *context, *result.frame_to_present);
    return result.enqueue_result;
}

bool CompositorState::async_scroll_by(Web::Compositor::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta)
{
    if (!m_async_scrolling_enabled)
        return false;

    auto* context = context_if_present(context_id);
    if (!context)
        return false;

    return apply_context_update_result(context_id, *context, context->async_scroll_by(position, delta));
}

bool CompositorState::should_defer_main_thread_present_for_async_scroll(Web::Compositor::CompositorContextId context_id) const
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    return context->should_defer_main_thread_present_for_async_scroll();
}

Web::Compositor::PendingAsyncScrollUpdates CompositorState::take_pending_async_scroll_updates(Web::Compositor::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    return context->take_pending_async_scroll_updates();
}

void CompositorState::viewport_size_updated(Web::Compositor::CompositorContextId context_id, Gfx::IntSize viewport_size, Web::Compositor::WindowResizingInProgress window_resize_in_progress)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    context->viewport_size_updated(viewport_size, window_resize_in_progress);
    resize_backing_stores_if_needed(context_id, *context);
    if (context->should_shrink_backing_stores_after_resize())
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

    if (context->set_display_metadata(display_id, refresh_rate))
        schedule_pending_present_frame(context_id, *context);
}

void CompositorState::present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    schedule_present_frame(context_id, *context, viewport_rect);
}

void CompositorState::present_frame(Web::Compositor::CompositorContextId context_id, ContextState& context, Gfx::IntRect viewport_rect)
{
    auto composited_context_resolver = resolver_for(context_id);
    auto prepared_frame = context.prepare_frame(*m_display_list_player, viewport_rect, &composited_context_resolver);
    if (!prepared_frame.has_value())
        return;

    m_pending_async_presents.append(context_id, viewport_rect, prepared_frame->bitmap_id);
    auto* pending_present = &m_pending_async_presents.last();

    auto& event_loop = Core::EventLoop::current();
    auto self = NonnullRefPtr { *this };
    m_display_list_player->flush_async(*prepared_frame->rendered_surface, [self = move(self), &event_loop, pending_present] {
        event_loop.deferred_invoke([self = move(self), pending_present] {
            self->did_finish_async_present(*pending_present);
        });
    });
    context.did_submit_prepared_frame(viewport_rect);
    schedule_gpu_completion_check();
}

void CompositorState::schedule_present_frame(Web::Compositor::CompositorContextId context_id, ContextState& context, Gfx::IntRect viewport_rect)
{
    context.queue_present_frame(viewport_rect);
    schedule_pending_present_frame(context_id, context);
}

void CompositorState::schedule_pending_present_frame(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    if (!context.presents_to_client()) {
        schedule_containing_context_present(context);
        return;
    }

    schedule_pending_present_frame_on_vsync(context_id, context);
}

void CompositorState::schedule_pending_present_frame_on_vsync(Web::Compositor::CompositorContextId, ContextState& context)
{
    context.mark_pending_present_frame_scheduled();
    vsync_scheduler_for_display(context.display_id()).schedule(context.display_refresh_rate());
}

void CompositorState::schedule_containing_context_present(ContextState& context)
{
    auto parent_context_id = context.parent_context_id();
    if (!parent_context_id.has_value())
        return;

    auto* parent_context = context_if_present(*parent_context_id);
    VERIFY(parent_context);
    present_current_frame(*parent_context_id, *parent_context);
}

void CompositorState::schedule_pending_present_frame_if_unblocked(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    if (!context.can_schedule_pending_present_frame_if_unblocked())
        return;

    schedule_pending_present_frame(context_id, context);
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
        if (!context.has_pending_present_frame_scheduled_on(display_id))
            continue;

        auto pending_present_frame = context.take_pending_present_frame_if_unblocked();
        if (!pending_present_frame.has_value())
            continue;
        present_frame(context_id, context, *pending_present_frame);
    }
}

bool CompositorState::request_screenshot(Web::Compositor::CompositorContextId context_id, Gfx::ShareableBitmap& target_bitmap)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    if (!context->can_paint_screenshot(target_bitmap))
        return false;

    auto composited_context_resolver = resolver_for(context_id);
    context->paint_screenshot(*m_display_list_player, target_bitmap, &composited_context_resolver);
    return true;
}

void CompositorState::presented_bitmap_ready_to_paint(Web::Compositor::CompositorContextId context_id, i32 bitmap_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    if (!context->acknowledge_presented_bitmap(bitmap_id))
        return;

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

    context->did_finish_gpu_present(bitmap_id);
    if (context->presents_to_client()) {
        VERIFY(m_client);
        m_client->did_present_frame(context_id, viewport_rect, bitmap_id);
    }
    if (auto parent_context_id = context->parent_context_id(); parent_context_id.has_value()) {
        auto* parent_context = context_if_present(*parent_context_id);
        VERIFY(parent_context);
        present_current_frame(*parent_context_id, *parent_context);
    }

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
    if (!m_skia_backend_context)
        return;
    VERIFY(!m_pending_async_presents.is_empty());

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

ContextState* CompositorState::context_if_present(Web::Compositor::CompositorContextId context_id)
{
    auto it = m_contexts.find(context_id);
    if (it == m_contexts.end())
        return nullptr;
    return it->value.ptr();
}

ContextState const* CompositorState::context_if_present(Web::Compositor::CompositorContextId context_id) const
{
    auto it = m_contexts.find(context_id);
    if (it == m_contexts.end())
        return nullptr;
    return it->value.ptr();
}

void CompositorState::clear_parent_context(ContextState& context)
{
    auto parent_context_id = context.parent_context_id();
    if (!parent_context_id.has_value())
        return;

    context.set_parent_context({});
    auto* parent_context = context_if_present(*parent_context_id);
    if (!parent_context)
        return;
    present_current_frame(*parent_context_id, *parent_context);
}

CompositedContextResolver CompositorState::resolver_for(Web::Compositor::CompositorContextId parent_context_id)
{
    return [this, parent_context_id](Web::Compositor::CompositorContextId child_context_id) {
        return resolve_composited_context(parent_context_id, child_context_id);
    };
}

RefPtr<Gfx::PaintingSurface> CompositorState::resolve_composited_context(Web::Compositor::CompositorContextId parent_context_id, Web::Compositor::CompositorContextId child_context_id)
{
    auto* child_context = context_if_present(child_context_id);
    if (!child_context)
        return nullptr;
    auto child_parent_context_id = child_context->parent_context_id();
    if (!child_parent_context_id.has_value() || *child_parent_context_id != parent_context_id)
        return nullptr;

    if (child_context->needs_rasterization()) {
        auto composited_context_resolver = resolver_for(child_context_id);
        Web::Painting::DisplayListPlayerSkia display_list_player { m_skia_backend_context };
        child_context->present_synchronously(display_list_player, &composited_context_resolver);
    }

    return child_context->latest_rendered_surface();
}

void CompositorState::resize_backing_stores_if_needed(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    if (auto publication = context.resize_backing_stores_if_needed(m_skia_backend_context); publication.has_value()) {
        publish_backing_stores(context_id, context, publication.release_value());
        present_current_frame(context_id, context);
    }
}

void CompositorState::schedule_backing_store_shrink(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    context.schedule_backing_store_shrink([this, context_id] {
        shrink_backing_stores_after_resize(context_id);
    });
}

void CompositorState::shrink_backing_stores_after_resize(Web::Compositor::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    context->finish_window_resize();
    resize_backing_stores_if_needed(context_id, *context);
}

void CompositorState::present_current_frame(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    // A queued frame already captures the newest viewport rect and will pick up
    // the updated resource when the outstanding bitmap is acknowledged.
    auto frame_to_present = context.current_frame_rect_to_present();
    if (frame_to_present.has_value())
        schedule_present_frame(context_id, context, *frame_to_present);
}

bool CompositorState::apply_context_update_result(
    Web::Compositor::CompositorContextId context_id,
    ContextState& context,
    ContextState::ContextUpdateResult const& result)
{
    if (result.frame_to_present.has_value())
        schedule_present_frame(context_id, context, *result.frame_to_present);
    if (result.should_request_rendering_update)
        context.request_rendering_update();
    return result.accepted;
}

void CompositorState::publish_backing_stores(Web::Compositor::CompositorContextId context_id, ContextState& context, BackingStoreManager::Publication&& publication)
{
    VERIFY(m_client);
    VERIFY(context.presents_to_client());

    m_client->did_allocate_backing_stores(context_id, publication.front_bitmap_id, move(publication.front_shared_image), publication.back_bitmap_id, move(publication.back_shared_image));
}

}
