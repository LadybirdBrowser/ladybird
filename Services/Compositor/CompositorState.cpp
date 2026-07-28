/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <Compositor/CompositorState.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>

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

    m_video_sink_states.remove(&client);
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
    update_unpainted_video_update_scheduling();
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

    if (display_list->compatible_visual_context_tree_version() != visual_context_tree.version()) {
        dbgln("Compositor: Dropping inconsistent display list update (display list version {}, tree version {})",
            display_list->compatible_visual_context_tree_version(),
            visual_context_tree.version());
        return;
    }

    context->apply_display_list_resource_transaction(move(resource_transaction));
    context->install_display_list_update(move(display_list), move(visual_context_tree), move(scroll_state_snapshot));
    resolve_video_sinks(*context);

    update_unpainted_video_update_scheduling();
}

void CompositorState::update_image_frame_resources(Web::Compositor::CompositorContextId context_id, Vector<Web::Painting::DisplayListImageFrameResource> image_frames)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->update_image_frame_resources(move(image_frames));
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

CompositorState::VideoSinkState* CompositorState::video_sink_state(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle)
{
    auto client_sinks = m_video_sink_states.get(&client);
    if (!client_sinks.has_value())
        return nullptr;
    auto sink_state = client_sinks->get(handle);
    if (!sink_state.has_value())
        return nullptr;
    return &sink_state.value();
}

void CompositorState::add_video_sink(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle)
{
    auto& sinks = m_video_sink_states.ensure(&client, [] { return HashMap<Media::VideoSinkHandle, VideoSinkState> {}; });
    if (sinks.contains(handle))
        return;
    sinks.set(handle, VideoSinkState {});
    client.create_video_edge(handle);
}

void CompositorState::remove_video_sink(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle)
{
    if (auto client_sinks = m_video_sink_states.get(&client); client_sinks.has_value())
        client_sinks->remove(handle);
    client.release_video_edge(handle);
}

void CompositorState::set_video_sink_ticking(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle, bool should_tick)
{
    auto* sink_state = video_sink_state(client, handle);
    if (!sink_state)
        return;
    sink_state->should_tick = should_tick;
    if (should_tick && sink_state->sink)
        present_contexts_drawing_video_sink(client, handle);
    update_unpainted_video_update_scheduling();
}

// The client decides this and we apply it, so that the two processes can never disagree about whether a sink is
// ticked. All we add is that a sink with no edge yet cannot be ticked, which can only delay ticking rather than
// stop it, and resolves as soon as the edge is created.
bool CompositorState::video_sink_updates_are_needed(VideoSinkState const& sink_state)
{
    return sink_state.sink != nullptr && sink_state.should_tick;
}

void CompositorState::on_video_sink_ready(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle, NonnullRefPtr<Media::DisplayingVideoSink> const& sink)
{
    auto* sink_state = video_sink_state(client, handle);
    if (!sink_state)
        return;
    sink_state->sink = sink;
    sink->set_on_present_needed([this, &client, handle] {
        present_contexts_drawing_video_sink(client, handle);
    });
    for (auto& context_entry : m_contexts) {
        if (&context_entry.value->web_content_client() == &client)
            resolve_video_sinks(*context_entry.value);
    }
    present_contexts_drawing_video_sink(client, handle);
    update_unpainted_video_update_scheduling();
}

void CompositorState::update_unpainted_video_update_scheduling()
{
    for (auto& client_entry : m_video_sink_states) {
        for (auto& sink_entry : client_entry.value) {
            auto& sink_state = sink_entry.value;
            if (!video_sink_updates_are_needed(sink_state))
                continue;
            if (!video_sink_is_painted_by_any_context(client_entry.key, sink_entry.key)) {
                schedule_unpainted_video_updates();
                return;
            }
        }
    }
}

void CompositorState::present_contexts_drawing_video_sink(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle)
{
    for (auto& context_entry : m_contexts) {
        auto& context = *context_entry.value;
        if (&context.web_content_client() != &client)
            continue;
        if (!context_is_effectively_visible(context))
            continue;
        for (auto const& resource_entry : context.video_sink_handles()) {
            if (resource_entry.value == handle) {
                if (auto rect = context.video_present_rect(); rect.has_value())
                    schedule_present_frame(context_entry.key, context, *rect);
                break;
            }
        }
    }
}

void CompositorState::resolve_video_sinks(ContextState& context)
{
    auto& client = context.web_content_client();
    for (auto const& resource_entry : context.video_sink_handles()) {
        auto* sink_state = video_sink_state(client, resource_entry.value);
        context.set_video_sink(Web::Painting::VideoSinkResourceId { resource_entry.key }, sink_state ? sink_state->sink : nullptr);
    }
}

bool CompositorState::video_sink_is_painted_by_any_context(CompositorStateWebContentClient* client, Media::VideoSinkHandle handle) const
{
    for (auto const& context_entry : m_contexts) {
        auto const& context = *context_entry.value;
        if (&context.web_content_client() != client)
            continue;
        for (auto const& resource_entry : context.video_sink_handles()) {
            if (resource_entry.value == handle)
                return true;
        }
    }
    return false;
}

int CompositorState::unpainted_video_update_interval_ms() const
{
    auto max_refresh_rate = 60.0;
    for (auto const& context_entry : m_contexts)
        max_refresh_rate = max(max_refresh_rate, context_entry.value->display_refresh_rate());
    return max(1, static_cast<int>(1000.0 / max_refresh_rate));
}

void CompositorState::schedule_unpainted_video_updates()
{
    if (!m_unpainted_video_update_timer) {
        m_unpainted_video_update_timer = Core::Timer::create_repeating(unpainted_video_update_interval_ms(), [this] {
            update_unpainted_video_sinks();
        });
    }
    if (!m_unpainted_video_update_timer->is_active())
        m_unpainted_video_update_timer->start();
}

void CompositorState::update_unpainted_video_sinks()
{
    if (update_all_video_sinks() == VideoSinkUpdateResult::NoUnpaintedSinkRequiresUpdates && m_unpainted_video_update_timer)
        m_unpainted_video_update_timer->stop();
}

CompositorState::VideoSinkUpdateResult CompositorState::update_all_video_sinks()
{
    auto now = MonotonicTime::now();
    auto result = VideoSinkUpdateResult::NoUnpaintedSinkRequiresUpdates;
    for (auto& client_entry : m_video_sink_states) {
        for (auto& sink_entry : client_entry.value) {
            auto& sink_state = sink_entry.value;
            auto is_painted = video_sink_is_painted_by_any_context(client_entry.key, sink_entry.key);
            sink_state.requires_updates = video_sink_updates_are_needed(sink_state)
                && sink_state.sink->update(now).may_require_updates;
            if (!is_painted && sink_state.requires_updates)
                result = VideoSinkUpdateResult::UnpaintedSinkRequiresUpdates;
        }
    }
    return result;
}

void CompositorState::update_video_sinks_for_display(Optional<u64> display_id)
{
    update_all_video_sinks();

    // Keep this display's vsync ticking while any sink painted on it may require updates.
    for (auto& context_entry : m_contexts) {
        auto& context = *context_entry.value;
        if (display_id_for_context(context) != display_id)
            continue;
        if (!context_is_effectively_visible(context))
            continue;
        for (auto const& resource_entry : context.video_sink_handles()) {
            auto* sink_state = video_sink_state(context.web_content_client(), resource_entry.value);
            if (sink_state != nullptr && sink_state->requires_updates) {
                vsync_scheduler_for_display(display_id).schedule(display_refresh_rate_for_context(context));
                break;
            }
        }
    }
}

Optional<u64> CompositorState::display_id_for_context(ContextState const& context) const
{
    auto current_context = &context;
    while (current_context) {
        if (current_context->display_id().has_value())
            return current_context->display_id();
        auto parent_context_id = current_context->parent_context_id();
        if (!parent_context_id.has_value())
            break;
        current_context = context_if_present(*parent_context_id);
    }
    return {};
}

ContextState const* CompositorState::root_context_of(ContextState const& context) const
{
    auto const* current_context = &context;
    while (true) {
        auto parent_context_id = current_context->parent_context_id();
        if (!parent_context_id.has_value())
            return current_context;
        auto const* parent_context = context_if_present(*parent_context_id);
        if (!parent_context)
            return current_context;
        current_context = parent_context;
    }
}

bool CompositorState::context_is_effectively_visible(ContextState const& context) const
{
    return root_context_of(context)->visibility() == Web::Compositor::ContextVisibility::Visible;
}

double CompositorState::display_refresh_rate_for_context(ContextState const& context) const
{
    auto current_context = &context;
    while (current_context) {
        if (current_context->display_id().has_value())
            return current_context->display_refresh_rate();
        auto parent_context_id = current_context->parent_context_id();
        if (!parent_context_id.has_value())
            break;
        current_context = context_if_present(*parent_context_id);
    }
    return context.display_refresh_rate();
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

Web::Compositor::AsyncScrollEnqueueResult CompositorState::async_scroll_by(Web::Compositor::CompositorContextId context_id, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::Compositor::SnapContainerHandling snap_container_handling, Web::Compositor::AsyncScrollOperationTracking operation_tracking)
{
    if (!m_async_scrolling_enabled)
        return {};

    auto* context = context_if_present(context_id);
    VERIFY(context);

    auto result = context->async_scroll_by(expected_document_id, position, delta, viewport_rect, snap_container_handling, operation_tracking);
    if (result.frame_to_present.has_value())
        schedule_present_frame(context_id, *context, *result.frame_to_present);
    return result.enqueue_result;
}

Web::Compositor::AsyncScrollEnqueueResult CompositorState::smooth_scroll_to(Web::Compositor::CompositorContextId context_id, Web::Compositor::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, double device_pixels_per_css_pixel, Web::Compositor::ScrollAnimationKind animation_kind)
{
    if (!m_async_scrolling_enabled)
        return {};

    auto* context = context_if_present(context_id);
    VERIFY(context);

    auto result = context->smooth_scroll_to(stable_node_id, offset, main_thread_offset, viewport_rect, device_pixels_per_css_pixel, animation_kind);
    if (result.frame_to_present.has_value())
        schedule_present_frame(context_id, *context, *result.frame_to_present);
    return result.enqueue_result;
}

void CompositorState::cancel_smooth_scroll(Web::Compositor::CompositorContextId context_id, Web::Compositor::AsyncScrollNodeStableID stable_node_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;
    context->cancel_smooth_scroll(stable_node_id);
}

bool CompositorState::async_scroll_by(Web::Compositor::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Web::Compositor::SnapContainerHandling snap_container_handling)
{
    if (!m_async_scrolling_enabled)
        return false;

    auto* context = context_if_present(context_id);
    if (!context)
        return false;

    return apply_context_update_result(context_id, *context, context->async_scroll_by(position, delta, snap_container_handling));
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

    if (context->set_display_metadata(display_id, refresh_rate)) {
        schedule_pending_present_frame(context_id, *context);
        if (m_unpainted_video_update_timer)
            m_unpainted_video_update_timer->set_interval(unpainted_video_update_interval_ms());
    }
}

void CompositorState::set_context_visibility(Web::Compositor::CompositorContextId context_id, Web::Compositor::ContextVisibility visibility)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;
    if (!context->set_visibility(visibility))
        return;

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} became {}", context_id, visibility == Web::Compositor::ContextVisibility::Visible ? "visible" : "hidden");

    if (visibility == Web::Compositor::ContextVisibility::Visible)
        resume_presentation_after_becoming_visible(context_id, *context);
}

void CompositorState::resume_presentation_after_becoming_visible(Web::Compositor::CompositorContextId root_context_id, ContextState& root_context)
{
    for (auto& context_entry : m_contexts) {
        auto& context = *context_entry.value;
        if (root_context_of(context) != &root_context)
            continue;
        if (context.has_active_smooth_scroll_animations())
            vsync_scheduler_for_display(display_id_for_context(context)).schedule(display_refresh_rate_for_context(context));
    }

    auto frame_rect_to_present = root_context.pending_present_frame_viewport_rect();
    if (!frame_rect_to_present.has_value())
        frame_rect_to_present = root_context.current_frame_rect_to_present();
    if (frame_rect_to_present.has_value())
        schedule_present_frame(root_context_id, root_context, *frame_rect_to_present);
}

void CompositorState::present_frame(Web::Compositor::CompositorContextId context_id, Gfx::IntRect viewport_rect, Gfx::IntRect damage_rect)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    if (context->should_defer_main_thread_present_for_async_scroll()) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Main thread deferred present while async scroll is pending");
        // NB: The in-flight compositor frame may have been rasterized before the
        //     main thread installed its new display list. Preserve the present as
        //     a full repaint so that it uses both the new list and the latest
        //     compositor scroll state once presentation is unblocked.
        schedule_present_frame(context_id, *context, viewport_rect);
        return;
    }
    damage_rect.intersect({ {}, viewport_rect.size() });
    schedule_present_frame(context_id, *context, ContextState::PendingFrame { viewport_rect, damage_rect });
}

void CompositorState::present_frame(Web::Compositor::CompositorContextId context_id, ContextState& context, ContextState::PendingFrame pending_frame)
{
    auto composited_context_resolver = resolver_for(context_id);
    auto prepared_frame = context.prepare_frame(*m_display_list_player, pending_frame, &composited_context_resolver);
    if (!prepared_frame.has_value())
        return;

    m_pending_async_presents.append(context_id, pending_frame.viewport_rect, pending_frame.damage_rect, prepared_frame->bitmap_id);
    auto* pending_present = &m_pending_async_presents.last();

    auto& event_loop = Core::EventLoop::current();
    auto self = NonnullRefPtr { *this };
    m_display_list_player->flush_async(*prepared_frame->rendered_surface, [self = move(self), &event_loop, pending_present] {
        event_loop.deferred_invoke([self = move(self), pending_present] {
            self->did_finish_async_present(*pending_present);
        });
    });
    context.did_submit_prepared_frame(pending_frame.viewport_rect);
    schedule_gpu_completion_check();
}

void CompositorState::schedule_present_frame(Web::Compositor::CompositorContextId context_id, ContextState& context, ContextState::PendingFrame pending_frame)
{
    context.queue_present_frame(pending_frame);
    schedule_pending_present_frame(context_id, context);
}

void CompositorState::schedule_present_frame(Web::Compositor::CompositorContextId context_id, ContextState& context, Gfx::IntRect viewport_rect)
{
    schedule_present_frame(context_id, context, ContextState::PendingFrame {
                                                    .viewport_rect = viewport_rect,
                                                    .damage_rect = { {}, viewport_rect.size() },
                                                });
}

void CompositorState::schedule_pending_present_frame(Web::Compositor::CompositorContextId context_id, ContextState& context)
{
    if (!context.presents_to_client()) {
        schedule_containing_context_present(context);
        // A nested context is presented through its containing context, but its
        // own async animations still need a vsync source. The containing frame
        // may already be up to date (and therefore not schedule a new present),
        // so explicitly keep the effective display's scheduler ticking while
        // a nested smooth scroll is active.
        if (context.has_active_smooth_scroll_animations() && context_is_effectively_visible(context))
            vsync_scheduler_for_display(display_id_for_context(context)).schedule(display_refresh_rate_for_context(context));
        return;
    }

    schedule_pending_present_frame_on_vsync(context_id, context);
}

void CompositorState::schedule_pending_present_frame_on_vsync(Web::Compositor::CompositorContextId, ContextState& context)
{
    if (!context_is_effectively_visible(context))
        return;
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
    update_video_sinks_for_display(display_id);

    auto now = MonotonicTime::now();
    for (auto& context_entry : m_contexts) {
        auto context_id = context_entry.key;
        auto& context = *context_entry.value;
        if (!context_is_effectively_visible(context)) {
            context.unschedule_pending_present_frame();
            continue;
        }
        auto has_active_smooth_scroll_animation_on_display = context.has_active_smooth_scroll_animations() && display_id_for_context(context) == display_id;
        if (!context.has_pending_present_frame_scheduled_on(display_id) && !has_active_smooth_scroll_animation_on_display)
            continue;

        if (auto animation_frame = context.advance_smooth_scroll_animations(now); animation_frame.has_value())
            context.queue_present_frame({
                .viewport_rect = *animation_frame,
                .damage_rect = { {}, animation_frame->size() },
            });

        auto pending_present_frame = context.take_pending_present_frame_if_unblocked();
        if (!pending_present_frame.has_value()) {
            has_active_smooth_scroll_animation_on_display = context.has_active_smooth_scroll_animations() && display_id_for_context(context) == display_id;
            if (context.has_pending_present_frame_scheduled_on(display_id) || has_active_smooth_scroll_animation_on_display)
                vsync_scheduler_for_display(display_id).schedule(display_refresh_rate_for_context(context));
            continue;
        }
        if (context.has_active_smooth_scroll_animations())
            schedule_present_frame(context_id, context, pending_present_frame->viewport_rect);
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
    auto damage_rect = pending_present.damage_rect;
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
        m_client->did_present_frame(context_id, viewport_rect, damage_rect, bitmap_id);
    }
    resize_backing_stores_if_needed(context_id, *context);
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
    if (auto publication = context.resize_backing_stores_if_needed(m_skia_backend_context, gpu_sharing_for_client()); publication.has_value()) {
        publish_backing_stores(context_id, context, publication.release_value());
        present_current_frame(context_id, context);
    }
}

BackingStoreManager::GpuSharing CompositorState::gpu_sharing_for_client() const
{
#ifdef USE_DIRECTX
    // Shared Direct3D textures can only be opened by the client if it presents on the same adapter.
    if (!m_skia_backend_context || m_client_gpu_presentation_adapter_luid != m_skia_backend_context->direct3d_context().adapter_luid())
        return BackingStoreManager::GpuSharing::Disallowed;
#endif
    return BackingStoreManager::GpuSharing::Allowed;
}

void CompositorState::set_client_gpu_presentation_capability(bool supported, u64 adapter_luid)
{
    Optional<u64> new_adapter_luid;
    if (supported)
        new_adapter_luid = adapter_luid;
    if (m_client_gpu_presentation_adapter_luid == new_adapter_luid)
        return;
    m_client_gpu_presentation_adapter_luid = new_adapter_luid;

    // Reallocate the backing stores of every presenting context so they match the new capability.
    for (auto& context_entry : m_contexts) {
        auto& context = *context_entry.value;
        if (!context.presents_to_client())
            continue;
        context.invalidate_backing_stores();
        resize_backing_stores_if_needed(context_entry.key, context);
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

    m_client->did_allocate_backing_stores(context_id, move(publication.bitmap_ids), move(publication.shared_images));
}

}
