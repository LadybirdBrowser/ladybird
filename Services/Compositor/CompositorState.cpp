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
#include <LibCore/StatsLog.h>
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

void CompositorState::register_presentation(CompositorStateClient& presentation, PresentationCapabilities capabilities)
{
    m_presentations.set(&presentation, capabilities);
}

void CompositorState::unregister_presentation(CompositorStateClient& presentation)
{
    Vector<Web::CompositorContextId> bound_context_ids;
    for (auto const& [context_id, bound_presentation] : m_context_presentations) {
        if (bound_presentation == &presentation)
            bound_context_ids.append(context_id);
    }
    for (auto context_id : bound_context_ids)
        unbind_presentation(context_id);
    m_presentations.remove(&presentation);
}

void CompositorState::bind_presentation(Web::CompositorContextId context_id, CompositorStateClient& presentation)
{
    VERIFY(m_presentations.contains(&presentation));
    auto* context = context_if_present(context_id);
    if (!context || !context->presents_to_client()) {
        dbgln("Compositor: Ignoring presentation binding for context {}, which is not a root context", context_id);
        return;
    }
    if (presentation_for_context(context_id) == &presentation)
        return;

    unbind_presentation(context_id);
    m_context_presentations.set(context_id, &presentation);
    update_scroll_capabilities();

    // The new presentation has seen none of the context's buffers, so it gets buffers of its own.
    context->presenter().invalidate_backing_stores();
    resize_backing_stores_if_needed(context_id, *context);
}

void CompositorState::unbind_presentation(Web::CompositorContextId context_id)
{
    if (!m_context_presentations.remove(context_id))
        return;
    auto* context = context_if_present(context_id);
    VERIFY(context);

    for (auto& pending_present : m_pending_async_presents) {
        if (pending_present.context_id == context_id)
            pending_present.publish_to_presentation = false;
    }
    // Nobody is left to release the buffers the presentation was showing.
    context->presenter().acknowledge_all_presented_bitmaps();
    update_scroll_capabilities();

    context->presenter().invalidate_backing_stores();
    resize_backing_stores_if_needed(context_id, *context);
    schedule_pending_present_frame_if_unblocked(context_id, *context);
}

CompositorStateClient* CompositorState::presentation_for_context(Web::CompositorContextId context_id) const
{
    return m_context_presentations.get(context_id).value_or(nullptr);
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
        m_canvas_surface_registry.set_canvas_surface(placeholder.owner->canvas_surface_owner_id(), canvas_id, *placeholder.surface);
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
        auto schedule_caret_repaint = [this, context_id](Gfx::IntRect damage_rect) {
            this->schedule_caret_repaint(context_id, damage_rect);
        };
        auto request_animation_frames = [this, context_id] {
            if (auto* context = context_if_present(context_id))
                schedule_animation_frames_if_needed(*context);
        };
        return make<ContextState>(context_id, page_id, web_content_client, OwnedCanvasSurfaces { m_canvas_surface_registry, web_content_client.canvas_surface_owner_id() }, move(schedule_caret_repaint), move(request_animation_frames));
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
    m_context_presentations.remove(context_id);
    m_contexts.remove(context_id);
    update_scroll_capabilities();
    update_unpainted_video_update_scheduling();
}

void CompositorState::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    if (parent_context_id.has_value()) {
        VERIFY(!context->presents_to_client());
        VERIFY(*parent_context_id != context_id);
        // The parent's context belongs to the process hosting the parent, which can destroy it before this context's
        // process hears that the parent is gone. The context is left without a parent then, as destroying the parent
        // would have left it.
        // NB: The UI process creates a context here before it hands the context's ID out, so a parent this doesn't know
        //     has been destroyed.
        if (!context_if_present(*parent_context_id))
            parent_context_id = {};
    }

    auto current_parent_context_id = context->parent_context_id();
    if (current_parent_context_id.has_value() == parent_context_id.has_value()
        && (!current_parent_context_id.has_value() || *current_parent_context_id == *parent_context_id))
        return;

    clear_parent_context(*context);
    context->set_parent_context(parent_context_id);
    update_scroll_capabilities();

    if (!parent_context_id.has_value())
        return;

    if (!context->presenter().latest_rendered_surface() && !context->presenter().needs_rasterization())
        return;

    auto* parent_context = context_if_present(*parent_context_id);
    VERIFY(parent_context);
    present_current_frame(*parent_context_id, *parent_context);
}

void CompositorState::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);
    unbind_presentation(context_id);
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

void CompositorState::create_video_edges(CompositorStateWebContentClient& client)
{
    auto client_sinks = m_video_sink_states.get(&client);
    if (!client_sinks.has_value())
        return;
    for (auto& sink_entry : *client_sinks)
        client.create_video_edge(sink_entry.key);
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
    if (sink_state->sink)
        sink_state->sink->set_ticking(should_tick);
    if (should_tick && sink_state->sink)
        present_contexts_drawing_video_sink(client, handle);
    update_unpainted_video_update_scheduling();
}

// The client decides this and we apply it, so that the two processes can never disagree about whether a sink is
// ticked. All we add is that a sink with no edge yet cannot be ticked, which can only delay ticking rather than
// stop it, and resolves as soon as the edge is created.
bool CompositorState::video_sink_updates_are_needed(VideoSinkState const& sink_state)
{
    return sink_state.sink != nullptr && sink_state.sink->is_ticking();
}

void CompositorState::on_video_sink_ready(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle, NonnullRefPtr<Media::DisplayingVideoSink> const& sink)
{
    auto* sink_state = video_sink_state(client, handle);
    if (!sink_state)
        return;
    sink_state->sink = sink;
    sink->set_ticking(sink_state->should_tick);
    sink->set_on_present_needed([this, &client, handle] {
        if (auto* sink_state = video_sink_state(client, handle); sink_state && sink_state->sink)
            client.video_sink_presented_frame(handle, sink_state->sink->current_frame(), sink_state->sink->take_frame_counts());
        present_contexts_drawing_video_sink(client, handle);
        update_unpainted_video_update_scheduling();
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
                if (auto rect = context.presenter().self_present_rect(); rect.has_value())
                    schedule_present_frame(context_entry.key, context, PendingFrame::repainting_changes(*rect));
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
        if (!context_is_effectively_visible(context) || !context.presenter().draws_canvas(canvas_id))
            continue;
        if (auto rect = context.presenter().self_present_rect(); rect.has_value())
            schedule_present_frame(context_entry.key, context, PendingFrame::repainting_changes(*rect));
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
        m_unpainted_video_update_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(unpainted_video_update_interval_ms()), [this] {
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
            Media::DisplayingVideoSinkFrameCounts frame_counts;
            if (sink_state.sink)
                frame_counts = sink_state.sink->take_frame_counts();
            if (!frame_counts.is_zero())
                client_entry.key->video_sink_counted_frames(sink_entry.key, frame_counts);
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

void CompositorState::handle_and_dispatch_key_event(Web::CompositorContextId context_id, Web::KeyEvent event, bool allow_keyboard_scroll)
{
    // NB: The presentation connection only passes on input for the contexts bound to it.
    auto* presentation = presentation_for_context(context_id);
    VERIFY(presentation);
    auto* context = context_if_present(context_id);
    if (!context || !context->can_dispatch_input_to_web_content()) {
        presentation->did_not_dispatch_input_event(context_id, event.id);
        return;
    }

    if (allow_keyboard_scroll) {
        if (event.type == Web::KeyEvent::Type::KeyDown && Web::is_keyboard_scroll_key(event.key, Web::UIEvents::Mod_None))
            dispatch_scroll_fling_step(context_id, *context, context->end_scroll_fling(), MonotonicTime::now());
        auto handled = apply_context_update_result(context_id, *context, context->handle_key_event(event));
        event.async_scroll_performed_default_action = handled && event.type == Web::KeyEvent::Type::KeyDown;
    }

    // The scroll updates the event produced reach WebContent ahead of the event itself on the same connection.
    publish_pending_async_scroll_updates(context_id, *context);
    context->dispatch_key_event_to_web_content(event);
    presentation->did_dispatch_input_event(context_id, event.id, event.async_scroll_performed_default_action);
}

void CompositorState::handle_and_dispatch_mouse_event(Web::CompositorContextId context_id, Web::MouseEvent event)
{
    // NB: The presentation connection only passes on input for the contexts bound to it.
    auto* presentation = presentation_for_context(context_id);
    VERIFY(presentation);
    auto* context = context_if_present(context_id);
    auto record_routing = [&](Web::InputRoutingDecision decision, Optional<Web::AsyncScrollNodeStableID> scroller = {}) {
        context->input_trace().record({ .input_event_id = event.id, .stage = Web::InputTraceStage::Compositor, .decision = decision, .scroller = scroller });
    };
    if (!context || !context->can_dispatch_input_to_web_content()) {
        if (context)
            record_routing(Web::InputRoutingDecision::NotDispatchedByCompositor);
        presentation->did_not_dispatch_input_event(context_id, event.id);
        return;
    }

    if (event.type == Web::MouseEvent::Type::MouseDown)
        dispatch_scroll_fling_step(context_id, *context, context->end_scroll_fling(), MonotonicTime::now());

    auto is_wheel_event = event.type == Web::MouseEvent::Type::MouseWheel;
    if (is_wheel_event) {
        context->note_wheel_event_from_ui(event);

        // The UI can send the end of a gesture more than once. An end that arrives during a fling belongs to the
        // flicked gesture, so it does not stop the fling.
        if (event.scroll_gesture_phase == Web::ScrollGesturePhase::Ended && context->has_active_scroll_fling()) {
            record_routing(Web::InputRoutingDecision::GestureEndAbsorbedByFling);
            presentation->did_consume_input_event(context_id, event.id);
            return;
        }

        // A new gesture must not continue the gesture of the fling, so the fling ends first.
        dispatch_scroll_fling_step(context_id, *context, context->interrupt_scroll_fling(event), MonotonicTime::now());

        // If the gesture continues as a fling, the end of the fling is the end of the gesture.
        if (m_presentations.get(presentation)->synthesizes_scroll_momentum && event.scroll_gesture_phase == Web::ScrollGesturePhase::Ended && context->start_scroll_fling_if_flicked(event)) {
            record_routing(Web::InputRoutingDecision::FlingStarted);
            presentation->did_consume_input_event(context_id, event.id);
            schedule_animation_frames_if_needed(*context);
            return;
        }
    }

    ContextState::ContextUpdateResult result;
    if (is_wheel_event)
        result = scroll_for_wheel_step(context_id, *context, event, WheelStepOrigin::UI);
    else
        result = context->handle_mouse_event(event);
    if (result.nested_context_hand_off.has_value())
        record_routing(*result.nested_context_hand_off);
    if (result.routing_decision.has_value()) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} routed input event {}: {}", context_id, event.id, Web::input_routing_decision_name(*result.routing_decision));
        record_routing(*result.routing_decision, result.routed_scroller);
    }

    // Schedules the present, publishes the scroll updates the event produced and asks for a rendering update, all of
    // which reach WebContent ahead of the event itself on the same connection.
    auto handled = apply_context_update_result(context_id, *context, result);
    // A wheel event that lets go of a stretched scroller presents nothing itself; the rebound does from here on. A press
    // beside the thumb of a scrollbar scrolls there in an animation too.
    schedule_animation_frames_if_needed(*context);
    if (is_wheel_event) {
        event.async_scroll_performed_default_action = handled;
    } else if (handled) {
        // The page still sees the events of a drag the compositor scrolls for a scrollbar the display list paints.
        if (!result.scrollbar_dragged_by_compositor.has_value()) {
            record_routing(Web::InputRoutingDecision::ConsumedByCompositor);
            presentation->did_consume_input_event(context_id, event.id);
            return;
        }
        event.scrollbar_dragged_by_compositor = result.scrollbar_dragged_by_compositor;
    }
    record_routing(Web::InputRoutingDecision::ForwardedToPage);
    context->dispatch_mouse_event_to_web_content(event, nested_context_scrolled_since_last_frame(context_id));
}

// Whether a context nested in the context `context_id` scrolled since the last frame its WebContent presented: the lane
// of the context's document hands a pointer move to the lane of a same-process iframe, which hit tests it in the iframe.
bool CompositorState::nested_context_scrolled_since_last_frame(Web::CompositorContextId context_id) const
{
    for (auto const& entry : m_contexts) {
        if (entry.value->scrolled_since_last_frame() && is_nested_in(context_id, *entry.value))
            return true;
    }
    return false;
}

bool CompositorState::is_nested_in(Web::CompositorContextId containing_context_id, ContextState const& context) const
{
    // A walk up the parents that outlasts the number of contexts went around a loop.
    auto const* ancestor = &context;
    for (size_t remaining_ancestors = m_contexts.size(); ancestor && remaining_ancestors > 0; --remaining_ancestors) {
        auto parent_context_id = ancestor->parent_context_id();
        if (!parent_context_id.has_value())
            return false;
        if (*parent_context_id == containing_context_id)
            return true;
        ancestor = context_if_present(*parent_context_id);
    }
    return false;
}

ContextState::ContextUpdateResult CompositorState::scroll_for_wheel_step(Web::CompositorContextId context_id, ContextState& context, Web::MouseEvent const& event, WheelStepOrigin origin, Optional<MonotonicTime> fling_step_time)
{
    auto now = fling_step_time.value_or(MonotonicTime::now());
    auto result = origin == WheelStepOrigin::UI ? context.handle_wheel_event(event) : context.scroll_for_fling_step(event, now);
    bool presents_frame = result.frame_to_present.has_value();
    if (result.nested_context_wheel_step.has_value()) {
        auto handed_step = hand_wheel_step_to_nested_context(context_id, context, event, *result.nested_context_wheel_step, now);
        result = move(handed_step.result);
        presents_frame = handed_step.nested_context_presents_frame;
    }

    // If the compositor accepts a momentum step of the fling but does not scroll, the scroller is at its edge.
    // Alternatively, a snap scroll uses the step. The next display tick ends the fling.
    if (origin == WheelStepOrigin::Fling && event.scroll_gesture_phase == Web::ScrollGesturePhase::Momentum && result.accepted && !presents_frame)
        context.end_scroll_fling_on_next_step();
    return result;
}

CompositorState::WheelStepHandedToNestedContext CompositorState::hand_wheel_step_to_nested_context(Web::CompositorContextId context_id, ContextState& context, Web::MouseEvent const& event, ContextState::NestedContextWheelStep const& nested_step, MonotonicTime now)
{
    auto step = ContextState::WheelStepFromContainingContext::for_wheel_event(event);
    // The nested context presents and publishes what its step changed, and this context reports how the step went.
    auto apply_result_of_nested_context = [&](Web::CompositorContextId nested_context_id, ContextState& nested_context, ContextState::ContextUpdateResult const& nested_result) {
        apply_context_update_result(nested_context_id, nested_context, nested_result);
        schedule_animation_frames_if_needed(nested_context);
        // Node ids are only unique within the process that made them.
        bool ids_are_of_this_process = nested_context.is_owned_by(context.web_content_client());
        return WheelStepHandedToNestedContext {
            .result = {
                .accepted = nested_result.accepted,
                .routing_decision = nested_result.routing_decision,
                .routed_scroller = ids_are_of_this_process ? nested_result.routed_scroller : Optional<Web::AsyncScrollNodeStableID> {},
            },
            .nested_context_presents_frame = nested_result.frame_to_present.has_value(),
        };
    };

    if (!nested_step.hit.has_value()) {
        auto* nested_context = context_if_present(nested_step.context_id);
        if (!nested_context || !is_nested_in(context_id, *nested_context)) {
            context.forget_wheel_gesture_latch();
            return { ContextState::ContextUpdateResult::declined(Web::InputRoutingDecision::NestedContextUnavailable) };
        }
        auto nested_result = nested_context->take_latched_wheel_step_from_containing_context(step, now);
        // A step that the nested context leaves to the main thread for now, as a new blocking listener may cancel it,
        // does not end the gesture there.
        if (nested_result.routing_decision == Web::InputRoutingDecision::NestedContextUnavailable)
            context.forget_wheel_gesture_latch();
        return apply_result_of_nested_context(nested_step.context_id, *nested_context, nested_result);
    }

    auto taking_context = nested_context_taking_first_wheel_step(context_id, *nested_step.hit, step.delta_in_css_pixels, step.wheel_delta_precision);
    if (!taking_context.has_value()) {
        auto result = context.scroll_for_wheel_event_passed_on_by_nested_context(event, now);
        result.nested_context_hand_off = Web::InputRoutingDecision::NestedContextPassedStepOn;
        return { .result = result, .nested_context_presents_frame = result.frame_to_present.has_value() };
    }

    WheelStepHandedToNestedContext handed_step;
    if (taking_context->context && taking_context->route.kind != ContextState::FirstWheelStepRoute::Kind::Declined) {
        step.position = taking_context->position;
        auto nested_result = taking_context->context->take_first_wheel_step_from_containing_context(taking_context->route, step, now);
        handed_step = apply_result_of_nested_context(taking_context->context_id, *taking_context->context, nested_result);
        context.latch_wheel_gesture_to_nested_context(taking_context->context_id, handed_step.result.routed_scroller, event, now);
    } else {
        handed_step.result = ContextState::ContextUpdateResult::declined(taking_context->route.decision);
    }
    handed_step.result.nested_context_hand_off = Web::InputRoutingDecision::HandedToNestedContext;
    return handed_step;
}

// The content box of a nested context is drawn scaled to the size of its viewport.
static Optional<Gfx::FloatPoint> position_in_nested_context(Compositing::NestedContextWheelHit const& hit, ContextState const& nested_context)
{
    auto viewport_size = nested_context.presenter().viewport_size().to_type<float>();
    if (hit.content_box_size.is_empty() || viewport_size.is_empty())
        return {};
    return Gfx::FloatPoint {
        hit.position_in_content_box.x() * viewport_size.width() / hit.content_box_size.width(),
        hit.position_in_content_box.y() * viewport_size.height() / hit.content_box_size.height(),
    };
}

Optional<CompositorState::NestedContextTakingWheelStep> CompositorState::nested_context_taking_first_wheel_step(Web::CompositorContextId context_id, Compositing::NestedContextWheelHit hit, Gfx::FloatPoint delta_in_css_pixels, Web::WheelDeltaPrecision wheel_delta_precision)
{
    using Kind = ContextState::FirstWheelStepRoute::Kind;
    // The contexts the step went into, outermost first, each nested in the one before.
    Vector<NestedContextTakingWheelStep, 4> contexts;
    auto containing_context_id = context_id;
    while (true) {
        // WebContent names the contexts its display lists draw, so a context only reaches those nested in it.
        auto* nested_context = context_if_present(hit.child_context_id);
        if (!nested_context || nested_context->parent_context_id() != containing_context_id)
            return NestedContextTakingWheelStep { .context_id = hit.child_context_id, .route = { Kind::Declined, Web::InputRoutingDecision::NestedContextUnavailable } };
        auto position = position_in_nested_context(hit, *nested_context);
        if (!position.has_value())
            return NestedContextTakingWheelStep { .context_id = hit.child_context_id, .route = { Kind::Declined, Web::InputRoutingDecision::NestedContextUnavailable } };
        auto route = nested_context->route_first_wheel_step_from_containing_context(*position, delta_in_css_pixels, wheel_delta_precision, Compositing::NestedContextRegions::HandToNestedContext);
        contexts.append({ .context_id = hit.child_context_id, .context = nested_context, .position = *position, .route = route });
        if (route.kind != Kind::NestedContext)
            break;
        containing_context_id = hit.child_context_id;
        hit = *route.nested_context_hit;
    }

    // What a nested context passes on goes to whatever is painted under it in the context it is nested in.
    while (contexts.last().route.kind == Kind::PassedOn) {
        contexts.take_last();
        if (contexts.is_empty())
            return {};
        auto& containing_context = contexts.last();
        containing_context.route = containing_context.context->route_first_wheel_step_from_containing_context(containing_context.position, delta_in_css_pixels, wheel_delta_precision, Compositing::NestedContextRegions::Ignore);
    }
    return contexts.take_last();
}

void CompositorState::dispatch_scroll_fling_step(Web::CompositorContextId context_id, ContextState& context, Optional<Web::MouseEvent> event, MonotonicTime now)
{
    // Send each fling step on the same route as a wheel event from the UI.
    if (!event.has_value())
        return;
    auto result = scroll_for_wheel_step(context_id, context, *event, WheelStepOrigin::Fling, now);
    if (result.nested_context_hand_off.has_value())
        context.input_trace().record({ .input_event_id = event->id, .stage = Web::InputTraceStage::Compositor, .decision = *result.nested_context_hand_off });
    if (result.routing_decision.has_value())
        context.input_trace().record({ .input_event_id = event->id, .stage = Web::InputTraceStage::Compositor, .decision = *result.routing_decision, .scroller = result.routed_scroller });
    event->async_scroll_performed_default_action = apply_context_update_result(context_id, context, result);
    schedule_animation_frames_if_needed(context);
    context.dispatch_mouse_event_to_web_content(*event);
}

void CompositorState::handle_pinch_event(Web::CompositorContextId context_id, Web::PinchEvent const& event)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    apply_context_update_result(context_id, *context, context->handle_pinch_event(event));
}

Compositing::AsyncScrollEnqueueResult CompositorState::async_scroll_by(Web::CompositorContextId context_id, Web::UniqueNodeID expected_document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, Compositing::AsyncScrollOperationTracking operation_tracking, Compositing::WheelStepDispatch wheel_step_dispatch, u64 input_event_id)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    auto result = context->async_scroll_by(expected_document_id, position, delta, viewport_rect, wheel_delta_precision, scroll_gesture_phase, modifiers, operation_tracking, wheel_step_dispatch, input_event_id);
    if (result.frame_to_present.has_value())
        schedule_present_frame(context_id, *context, *result.frame_to_present);
    schedule_animation_frames_if_needed(*context);
    // The process adopts the offsets a scroll moved in its next rendering update, which nothing else need prompt when
    // the context presents through another process's.
    if (result.enqueue_result.accepted)
        context->request_rendering_update();
    else
        publish_pending_async_scroll_updates(context_id, *context);
    return result.enqueue_result;
}

Compositing::AsyncScrollEnqueueResult CompositorState::smooth_scroll_to(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind animation_kind, Compositing::SmoothScrollInitiator initiator, Optional<Compositing::AsyncScrollOperationID> operation_to_continue)
{
    auto* context = context_if_present(context_id);
    VERIFY(context);

    auto result = context->smooth_scroll_to(stable_node_id, offset, main_thread_offset, viewport_rect, animation_kind, initiator, operation_to_continue);
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

void CompositorState::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    auto only_the_pixel_ratio_changed = viewport_size == context->presenter().viewport_size() && device_pixel_ratio != context->presenter().device_pixel_ratio();
    context->viewport_size_updated(viewport_size, device_pixel_ratio, window_resize_in_progress);
    resize_backing_stores_if_needed(context_id, *context);

    // The paused debugger's page will not paint again. Any other page's next frame damages the whole viewport when its
    // size changes, but a new pixel ratio alone need not damage the overlays.
    auto const& overlays = context->presenter().ui_overlays();
    if (overlays.paused_debugger.has_value() || (only_the_pixel_ratio_changed && overlays.picture_in_picture_controls.has_value())) {
        if (auto viewport_rect = context->presenter().viewport_rect_for_ui_overlay(); viewport_rect.has_value())
            schedule_present_frame(context_id, *context, *viewport_rect);
    }
    if (context->presenter().should_shrink_backing_stores_after_resize())
        schedule_backing_store_shrink(context_id, *context);
}

void CompositorState::set_ui_overlays(Web::CompositorContextId context_id, Compositing::UIOverlays overlays)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;
    if (!context->set_ui_overlays(move(overlays)))
        return;

    if (auto viewport_rect = context->presenter().viewport_rect_for_ui_overlay(); viewport_rect.has_value())
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
            m_unpainted_video_update_timer->set_interval(AK::Duration::from_milliseconds(unpainted_video_update_interval_ms()));
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
        dispatch_scroll_fling_step(context_id, *context, context->end_scroll_fling(), MonotonicTime::now());
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

    if (auto frame_rect_to_present = root_context.presenter().frame_rect_to_repaint(); frame_rect_to_present.has_value())
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
    context->presenter().queue_present_frame({ viewport_rect, {} });
    if (try_present_frame_during_resize(context_id, *context))
        return;
    schedule_pending_present_frame(context_id, *context);
}

void CompositorState::present_frame(Web::CompositorContextId context_id, ContextState& context, PendingFrame pending_frame)
{
    auto composited_context_resolver = resolver_for(context_id);
    auto prepared_frame = context.prepare_frame(*m_display_list_player, pending_frame, &composited_context_resolver);
    if (!prepared_frame.has_value())
        return;

    m_pending_async_presents.append(context_id, pending_frame.viewport_rect, prepared_frame->damage_rect, prepared_frame->bitmap_id, prepared_frame->device_pixel_ratio);
    auto* pending_present = &m_pending_async_presents.last();
    pending_present->publish_to_presentation = presentation_for_context(context_id) != nullptr;

    auto& event_loop = Core::EventLoop::current();
    auto self = NonnullRefPtr { *this };
    m_display_list_player->flush_async(*prepared_frame->rendered_surface, [self = move(self), &event_loop, pending_present] {
        event_loop.deferred_invoke([self = move(self), pending_present] {
            self->did_finish_async_present(*pending_present);
        });
    });
    context.presenter().did_submit_prepared_frame(pending_frame.viewport_rect);
#ifdef AK_OS_MACOS
    // The window server's reads of an IOSurface wait for GPU work that is already scheduled to write it. Handing the
    // frame to the client before that work finishes keeps it ahead of the window server's deadline for the next refresh.
    if (context.presents_to_client() && m_skia_backend_context) {
        m_skia_backend_context->metal_context().when_committed_work_is_scheduled([self = NonnullRefPtr { *this }, &event_loop, pending_present]() mutable {
            event_loop.deferred_invoke([self = move(self), pending_present] {
                for (auto& entry : self->m_pending_async_presents) {
                    if (&entry == pending_present) {
                        self->deliver_rendered_frame(entry);
                        return;
                    }
                }
            });
        });
    }
#endif
    schedule_gpu_completion_check();
}

void CompositorState::schedule_present_frame(Web::CompositorContextId context_id, ContextState& context, PendingFrame pending_frame)
{
    context.presenter().queue_present_frame(pending_frame);
    schedule_pending_present_frame(context_id, context);
}

void CompositorState::schedule_present_frame(Web::CompositorContextId context_id, ContextState& context, Gfx::IntRect viewport_rect)
{
    schedule_present_frame(context_id, context, PendingFrame::repainting_everything(viewport_rect));
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
    context.presenter().mark_pending_present_frame_scheduled();
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
    if (!context.presenter().can_schedule_pending_present_frame_if_unblocked())
        return;

    if (try_present_frame_during_resize(context_id, context))
        return;
    schedule_pending_present_frame(context_id, context);
}

bool CompositorState::try_present_frame_during_resize(Web::CompositorContextId context_id, ContextState& context)
{
    if (!context.presenter().window_resize_in_progress() || !context.presents_to_client() || !context_is_effectively_visible(context))
        return false;
    auto pending_frame = context.presenter().take_pending_present_frame_if_unblocked();
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
    auto viewport_rect = context->presenter().viewport_rect_for_ui_overlay();
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
            context.presenter().unschedule_pending_present_frame();
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
                dispatch_scroll_fling_step(context_id, context, context.take_scroll_fling_step(frame_time), frame_time);
            if (auto animation_frame = context.advance_smooth_scroll_animations(frame_time); animation_frame.has_value())
                context.presenter().queue_present_frame(PendingFrame::repainting_changes(*animation_frame));
            if (auto rebound_frame = context.advance_elastic_overscroll_rebounds(frame_time); rebound_frame.has_value())
                context.presenter().queue_present_frame(PendingFrame::repainting_changes(*rebound_frame));
            if (auto fade_frame = context.presenter().advance_ui_overlay_fades(frame_time); fade_frame.has_value())
                context.presenter().queue_present_frame(*fade_frame);
            if (auto fade_frame = context.advance_scrollbar_fades(frame_time); fade_frame.has_value())
                context.presenter().queue_present_frame(*fade_frame);
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
            if (auto viewport_rect = context.presenter().viewport_rect_for_ui_overlay(); viewport_rect.has_value())
                context.presenter().queue_present_frame(PendingFrame::repainting_changes(*viewport_rect));
        }

        add_backing_store_for_pending_frame_if_needed(context_id, context);
        auto pending_present_frame = context.presenter().take_pending_present_frame_if_unblocked();
        if (!pending_present_frame.has_value()) {
            has_active_animation_on_display = context.needs_animation_frames() && display_id_for_context(context) == display_id;
            if (context.has_pending_present_frame_scheduled_on(display_id) || has_active_animation_on_display)
                vsync_scheduler_for_display(display_id).schedule(display_refresh_rate_for_context(context));
            continue;
        }
        if (context.needs_animation_frames())
            schedule_present_frame(context_id, context, PendingFrame::repainting_changes(pending_present_frame->viewport_rect));
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
    if (auto scrollbar_fade_frame = context->paint_screenshot(*m_display_list_player, target_bitmap, &composited_context_resolver); scrollbar_fade_frame.has_value())
        schedule_present_frame(context_id, *context, *scrollbar_fade_frame);
    return true;
}

void CompositorState::presented_bitmap_ready_to_paint(Web::CompositorContextId context_id, i32 bitmap_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    if (!context->presenter().acknowledge_presented_bitmap(bitmap_id))
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

    auto finished_present = pending_present;
    (void)m_pending_async_presents.remove(pending_present_iterator);
    if (m_pending_async_presents.is_empty() && m_gpu_completion_timer)
        m_gpu_completion_timer->stop();

    deliver_rendered_frame(finished_present);
}

void CompositorState::deliver_rendered_frame(PendingAsyncPresent& pending_present)
{
    if (pending_present.was_cancelled || exchange(pending_present.was_delivered, true))
        return;

    auto context_id = pending_present.context_id;
    auto* context = context_if_present(context_id);
    VERIFY(context);

    context->presenter().did_finish_gpu_present(pending_present.bitmap_id);
    auto* presentation = pending_present.publish_to_presentation ? presentation_for_context(context_id) : nullptr;
    if (context->presents_to_client() && !presentation) {
        // Nobody shows the frame, so it is released here, on another turn, as a presentation would release it.
        Core::deferred_invoke([self = NonnullRefPtr { *this }, context_id, bitmap_id = pending_present.bitmap_id] {
            self->presented_bitmap_ready_to_paint(context_id, bitmap_id);
        });
    } else if (context->presents_to_client()) {
        if (auto* log = Core::StatsLog::the(); log && context->page_id().has_value()) {
            auto const& damage = pending_present.damage_rect;
            JsonObject data;
            data.set("page_id"sv, *context->page_id());
            data.set("changed"sv, !damage.is_empty());
            data.set("damage_x"sv, damage.x());
            data.set("damage_y"sv, damage.y());
            data.set("damage_width"sv, damage.width());
            data.set("damage_height"sv, damage.height());
            log->event("frame"sv, move(data));
        }
        presentation->did_present_frame(context_id, pending_present.viewport_rect, pending_present.damage_rect, pending_present.bitmap_id, pending_present.device_pixel_ratio);
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
        m_gpu_completion_timer = Core::Timer::create_repeating(AK::Duration::from_milliseconds(gpu_completion_check_interval_ms), [this] {
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

CompositedContextSurface CompositorState::resolve_composited_context(Web::CompositorContextId parent_context_id, Web::CompositorContextId child_context_id, Gfx::FloatRect destination_rect, Gfx::FloatMatrix4x4 const& canvas_transform)
{
    auto* child_context = context_if_present(child_context_id);
    if (!child_context)
        return {};
    auto child_parent_context_id = child_context->parent_context_id();
    if (!child_parent_context_id.has_value() || *child_parent_context_id != parent_context_id)
        return {};

    if (child_context->presenter().update_composited_raster_transform(destination_rect, canvas_transform)) {
        auto publication = child_context->presenter().resize_backing_stores_if_needed(m_skia_backend_context, BackingStoreManager::GpuSharing::Disallowed, BackingStoreManager::PublicationMode::Unpublished);
        VERIFY(!publication.has_value());
    }

    if (child_context->presenter().needs_rasterization()) {
        auto composited_context_resolver = resolver_for(child_context_id);
        DisplayListPlayerSkia display_list_player { m_skia_backend_context };
        child_context->present_synchronously(display_list_player, &composited_context_resolver);
    }

    return child_context->presenter().composited_surface();
}

void CompositorState::resize_backing_stores_if_needed(Web::CompositorContextId context_id, ContextState& context)
{
    auto publication_mode = presentation_for_context(context_id) ? BackingStoreManager::PublicationMode::Published : BackingStoreManager::PublicationMode::Unpublished;
    if (auto publication = context.presenter().resize_backing_stores_if_needed(m_skia_backend_context, gpu_sharing_for_context(context_id), publication_mode); publication.has_value()) {
        publish_backing_stores(context_id, context, publication.release_value());
        present_current_frame(context_id, context);
    }
}

BackingStoreManager::GpuSharing CompositorState::gpu_sharing_for_context([[maybe_unused]] Web::CompositorContextId context_id) const
{
#ifdef USE_DIRECTX
    // Shared Direct3D textures can only be opened by the presentation if it presents on the same adapter.
    auto* presentation = presentation_for_context(context_id);
    if (!presentation || !m_skia_backend_context || m_presentations.get(presentation)->gpu_presentation_adapter_luid != m_skia_backend_context->direct3d_context().adapter_luid())
        return BackingStoreManager::GpuSharing::Disallowed;
#endif
    return BackingStoreManager::GpuSharing::Allowed;
}

// A context scrolls elastically, and takes presses on scrollbar tracks, as the presentation of its root context does.
void CompositorState::update_scroll_capabilities()
{
    for (auto& [context_id, context] : m_contexts) {
        auto* presentation = presentation_for_context(root_context_of(*context)->context_id());
        context->set_scrolls_elastically(presentation && m_presentations.get(presentation)->scrolls_elastically);
        context->set_scrollbar_track_press_jumps(presentation && m_presentations.get(presentation)->scrollbar_track_press_jumps);
    }
}

void CompositorState::set_scrollbar_track_press_jumps(CompositorStateClient& presentation, bool jumps)
{
    m_presentations.get(&presentation)->scrollbar_track_press_jumps = jumps;
    update_scroll_capabilities();
}

String CompositorState::scroll_state_for_testing(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stretched_scroller) const
{
    auto const* context = context_if_present(context_id);
    if (!context)
        return {};
    return context->scroll_state_for_testing(stretched_scroller);
}

Optional<Compositing::FirstWheelStepRouteForTesting> CompositorState::route_first_wheel_step_for_testing(Web::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return {};
    if (!context->presents_to_client() || !context->may_hand_first_wheel_steps_to_nested_contexts())
        return context->route_first_wheel_step_for_testing(position, delta_in_css_pixels, Compositing::NestedContextRegions::LeaveToMainThread);
    auto hit = context->nested_context_for_wheel_at(position);
    if (!hit.has_value())
        return context->route_first_wheel_step_for_testing(position, delta_in_css_pixels, Compositing::NestedContextRegions::HandToNestedContext);
    auto taking_context = nested_context_taking_first_wheel_step(context_id, *hit, delta_in_css_pixels, Web::WheelDeltaPrecision::Precise);
    if (!taking_context.has_value())
        return context->route_first_wheel_step_for_testing(position, delta_in_css_pixels, Compositing::NestedContextRegions::Ignore);
    // Node ids are only unique within the process that made them.
    bool ids_are_of_this_process = taking_context->context && taking_context->context->is_owned_by(context->web_content_client());
    Compositing::FirstWheelStepRouteForTesting route { .route = { taking_context->route.decision }, .document_id = {} };
    if (ids_are_of_this_process) {
        route.route.scroller = taking_context->route.scroller;
        route.document_id = taking_context->context->document_id();
    }
    return route;
}

void CompositorState::begin_input_trace_for_testing(Web::CompositorContextId context_id)
{
    if (auto* context = context_if_present(context_id))
        context->input_trace().begin();
}

Vector<Web::InputTraceRecord> CompositorState::take_input_trace_for_testing(Web::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return {};
    return context->input_trace().take();
}

void CompositorState::set_client_gpu_presentation_capability(CompositorStateClient& presentation, bool supported, u64 adapter_luid)
{
    Optional<u64> new_adapter_luid;
    if (supported)
        new_adapter_luid = adapter_luid;
    auto& presentation_adapter_luid = m_presentations.get(&presentation)->gpu_presentation_adapter_luid;
    if (presentation_adapter_luid == new_adapter_luid)
        return;
    presentation_adapter_luid = new_adapter_luid;

    // Reallocate the backing stores of the presentation's contexts so they match the new capability.
    for (auto& context_entry : m_contexts) {
        auto& context = *context_entry.value;
        if (presentation_for_context(context_entry.key) != &presentation)
            continue;
        context.presenter().invalidate_backing_stores();
        resize_backing_stores_if_needed(context_entry.key, context);
    }
}

void CompositorState::schedule_backing_store_shrink(Web::CompositorContextId context_id, ContextState& context)
{
    context.presenter().schedule_backing_store_shrink([this, context_id] {
        shrink_backing_stores_after_resize(context_id);
    });
}

void CompositorState::shrink_backing_stores_after_resize(Web::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    context->presenter().finish_window_resize();
    resize_backing_stores_if_needed(context_id, *context);
}

void CompositorState::add_backing_store_for_pending_frame_if_needed(Web::CompositorContextId context_id, ContextState& context)
{
    auto* presentation = presentation_for_context(context_id);
    if (!presentation)
        return;
    auto publication = context.presenter().add_backing_store_for_pending_frame_if_needed(m_skia_backend_context);
    if (!publication.has_value())
        return;

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} gained backing store {}, as the window server still reads every released one", context_id, publication->bitmap_ids);
    presentation->did_add_backing_stores(context_id, move(publication->bitmap_ids), move(publication->shared_images));
    schedule_surplus_backing_store_retirement(context_id, context);
}

void CompositorState::schedule_surplus_backing_store_retirement(Web::CompositorContextId context_id, ContextState& context)
{
    context.presenter().schedule_surplus_backing_store_retirement([this, context_id] {
        retire_idle_surplus_backing_stores(context_id);
    });
}

void CompositorState::retire_idle_surplus_backing_stores(Web::CompositorContextId context_id)
{
    auto* context = context_if_present(context_id);
    if (!context)
        return;

    auto retired_bitmap_ids = context->presenter().retire_idle_surplus_backing_stores();
    if (auto* presentation = presentation_for_context(context_id); presentation && !retired_bitmap_ids.is_empty()) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} retired idle backing stores {}", context_id, retired_bitmap_ids);
        presentation->did_retire_backing_stores(context_id, move(retired_bitmap_ids));
    }
    if (context->presenter().has_surplus_backing_stores())
        schedule_surplus_backing_store_retirement(context_id, *context);
}

void CompositorState::present_current_frame(Web::CompositorContextId context_id, ContextState& context)
{
    if (auto frame_to_present = context.presenter().frame_rect_to_repaint(); frame_to_present.has_value())
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
    auto* presentation = presentation_for_context(context_id);
    VERIFY(presentation);
    VERIFY(context.presents_to_client());

    presentation->did_allocate_backing_stores(context_id, move(publication.bitmap_ids), move(publication.shared_images));
}

}
