/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Random.h>
#include <AK/StdLibExtras.h>
#include <Compositor/CanvasHost.h>
#include <Compositor/CompositorState.h>
#include <LibCore/EventLoop.h>
#include <LibCore/Timer.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>

namespace Compositor {

static constexpr int gpu_completion_check_interval_ms = 1;

NonnullRefPtr<CompositorState> CompositorState::create(RefPtr<Gfx::SkiaBackendContext> skia_backend_context)
{
    return adopt_ref(*new CompositorState(move(skia_backend_context)));
}

CompositorState::CompositorState(RefPtr<Gfx::SkiaBackendContext> skia_backend_context)
    : m_skia_backend_context(move(skia_backend_context))
    , m_display_list_player(make<DisplayListPlayerSkia>(m_skia_backend_context))
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

CompositorState::ContextOwnerCheckResult CompositorState::check_context_owner(Web::CompositorContextId context_id, CompositorStateWebContentClient& client)
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
    Vector<Web::CompositorContextId> context_ids;
    for (auto& context : m_contexts) {
        if (context.value->is_owned_by(client))
            context_ids.append(context.key);
    }

    for (auto context_id : context_ids) {
        destroy_context(context_id);
    }

    m_video_sink_states.remove(&client);

    m_placeholder_canvases.remove_all_matching([&](auto canvas_id, auto const& placeholder) {
        if (placeholder.owner != &client)
            return false;
        m_canvas_surface_registry.remove_canvas_surface(canvas_id);
        return true;
    });
}

CompositorState::PlaceholderCanvasAllocation CompositorState::allocate_placeholder_canvas(CompositorStateWebContentClient& client)
{
    auto canvas_id = m_canvas_surface_registry.allocate_canvas_id();
    auto secret = get_random<u64>();
    m_placeholder_canvases.set(canvas_id, { .owner = &client, .secret = secret, .surface = nullptr, .size = {}, .origin_clean = true });
    return { canvas_id, secret };
}

void CompositorState::release_placeholder_canvas(CompositorStateWebContentClient& client, Compositing::CanvasId canvas_id)
{
    auto it = m_placeholder_canvases.find(canvas_id);
    if (it == m_placeholder_canvases.end() || it->value.owner != &client)
        return;
    m_placeholder_canvases.remove(it);
    m_canvas_surface_registry.remove_canvas_surface(canvas_id);
}

void CompositorState::commit_placeholder_canvas(Compositing::CanvasId canvas_id, u64 secret, RefPtr<Gfx::PaintingSurface> source_surface, Gfx::IntSize size, bool origin_clean)
{
    auto it = m_placeholder_canvases.find(canvas_id);
    if (it == m_placeholder_canvases.end() || it->value.secret != secret)
        return;

    auto& placeholder = it->value;
    if (source_surface) {
        if (!placeholder.surface || placeholder.surface->size() != source_surface->size())
            placeholder.surface = Gfx::PaintingSurface::create_with_size(source_surface->size(), Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, m_skia_backend_context);
        placeholder.surface->copy_from_surface(*source_surface);
        m_canvas_surface_registry.set_canvas_surface(canvas_id, *placeholder.surface);
    } else {
        placeholder.surface = nullptr;
        m_canvas_surface_registry.remove_canvas_surface(canvas_id);
    }
    present_contexts_drawing_canvas(*placeholder.owner, canvas_id);

    if (placeholder.size == size && placeholder.origin_clean == origin_clean)
        return;
    placeholder.size = size;
    placeholder.origin_clean = origin_clean;
    placeholder.owner->placeholder_canvas_committed(canvas_id, size, origin_clean);
}

CompositorState::PlaceholderCanvasPixels CompositorState::read_placeholder_canvas_pixels(CompositorStateWebContentClient& client, Compositing::CanvasId canvas_id, Gfx::IntRect rect)
{
    auto it = m_placeholder_canvases.find(canvas_id);
    if (it == m_placeholder_canvases.end() || it->value.owner != &client)
        return {};
    auto& placeholder = it->value;
    if (!placeholder.surface)
        return { .pixels = {}, .origin_clean = placeholder.origin_clean };
    return { .pixels = CanvasHost::read_back_surface(*placeholder.surface, rect), .origin_clean = placeholder.origin_clean };
}

void CompositorState::create_context(Web::CompositorContextId context_id, Optional<u64> page_id, CompositorStateWebContentClient& web_content_client)
{
    VERIFY(!m_contexts.contains(context_id));
    if (page_id.has_value())
        VERIFY(context_id == Web::compositor_context_id_for_page(*page_id));

    auto& context = *m_contexts.ensure(context_id, [&] {
        return make<ContextState>(context_id, page_id, web_content_client, m_canvas_surface_registry, [this, context_id](Gfx::IntRect damage_rect) {
            schedule_caret_repaint(context_id, damage_rect);
        });
    });
    resize_backing_stores_if_needed(context_id, context);
}

void CompositorState::destroy_context(Web::CompositorContextId context_id)
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

void CompositorState::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
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

void CompositorState::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->stop_presenting_to_client();
}

void CompositorState::update_display_list(Web::CompositorContextId context_id, NonnullRefPtr<Compositing::DisplayList> display_list, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction, Compositing::ScrollStateSnapshot&& scroll_state_snapshot)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    if (display_list->compatible_visual_context_tree_structural_epoch() != visual_context_tree.structural_epoch()) {
        dbgln("Compositor: Dropping inconsistent display list update (display list epoch {}, tree epoch {})",
            display_list->compatible_visual_context_tree_structural_epoch(),
            visual_context_tree.structural_epoch());
        return;
    }
    if (auto validation = Compositing::validate_display_list_references_live_visual_context_nodes(*display_list, visual_context_tree); validation.is_error()) {
        dbgln("Compositor: Dropping display list update: {}", validation.error());
        return;
    }

    context->apply_display_list_resource_transaction(move(resource_transaction));
    context->install_display_list_update(move(display_list), move(visual_context_tree), move(scroll_state_snapshot));
    resolve_video_sinks(*context);

    update_unpainted_video_update_scheduling();
}

void CompositorState::update_display_list_resources(Web::CompositorContextId context_id, Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->apply_display_list_resource_transaction(move(resource_transaction));
}

void CompositorState::update_visual_context_tree(Web::CompositorContextId context_id, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    context->update_visual_context_tree(move(visual_context_tree), move(resource_transaction));
}

void CompositorState::update_scroll_state(Web::CompositorContextId context_id, Compositing::ScrollStateSnapshot&& scroll_state_snapshot, Compositing::KeyboardScrollState keyboard_scroll_state)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    context->update_scroll_state(move(scroll_state_snapshot), move(keyboard_scroll_state));
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
                if (auto rect = context.self_present_rect(); rect.has_value())
                    schedule_present_frame(context_entry.key, context, *rect);
                break;
            }
        }
    }
}

void CompositorState::present_contexts_drawing_canvas(CompositorStateWebContentClient& client, Compositing::CanvasId canvas_id)
{
    for (auto& context_entry : m_contexts) {
        auto& context = *context_entry.value;
        if (&context.web_content_client() != &client)
            continue;
        if (!context_is_effectively_visible(context) || !context.draws_canvas(canvas_id))
            continue;
        if (auto rect = context.self_present_rect(); rect.has_value())
            schedule_present_frame(context_entry.key, context, *rect);
    }
}

void CompositorState::resolve_video_sinks(ContextState& context)
{
    auto& client = context.web_content_client();
    for (auto const& resource_entry : context.video_sink_handles()) {
        auto* sink_state = video_sink_state(client, resource_entry.value);
        context.set_video_sink(Compositing::VideoSinkResourceId { resource_entry.key }, sink_state ? sink_state->sink : nullptr);
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
    return root_context_of(context)->visibility() == Compositing::ContextVisibility::Visible;
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

void CompositorState::invalidate_wheel_event_listener_state(Web::CompositorContextId context_id, u64 generation)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    context->invalidate_wheel_event_listener_state(generation);
}

void CompositorState::invalidate_keyboard_scroll_state(Web::CompositorContextId context_id, u64 generation)
{
    if (auto* context = context_if_present(context_id))
        context->invalidate_keyboard_scroll_state(generation);
}

bool CompositorState::handle_key_event(Web::CompositorContextId context_id, Web::KeyEvent const& event)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return false;
    if (event.type == Web::KeyEvent::Type::KeyDown && Web::is_keyboard_scroll_key(event.key, Web::UIEvents::Mod_None))
        dispatch_scroll_fling_step(context_id, *context, context->end_scroll_fling(MonotonicTime::now()));
    return apply_context_update_result(context_id, *context, context->handle_key_event(event));
}

bool CompositorState::dispatch_key_event_to_web_content(Web::CompositorContextId context_id, Web::KeyEvent const& event)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return false;
    publish_pending_async_scroll_updates(context_id, *context);
    context->dispatch_key_event_to_web_content(event);
    return true;
}

void CompositorState::handle_and_dispatch_mouse_event(Web::CompositorContextId context_id, Web::MouseEvent event)
{
    VERIFY(m_client);
    auto* context = context_if_present(context_id);
    if (!context || !context->can_dispatch_input_to_web_content()) {
        m_client->did_not_dispatch_input_event(context_id, event.id);
        return;
    }

    if (event.type == Web::MouseEvent::Type::MouseDown)
        dispatch_scroll_fling_step(context_id, *context, context->end_scroll_fling(MonotonicTime::now()));

    auto is_wheel_event = event.type == Web::MouseEvent::Type::MouseWheel;
    if (is_wheel_event) {
        // The UI can send the end of a gesture more than once. An end that arrives during a fling belongs to the
        // flicked gesture, so it does not stop the fling.
        if (event.scroll_gesture_phase == Web::ScrollGesturePhase::Ended && context->has_active_scroll_fling()) {
            m_client->did_consume_input_event(context_id, event.id);
            return;
        }

        // A new gesture must not continue the gesture of the fling, so the fling ends first.
        dispatch_scroll_fling_step(context_id, *context, context->interrupt_scroll_fling(event));

        // If the gesture continues as a fling, the end of the fling is the end of the gesture.
        if (m_synthesizes_scroll_momentum && event.scroll_gesture_phase == Web::ScrollGesturePhase::Ended && context->start_scroll_fling_if_flicked(event)) {
            m_client->did_consume_input_event(context_id, event.id);
            schedule_animation_frames_if_needed(*context);
            return;
        }
    }

    ContextState::ContextUpdateResult result;
    if (is_wheel_event)
        result = context->handle_wheel_event(event);
    else
        result = context->handle_mouse_event(event);

    // Schedules the present, publishes the scroll updates the event produced and asks for a rendering update, all of
    // which reach WebContent ahead of the event itself on the same connection.
    auto handled = apply_context_update_result(context_id, *context, result);
    if (is_wheel_event) {
        event.async_scroll_performed_default_action = handled;
    } else if (handled) {
        // The page still sees the events of a drag the compositor scrolls for a scrollbar the display list paints.
        if (!result.scrollbar_dragged_by_compositor.has_value()) {
            m_client->did_consume_input_event(context_id, event.id);
            return;
        }
        event.scrollbar_dragged_by_compositor = result.scrollbar_dragged_by_compositor;
    }
    context->dispatch_mouse_event_to_web_content(event);
}

void CompositorState::dispatch_scroll_fling_step(Web::CompositorContextId context_id, ContextState& context, Optional<ContextState::ScrollFlingStep> step)
{
    // Send each fling step on the same route as a wheel event from the UI.
    if (!step.has_value())
        return;
    step->event.async_scroll_performed_default_action = apply_context_update_result(context_id, context, step->result);
    context.dispatch_mouse_event_to_web_content(step->event);
}

void CompositorState::handle_pinch_event(Web::CompositorContextId context_id, Web::PinchEvent const& event)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    apply_context_update_result(context_id, *context, context->handle_pinch_event(event));
}

Compositing::AsyncScrollEnqueueResult CompositorState::async_scroll_by(Web::CompositorContextId context_id, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, Compositing::AsyncScrollOperationTracking operation_tracking)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    auto result = context->async_scroll_by(expected_document_id, position, delta, viewport_rect, wheel_delta_precision, scroll_gesture_phase, modifiers, operation_tracking);
    if (result.frame_to_present.has_value())
        schedule_present_frame(context_id, *context, *result.frame_to_present);
    // The process adopts the offsets a scroll moved in its next rendering update, which nothing else need prompt when
    // the context presents through another process's.
    if (result.enqueue_result.accepted)
        context->request_rendering_update();
    else
        publish_pending_async_scroll_updates(context_id, *context);
    return result.enqueue_result;
}

Compositing::AsyncScrollEnqueueResult CompositorState::smooth_scroll_to(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind animation_kind, Compositing::SmoothScrollInitiator initiator)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    auto result = context->smooth_scroll_to(stable_node_id, offset, main_thread_offset, viewport_rect, animation_kind, initiator);
    if (result.frame_to_present.has_value())
        schedule_present_frame(context_id, *context, *result.frame_to_present);
    publish_pending_async_scroll_updates(context_id, *context);
    return result.enqueue_result;
}

void CompositorState::cancel_smooth_scroll(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stable_node_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;
    context->cancel_smooth_scroll(stable_node_id);
    publish_pending_async_scroll_updates(context_id, *context);
}

Compositing::PendingAsyncScrollUpdates CompositorState::take_pending_async_scroll_updates(Web::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    return context->take_pending_async_scroll_updates();
}

void CompositorState::publish_pending_async_scroll_updates(Web::CompositorContextId context_id, ContextState& context)
{
    if (!context.has_pending_async_scroll_updates())
        return;
    context.web_content_client().async_scroll_updates(context_id, context.take_pending_async_scroll_updates());
}

void CompositorState::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    context->viewport_size_updated(viewport_size, window_resize_in_progress);
    resize_backing_stores_if_needed(context_id, *context);
    if (context->paused_debugger_overlay_visible()) {
        if (auto viewport_rect = context->viewport_rect_for_ui_overlay(); viewport_rect.has_value())
            schedule_present_frame(context_id, *context, *viewport_rect);
    }
    if (context->should_shrink_backing_stores_after_resize())
        schedule_backing_store_shrink(context_id, *context);
}

void CompositorState::set_paused_debugger_overlay(Web::CompositorContextId context_id, bool visible, double device_pixel_ratio, Optional<String> font_family, Optional<Compositing::PausedDebuggerOverlayAction> hovered_action)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;
    if (!context->set_paused_debugger_overlay(visible, device_pixel_ratio, move(font_family), hovered_action))
        return;

    if (auto viewport_rect = context->viewport_rect_for_ui_overlay(); viewport_rect.has_value())
        schedule_present_frame(context_id, *context, *viewport_rect);
}

void CompositorState::set_display_metadata(Web::CompositorContextId context_id, Optional<u64> display_id, double refresh_rate)
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

    if (context->display_tick_requested() && context_is_effectively_visible(*context))
        vsync_scheduler_for_display(display_id_for_context(*context)).schedule(display_refresh_rate_for_context(*context));
}

void CompositorState::set_context_visibility(Web::CompositorContextId context_id, Compositing::ContextVisibility visibility)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;
    if (visibility == Compositing::ContextVisibility::Hidden)
        dispatch_scroll_fling_step(context_id, *context, context->end_scroll_fling(MonotonicTime::now()));
    if (!context->set_visibility(visibility))
        return;

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} became {}", context_id, visibility == Compositing::ContextVisibility::Visible ? "visible" : "hidden");

    if (visibility == Compositing::ContextVisibility::Visible)
        resume_presentation_after_becoming_visible(context_id, *context);
}

void CompositorState::resume_presentation_after_becoming_visible(Web::CompositorContextId root_context_id, ContextState& root_context)
{
    for (auto& context_entry : m_contexts) {
        auto& context = *context_entry.value;
        if (root_context_of(context) != &root_context)
            continue;
        schedule_animation_frames_if_needed(context);
        if (context.display_tick_requested())
            vsync_scheduler_for_display(display_id_for_context(context)).schedule(display_refresh_rate_for_context(context));
    }

    if (auto frame_rect_to_present = root_context.frame_rect_to_repaint(); frame_rect_to_present.has_value())
        schedule_present_frame(root_context_id, root_context, *frame_rect_to_present);
}

void CompositorState::request_rendering_opportunity(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    if (!context->request_rendering_opportunity(maximum_frames_per_second))
        return;
    if (!context_is_effectively_visible(*context))
        return;

    auto display_id = display_id_for_context(*context);
    auto display_refresh_rate = display_refresh_rate_for_context(*context);
    auto& scheduler = vsync_scheduler_for_display(display_id);
    // INTEROP: Like Chromium's missed BeginFrame delivery, reuse a recent display tick when a context requests its
    //          next opportunity after the tick has already happened. This keeps heavy frames from waiting an extra tick.
    if (auto frame_time = scheduler.most_recent_tick_time(MonotonicTime::now(), display_refresh_rate);
        frame_time.has_value() && context->rendering_opportunity_is_due(*frame_time, display_refresh_rate)) {
        auto frame_interval = context->rendering_opportunity_frame_interval(display_refresh_rate);
        context->did_deliver_rendering_opportunity(*frame_time);
        context->web_content_client().rendering_opportunity(context_id, frame_time->nanoseconds(), frame_interval);
        return;
    }

    scheduler.schedule(display_refresh_rate);
}

void CompositorState::request_clock_tick(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    auto* context = context_if_present(context_id);
    if (!context || !context->request_clock_tick(maximum_frames_per_second) || !context_is_effectively_visible(*context))
        return;
    vsync_scheduler_for_display(display_id_for_context(*context)).schedule(display_refresh_rate_for_context(*context));
}

void CompositorState::hurry_rendering_opportunity(Web::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    if (!context || !context->rendering_opportunity_requested() || !context_is_effectively_visible(*context))
        return;
    auto display_refresh_rate = display_refresh_rate_for_context(*context);
    auto frame_interval = context->rendering_opportunity_frame_interval(display_refresh_rate);
    auto frame_time = MonotonicTime::now();
    context->did_deliver_rendering_opportunity(frame_time);
    context->web_content_client().rendering_opportunity(context_id, frame_time.nanoseconds(), frame_interval);
}

void CompositorState::present_frame(Web::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    // The frame joins whatever is queued, so a frame queued earlier cannot be presented after it.
    context->queue_present_frame({ viewport_rect, {} });
    if (try_present_frame_during_resize(context_id, *context))
        return;
    schedule_pending_present_frame(context_id, *context);
}

void CompositorState::present_frame(Web::CompositorContextId context_id, ContextState& context, ContextState::PendingFrame pending_frame)
{
    auto composited_context_resolver = resolver_for(context_id);
    auto prepared_frame = context.prepare_frame(*m_display_list_player, pending_frame, &composited_context_resolver);
    if (!prepared_frame.has_value())
        return;

    m_pending_async_presents.append(context_id, pending_frame.viewport_rect, prepared_frame->damage_rect, prepared_frame->bitmap_id);
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

void CompositorState::schedule_present_frame(Web::CompositorContextId context_id, ContextState& context, ContextState::PendingFrame pending_frame)
{
    context.queue_present_frame(pending_frame);
    schedule_pending_present_frame(context_id, context);
}

void CompositorState::schedule_present_frame(Web::CompositorContextId context_id, ContextState& context, Gfx::IntRect viewport_rect)
{
    schedule_present_frame(context_id, context, ContextState::PendingFrame::repainting_everything(viewport_rect));
}

void CompositorState::schedule_pending_present_frame(Web::CompositorContextId context_id, ContextState& context)
{
    if (!context.presents_to_client()) {
        schedule_containing_context_present(context);
        // A nested context is presented through its containing context, but its
        // own async animations still need a vsync source. The containing frame
        // may already be up to date (and therefore not schedule a new present),
        // so explicitly keep the effective display's scheduler ticking while
        // a nested animation is active.
        schedule_animation_frames_if_needed(context);
        return;
    }

    schedule_pending_present_frame_on_vsync(context_id, context);
}

void CompositorState::schedule_animation_frames_if_needed(ContextState& context)
{
    if (context.needs_animation_frames() && context_is_effectively_visible(context))
        vsync_scheduler_for_display(display_id_for_context(context)).schedule(display_refresh_rate_for_context(context));
}

void CompositorState::schedule_pending_present_frame_on_vsync(Web::CompositorContextId, ContextState& context)
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

void CompositorState::schedule_pending_present_frame_if_unblocked(Web::CompositorContextId context_id, ContextState& context)
{
    if (!context.can_schedule_pending_present_frame_if_unblocked())
        return;

    if (try_present_frame_during_resize(context_id, context))
        return;
    schedule_pending_present_frame(context_id, context);
}

bool CompositorState::try_present_frame_during_resize(Web::CompositorContextId context_id, ContextState& context)
{
    if (!context.window_resize_in_progress() || !context.presents_to_client() || !context_is_effectively_visible(context))
        return false;
    auto pending_frame = context.take_pending_present_frame_if_unblocked();
    if (!pending_frame.has_value())
        return false;

    // Rasterize resize frames as soon as a backing store is available, so the previous size does not
    // remain visible for another display tick. prepare_frame() owns requeuing any blocked frame.
    present_frame(context_id, context, *pending_frame);
    // The display tick does more than present frames, such as updating the video sinks that are painted.
    vsync_scheduler_for_display(display_id_for_context(context)).schedule(display_refresh_rate_for_context(context));
    return true;
}

void CompositorState::schedule_caret_repaint(Web::CompositorContextId context_id, Gfx::IntRect damage_rect)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;
    auto viewport_rect = context->viewport_rect_for_ui_overlay();
    if (!viewport_rect.has_value())
        return;
    schedule_present_frame(context_id, *context, { *viewport_rect, damage_rect });
}

VSyncScheduler& CompositorState::vsync_scheduler_for_display(Optional<u64> display_id)
{
    return *m_vsync_schedulers_by_display.ensure(display_id, [this, display_id] {
        return create_vsync_scheduler(display_id, [this, display_id](MonotonicTime frame_time) {
            present_pending_frames_on_vsync(display_id, frame_time);
        });
    });
}

void CompositorState::present_pending_frames_on_vsync(Optional<u64> display_id, MonotonicTime frame_time)
{
    update_video_sinks_for_display(display_id);

    for (auto& context_entry : m_contexts) {
        auto context_id = context_entry.key;
        auto& context = *context_entry.value;
        if (!context_is_effectively_visible(context)) {
            context.unschedule_pending_present_frame();
            continue;
        }

        if (context.rendering_opportunity_requested() && display_id_for_context(context) == display_id) {
            auto display_refresh_rate = display_refresh_rate_for_context(context);
            if (context.rendering_opportunity_is_due(frame_time, display_refresh_rate)) {
                auto frame_interval = context.rendering_opportunity_frame_interval(display_refresh_rate);
                context.did_deliver_rendering_opportunity(frame_time);
                context.web_content_client().rendering_opportunity(context_id, frame_time.nanoseconds(), frame_interval);
            } else {
                vsync_scheduler_for_display(display_id).schedule(display_refresh_rate);
            }
        }

        auto has_active_animation_on_display = context.needs_animation_frames() && display_id_for_context(context) == display_id;
        auto presents_on_display = context.has_pending_present_frame_scheduled_on(display_id) || has_active_animation_on_display;
        if (presents_on_display) {
            if (display_id_for_context(context) == display_id)
                dispatch_scroll_fling_step(context_id, context, context.take_scroll_fling_step(frame_time));
            if (auto animation_frame = context.advance_smooth_scroll_animations(frame_time); animation_frame.has_value())
                context.queue_present_frame(ContextState::PendingFrame::repainting_changes(*animation_frame));
        }

        // A render clock tick goes to the process's render clock thread, beside what its main thread is doing, with
        // where the compositor has scrolled to for this vsync, which that main thread may not have taken in yet.
        if (context.clock_tick_requested() && display_id_for_context(context) == display_id) {
            auto display_refresh_rate = display_refresh_rate_for_context(context);
            if (context.clock_tick_is_due(frame_time, display_refresh_rate)) {
                auto frame_interval = context.clock_tick_interval(display_refresh_rate);
                context.did_deliver_clock_tick(frame_time);
                context.web_content_client().clock_tick(context_id, frame_time.nanoseconds(), frame_interval, context.scroll_offsets());
            } else {
                vsync_scheduler_for_display(display_id).schedule(display_refresh_rate);
            }
        }

        if (!presents_on_display)
            continue;

        publish_pending_async_scroll_updates(context_id, context);
        if (context.visual_animations_need_frame()) {
            context.advance_visual_animations(frame_time);
            if (auto viewport_rect = context.viewport_rect_for_ui_overlay(); viewport_rect.has_value())
                context.queue_present_frame(ContextState::PendingFrame::repainting_changes(*viewport_rect));
        }

        add_backing_store_for_pending_frame_if_needed(context_id, context);
        auto pending_present_frame = context.take_pending_present_frame_if_unblocked();
        if (!pending_present_frame.has_value()) {
            has_active_animation_on_display = context.needs_animation_frames() && display_id_for_context(context) == display_id;
            if (context.has_pending_present_frame_scheduled_on(display_id) || has_active_animation_on_display)
                vsync_scheduler_for_display(display_id).schedule(display_refresh_rate_for_context(context));
            continue;
        }
        if (context.needs_animation_frames())
            schedule_present_frame(context_id, context, ContextState::PendingFrame::repainting_changes(pending_present_frame->viewport_rect));
        present_frame(context_id, context, *pending_present_frame);
    }
}

bool CompositorState::request_screenshot(Web::CompositorContextId context_id, Gfx::ShareableBitmap& target_bitmap)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    if (!context->can_paint_screenshot(target_bitmap))
        return false;

    auto composited_context_resolver = resolver_for(context_id);
    context->paint_screenshot(*m_display_list_player, target_bitmap, &composited_context_resolver);
    return true;
}

void CompositorState::presented_bitmap_ready_to_paint(Web::CompositorContextId context_id, i32 bitmap_id)
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

void CompositorState::cancel_pending_async_presents_for_context(Web::CompositorContextId context_id)
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

ContextState* CompositorState::context_if_present(Web::CompositorContextId context_id)
{
    auto it = m_contexts.find(context_id);
    if (it == m_contexts.end())
        return nullptr;
    return it->value.ptr();
}

ContextState const* CompositorState::context_if_present(Web::CompositorContextId context_id) const
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

CompositedContextResolver CompositorState::resolver_for(Web::CompositorContextId parent_context_id)
{
    return [this, parent_context_id](Web::CompositorContextId child_context_id, Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform) {
        return resolve_composited_context(parent_context_id, child_context_id, destination_rect, canvas_transform);
    };
}

Compositing::CompositedContextSurface CompositorState::resolve_composited_context(Web::CompositorContextId parent_context_id, Web::CompositorContextId child_context_id, Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform)
{
    auto* child_context = context_if_present(child_context_id);
    if (!child_context)
        return {};
    auto child_parent_context_id = child_context->parent_context_id();
    if (!child_parent_context_id.has_value() || *child_parent_context_id != parent_context_id)
        return {};

    if (child_context->update_composited_raster_transform(destination_rect, canvas_transform)) {
        auto publication = child_context->resize_backing_stores_if_needed(m_skia_backend_context, BackingStoreManager::GpuSharing::Disallowed);
        VERIFY(!publication.has_value());
    }

    if (child_context->needs_rasterization()) {
        auto composited_context_resolver = resolver_for(child_context_id);
        DisplayListPlayerSkia display_list_player { m_skia_backend_context };
        child_context->present_synchronously(display_list_player, &composited_context_resolver);
    }

    return child_context->composited_surface();
}

void CompositorState::resize_backing_stores_if_needed(Web::CompositorContextId context_id, ContextState& context)
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

void CompositorState::set_synthesizes_scroll_momentum(bool synthesizes_scroll_momentum)
{
    m_synthesizes_scroll_momentum = synthesizes_scroll_momentum;
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

void CompositorState::schedule_backing_store_shrink(Web::CompositorContextId context_id, ContextState& context)
{
    context.schedule_backing_store_shrink([this, context_id] {
        shrink_backing_stores_after_resize(context_id);
    });
}

void CompositorState::shrink_backing_stores_after_resize(Web::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    context->finish_window_resize();
    resize_backing_stores_if_needed(context_id, *context);
}

void CompositorState::add_backing_store_for_pending_frame_if_needed(Web::CompositorContextId context_id, ContextState& context)
{
    auto publication = context.add_backing_store_for_pending_frame_if_needed(m_skia_backend_context);
    if (!publication.has_value())
        return;

    VERIFY(m_client);
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} gained backing store {}, as the window server still reads every released one", context_id, publication->bitmap_ids);
    m_client->did_add_backing_stores(context_id, move(publication->bitmap_ids), move(publication->shared_images));
    schedule_surplus_backing_store_retirement(context_id, context);
}

void CompositorState::schedule_surplus_backing_store_retirement(Web::CompositorContextId context_id, ContextState& context)
{
    context.schedule_surplus_backing_store_retirement([this, context_id] {
        retire_idle_surplus_backing_stores(context_id);
    });
}

void CompositorState::retire_idle_surplus_backing_stores(Web::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    auto retired_bitmap_ids = context->retire_idle_surplus_backing_stores();
    if (!retired_bitmap_ids.is_empty() && context->presents_to_client()) {
        VERIFY(m_client);
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} retired idle backing stores {}", context_id, retired_bitmap_ids);
        m_client->did_retire_backing_stores(context_id, move(retired_bitmap_ids));
    }
    if (context->has_surplus_backing_stores())
        schedule_surplus_backing_store_retirement(context_id, *context);
}

void CompositorState::present_current_frame(Web::CompositorContextId context_id, ContextState& context)
{
    if (auto frame_to_present = context.frame_rect_to_repaint(); frame_to_present.has_value())
        schedule_present_frame(context_id, context, *frame_to_present);
}

bool CompositorState::apply_context_update_result(
    Web::CompositorContextId context_id,
    ContextState& context,
    ContextState::ContextUpdateResult const& result)
{
    if (result.frame_to_present.has_value())
        schedule_present_frame(context_id, context, *result.frame_to_present);
    publish_pending_async_scroll_updates(context_id, context);
    if (result.should_request_rendering_update)
        context.request_rendering_update();
    return result.accepted;
}

void CompositorState::publish_backing_stores(Web::CompositorContextId context_id, ContextState& context, BackingStoreManager::Publication&& publication)
{
    VERIFY(m_client);
    VERIFY(context.presents_to_client());

    m_client->did_allocate_backing_stores(context_id, move(publication.bitmap_ids), move(publication.shared_images));
}

}
