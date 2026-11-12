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

NonnullRefPtr<CompositorState> CompositorState::create(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, bool scrollbars_always_shown)
{
    return adopt_ref(*new CompositorState(move(skia_backend_context), scrollbars_always_shown));
}

CompositorState::CompositorState(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, bool scrollbars_always_shown)
    : m_skia_backend_context(move(skia_backend_context))
    , m_display_list_player(make<DisplayListPlayerSkia>(m_skia_backend_context))
    , m_core(*this, scrollbars_always_shown)
{
}

IPC::RawMessage::OutcomeInRust CompositorState::handle_message_from_web_content(WebContentClientId client_id, IPC::RawMessage& message)
{
    auto outcome = m_core.handle_message_from_web_content(client_id, message, MonotonicTime::now());
    schedule_requested_presents();
    return outcome;
}

IPC::RawMessage::OutcomeInRust CompositorState::handle_message_from_presentation(PresentationId presentation_id, IPC::RawMessage& message)
{
    auto outcome = m_core.handle_message_from_presentation(presentation_id, message, MonotonicTime::now());
    schedule_requested_presents();
    return outcome;
}

CompositorState::PresentedContext::PresentedContext(Optional<u64> page_id, CompositorStateWebContentClient& web_content_client, OwnedCanvasSurfaces canvas_surfaces)
    : page_id(page_id)
    , web_content_client(web_content_client)
    , presenter(canvas_surfaces, page_id.has_value())
{
}

CompositorState::~CompositorState()
{
    if (!m_gpu_completion_timer)
        return;
    m_gpu_completion_timer->on_timeout = {};
    m_gpu_completion_timer->stop();
}

WebContentClientId CompositorState::register_web_content_client(CompositorStateWebContentClient& client)
{
    WebContentClientId client_id { m_next_web_content_client_id++ };
    m_web_content_clients.set(client_id, &client);
    return client_id;
}

CompositorStateWebContentClient* CompositorState::web_content_client_if_present(WebContentClientId client_id) const
{
    return m_web_content_clients.get(client_id).value_or(nullptr);
}

void CompositorState::post_message_to_web_content(WebContentClientId client_id, IPC::MessageBuffer& buffer)
{
    if (auto* client = web_content_client_if_present(client_id))
        client->post_message_to_web_content(buffer);
}

void CompositorState::post_message_to_render_clock_of_web_content(WebContentClientId client_id, IPC::MessageBuffer& buffer)
{
    if (auto* client = web_content_client_if_present(client_id))
        client->post_message_to_render_clock(buffer);
}

void CompositorState::post_message_to_presentation(PresentationId presentation_id, IPC::MessageBuffer& buffer)
{
    if (auto* presentation = m_presentations_by_id.get(presentation_id).value_or(nullptr))
        presentation->post_message_to_presentation(buffer);
}

void CompositorState::request_display_ticks(Web::CompositorContextId context_id)
{
    if (auto* context = presented_context_if_present(context_id); context && context_is_effectively_visible(*context))
        vsync_scheduler_for_display(display_id_for_context(*context)).schedule(display_refresh_rate_for_context(*context));
}

void CompositorState::set_timer(Web::CompositorContextId context_id, ContextTimer timer, AK::Duration delay)
{
    auto& context_timer = m_context_timers.ensure(context_id).ensure(timer, [&] {
        return Core::Timer::create_single_shot(delay, [this, context_id, timer] {
            context_timer_fired(context_id, timer);
        });
    });
    context_timer->restart(delay);
}

void CompositorState::cancel_timer(Web::CompositorContextId context_id, ContextTimer timer)
{
    auto timers = m_context_timers.find(context_id);
    if (timers == m_context_timers.end())
        return;
    if (auto context_timer = timers->value.get(timer); context_timer.has_value())
        (*context_timer)->stop();
}

void CompositorState::context_timer_fired(Web::CompositorContextId context_id, ContextTimer timer)
{
    if (!presented_context_if_present(context_id))
        return;
    m_core.timer_fired(context_id, timer, MonotonicTime::now());
    schedule_requested_presents();
}

PresentationId CompositorState::register_presentation(CompositorStateClient& presentation, PresentationCapabilities capabilities)
{
    PresentationId presentation_id { m_next_presentation_id++ };
    m_presentations.set(&presentation, capabilities);
    m_presentations_by_id.set(presentation_id, &presentation);
    if (auto window_server = WindowServer::try_create_for_presentation())
        m_window_servers.set(&presentation, window_server.release_nonnull());
    return presentation_id;
}

void CompositorState::unregister_presentation(CompositorStateClient& presentation)
{
    Vector<u64> remote_layer_ids;
    for (auto const& [remote_layer_id, remote_layer] : m_remote_layers) {
        if (remote_layer.presentation == &presentation)
            remote_layer_ids.append(remote_layer_id);
    }
    for (auto remote_layer_id : remote_layer_ids)
        destroy_remote_layer(remote_layer_id);
    if (auto window_server = m_window_servers.take(&presentation); window_server.has_value()) {
        if (m_window_servers_holding_changes.remove_first_matching([&](auto const& holding) { return holding.ptr() == window_server->ptr(); }))
            (*window_server)->release_changes_held_off_screen();
    }

    Vector<Web::CompositorContextId> bound_context_ids;
    for (auto const& [context_id, bound_presentation] : m_context_presentations) {
        if (bound_presentation == &presentation)
            bound_context_ids.append(context_id);
    }
    for (auto context_id : bound_context_ids)
        unbind_presentation(context_id);
    m_presentations.remove(&presentation);
    m_presentations_by_id.remove(presentation.presentation_id());
}

void CompositorState::bind_presentation(Web::CompositorContextId context_id, CompositorStateClient& presentation)
{
    VERIFY(m_presentations.contains(&presentation));
    auto* context = presented_context_if_present(context_id);
    if (!context || !context->presenter.presents_to_client()) {
        dbgln("Compositor: Ignoring presentation binding for context {}, which is not a root context", context_id);
        return;
    }
    if (presentation_for_context(context_id) == &presentation)
        return;

    unbind_presentation(context_id);
    m_context_presentations.set(context_id, &presentation);
    m_core.set_bound_presentation(context_id, presentation.presentation_id());
    update_scroll_capabilities();

    // The new presentation has seen none of the context's buffers, so it gets buffers of its own.
    context->presenter.invalidate_backing_stores();
    resize_backing_stores_if_needed(context_id, *context);
}

void CompositorState::unbind_presentation(Web::CompositorContextId context_id)
{
    if (!m_context_presentations.remove(context_id))
        return;
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);
    m_core.set_bound_presentation(context_id, {});

    for (auto& pending_present : m_pending_async_presents) {
        if (pending_present.context_id == context_id)
            pending_present.publish_to_presentation = false;
    }
    // Nobody is left to release the buffers the presentation was showing.
    context->presenter.acknowledge_all_presented_bitmaps();
    update_scroll_capabilities();

    context->presenter.invalidate_backing_stores();
    resize_backing_stores_if_needed(context_id, *context);
    schedule_pending_present_frame_if_unblocked(context_id, *context);
}

u32 CompositorState::create_remote_layer(CompositorStateClient& presentation, u64 remote_layer_id)
{
    VERIFY(m_presentations.contains(&presentation));
    if (auto existing = m_remote_layers.find(remote_layer_id); existing != m_remote_layers.end()) {
        dbgln("Compositor: Ignoring creation of remote layer {}, which already exists", remote_layer_id);
        return 0;
    }
    auto window_server = m_window_servers.get(&presentation);
    if (!window_server.has_value())
        return 0;
    auto layer = (*window_server)->try_create_remote_layer(remote_layer_id);
    if (!layer)
        return 0;
    auto host_handle = layer->host_handle();
    VERIFY(host_handle.kind == Compositing::RemoteLayerHostKind::CoreAnimationContext);
    m_remote_layers.set(remote_layer_id, { .presentation = &presentation, .window_server = **window_server, .layer = move(layer), .shown_context_id = {}, .fit_within = {}, .size_of_shown_frame_laid_out_by_page = {} });
    return static_cast<u32>(host_handle.id);
}

void CompositorState::destroy_remote_layer(u64 remote_layer_id)
{
    show_context_in_remote_layer(remote_layer_id, {});
    m_remote_layers.remove(remote_layer_id);
    auto had_synchronized_resizes = !m_synchronized_resizes.is_empty();
    m_synchronized_resizes.remove_all_matching([&](auto const& resize) { return resize.remote_layer_id == remote_layer_id; });
    if (had_synchronized_resizes && m_synchronized_resizes.is_empty())
        release_synchronized_resize_hold();
}

void CompositorState::show_context_in_remote_layer(u64 remote_layer_id, Optional<Web::CompositorContextId> context_id)
{
    auto remote_layer = m_remote_layers.find(remote_layer_id);
    if (remote_layer == m_remote_layers.end())
        return;
    if (remote_layer->value.shown_context_id == context_id)
        return;

    remote_layer->value.size_of_shown_frame_laid_out_by_page = {};
    if (auto shown_context_id = exchange(remote_layer->value.shown_context_id, {}); shown_context_id.has_value()) {
        if (auto* shown_context = presented_context_if_present(*shown_context_id))
            stop_showing_context_in_remote_layer(*shown_context_id, *shown_context);
    }
    if (!context_id.has_value())
        return;

    auto* context = presented_context_if_present(*context_id);
    if (!context || !context->presenter.presents_to_client()) {
        dbgln("Compositor: Ignoring context {} for remote layer {}, as it is not a root context", *context_id, remote_layer_id);
        return;
    }
    if (context->remote_layer_id.has_value()) {
        if (auto other_remote_layer = m_remote_layers.find(*context->remote_layer_id); other_remote_layer != m_remote_layers.end()) {
            other_remote_layer->value.shown_context_id = {};
            other_remote_layer->value.size_of_shown_frame_laid_out_by_page = {};
        }
    }

    remote_layer->value.shown_context_id = context_id;
    context->remote_layer_id = remote_layer_id;
    stop_delivering_in_flight_frames_to_their_viewer(*context_id);
    // The presentation no longer shows the stores it was given, so none of them is waiting to be released by it.
    context->presenter.acknowledge_all_presented_bitmaps();
    context->presenter.invalidate_backing_stores();
    resize_backing_stores_if_needed(*context_id, *context);
    schedule_pending_present_frame_if_unblocked(*context_id, *context);
}

void CompositorState::set_remote_layer_fit(u64 remote_layer_id, Optional<Gfx::IntSize> fit_within)
{
    auto remote_layer = m_remote_layers.find(remote_layer_id);
    if (remote_layer == m_remote_layers.end() || remote_layer->value.fit_within == fit_within)
        return;
    remote_layer->value.fit_within = fit_within;
    remote_layer->value.layer->place_frame(fit_within);
}

// A UI that never commits its resize, as one that crashed, would otherwise freeze every window of the browser.
static constexpr auto SYNCHRONIZED_RESIZE_HOLD_LIMIT = AK::Duration::from_seconds(1);

void CompositorState::begin_synchronized_resize(u64 remote_layer_id, u64 token)
{
    auto remote_layer = m_remote_layers.find(remote_layer_id);
    if (remote_layer == m_remote_layers.end())
        return;
    auto& window_server = remote_layer->value.window_server;
    if (!m_window_servers_holding_changes.contains_slow(window_server)) {
        if (m_window_servers_holding_changes.is_empty()) {
            if (!m_synchronized_resize_hold_timeout) {
                m_synchronized_resize_hold_timeout = Core::Timer::create_single_shot(SYNCHRONIZED_RESIZE_HOLD_LIMIT, [this] {
                    dbgln("Compositor: Releasing the window resize that was never committed");
                    release_synchronized_resize_hold();
                });
            }
            m_synchronized_resize_hold_timeout->start();
        }
        m_window_servers_holding_changes.append(window_server);
        window_server->hold_changes_on_screen();
    }
    m_synchronized_resizes.append({ .remote_layer_id = remote_layer_id, .token = token, .awaited_size = {}, .frame_is_held = false });
}

void CompositorState::await_synchronized_resize_frame(u64 remote_layer_id, u64 token, Gfx::IntSize size)
{
    for (auto& resize : m_synchronized_resizes) {
        if (resize.remote_layer_id == remote_layer_id && resize.token == token)
            resize.awaited_size = size;
    }
    report_held_synchronized_resize_frames(remote_layer_id);
}

void CompositorState::report_held_synchronized_resize_frames(u64 remote_layer_id)
{
    auto remote_layer = m_remote_layers.find(remote_layer_id);
    if (remote_layer == m_remote_layers.end())
        return;
    auto shown_size = remote_layer->value.size_of_shown_frame_laid_out_by_page;
    for (auto& resize : m_synchronized_resizes) {
        if (resize.remote_layer_id != remote_layer_id || resize.frame_is_held || !resize.awaited_size.has_value() || resize.awaited_size != shown_size)
            continue;
        resize.frame_is_held = true;
        remote_layer->value.presentation->did_hold_synchronized_resize_frame(remote_layer_id, resize.token);
    }
}

void CompositorState::commit_synchronized_resize(u64 remote_layer_id, u64 token)
{
    m_synchronized_resizes.remove_all_matching([&](auto const& resize) {
        return resize.remote_layer_id == remote_layer_id && resize.token == token;
    });
    if (m_synchronized_resizes.is_empty())
        release_synchronized_resize_hold();
    if (auto remote_layer = m_remote_layers.find(remote_layer_id); remote_layer != m_remote_layers.end())
        remote_layer->value.presentation->did_commit_synchronized_resize(remote_layer_id, token);
}

void CompositorState::release_synchronized_resize_hold()
{
    if (m_synchronized_resize_hold_timeout)
        m_synchronized_resize_hold_timeout->stop();
    m_synchronized_resizes.clear();
    for (auto& window_server : exchange(m_window_servers_holding_changes, {}))
        window_server->release_changes_held_off_screen();
}

void CompositorState::stop_showing_context_in_remote_layer(Web::CompositorContextId context_id, PresentedContext& context)
{
    context.remote_layer_id = {};
    stop_delivering_in_flight_frames_to_their_viewer(context_id);
    context.presenter.invalidate_backing_stores();
    resize_backing_stores_if_needed(context_id, context);
    schedule_pending_present_frame_if_unblocked(context_id, context);
}

// The frames were drawn into stores of the way the context was shown before, so they are released without being shown.
void CompositorState::stop_delivering_in_flight_frames_to_their_viewer(Web::CompositorContextId context_id)
{
    for (auto& pending_present : m_pending_async_presents) {
        if (pending_present.context_id != context_id)
            continue;
        pending_present.publish_to_presentation = false;
        pending_present.shows_in_remote_layer = false;
    }
}

CompositorStateClient* CompositorState::presentation_for_context(Web::CompositorContextId context_id) const
{
    return m_context_presentations.get(context_id).value_or(nullptr);
}

CompositorState::ContextOwnerCheckResult CompositorState::check_context_owner(Web::CompositorContextId context_id, WebContentClientId client)
{
    switch (m_core.check_context_owner(context_id, client)) {
    case CompositorCore::ContextOwnerCheckResult::OwnedByClient:
        return ContextOwnerCheckResult::OwnedByClient;
    case CompositorCore::ContextOwnerCheckResult::ContextUnavailable:
        return ContextOwnerCheckResult::ContextUnavailable;
    case CompositorCore::ContextOwnerCheckResult::ConflictingOwner:
        return ContextOwnerCheckResult::ConflictingOwner;
    }
    VERIFY_NOT_REACHED();
}

void CompositorState::destroy_contexts_for_web_content_client(CompositorStateWebContentClient& client)
{
    Vector<Web::CompositorContextId> context_ids;
    for (auto& context : m_presented_contexts) {
        if (context.value->web_content_client.web_content_client_id() == client.web_content_client_id())
            context_ids.append(context.key);
    }

    for (auto context_id : context_ids) {
        destroy_context(context_id);
    }

    m_video_sink_states.remove(&client);
    m_animated_images.remove(&client);

    m_placeholder_canvases.remove_all_matching([&](auto canvas_id, auto const& placeholder) {
        if (placeholder.owner != &client)
            return false;
        m_canvas_surface_registry.remove_canvas_surface(canvas_id);
        return true;
    });

    // A process that goes away paints nothing more, and is told of nothing.
    m_screenshot_preparations.remove_all_matching([&](auto, auto const& preparation) {
        return preparation.requester == client.web_content_client_id();
    });
    Vector<u64> preparations_awaiting_client;
    for (auto& [preparation_id, preparation] : m_screenshot_preparations) {
        if (preparation.awaited_painters.remove(client.web_content_client_id()))
            preparations_awaiting_client.append(preparation_id);
    }
    m_web_content_clients.remove(client.web_content_client_id());
    for (auto preparation_id : preparations_awaiting_client)
        finish_screenshot_preparation_if_done(preparation_id);
}

CompositorState::PlaceholderCanvasAllocation CompositorState::allocate_placeholder_canvas(CompositorStateWebContentClient& client)
{
    auto canvas_id = m_canvas_surface_registry.allocate_canvas_id();
    auto secret = get_random<u64>();
    m_placeholder_canvases.set(canvas_id, { .owner = &client, .secret = secret, .presented_surfaces = {}, .surface = nullptr, .size = {}, .origin_clean = true });
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
        auto presented_surface_or_error = placeholder.presented_surfaces.surface_for_next_content(source_surface->size(), m_skia_backend_context);
        if (presented_surface_or_error.is_error()) {
            dbgln("Unable to commit a placeholder canvas: {}", presented_surface_or_error.error());
            return;
        }
        auto presented_surface = presented_surface_or_error.release_value();
        presented_surface->painting_surface().copy_from_surface(*source_surface);
        placeholder.surface = presented_surface->painting_surface();
        m_canvas_surface_registry.set_canvas_surface(placeholder.owner->canvas_surface_owner_id(), canvas_id, *presented_surface);
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
    m_core.create_context(context_id, page_id, web_content_client.web_content_client_id());
    auto& context = *m_presented_contexts.ensure(context_id, [&] {
        return make<PresentedContext>(page_id, web_content_client, OwnedCanvasSurfaces { m_canvas_surface_registry, web_content_client.canvas_surface_owner_id() });
    });
    context.presenter.set_window_server_may_show_high_dynamic_range(m_window_server_may_show_high_dynamic_range);
    resize_backing_stores_if_needed(context_id, context);
}

void CompositorState::destroy_context(Web::CompositorContextId context_id)
{
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);

    cancel_pending_async_presents_for_context(context_id);
    clear_parent_context(context_id, *context);
    for (auto& context_entry : m_presented_contexts) {
        if (context_entry.key == context_id)
            continue;
        auto& possible_child_context = *context_entry.value;
        auto parent_context_id = possible_child_context.parent_context_id;
        if (parent_context_id.has_value() && *parent_context_id == context_id)
            possible_child_context.parent_context_id = {};
    }
    m_context_presentations.remove(context_id);
    if (context->remote_layer_id.has_value()) {
        // The layer keeps the context's last frame until another context is shown in it.
        if (auto remote_layer = m_remote_layers.find(*context->remote_layer_id); remote_layer != m_remote_layers.end())
            remote_layer->value.shown_context_id = {};
    }
    if (auto timers = m_context_timers.find(context_id); timers != m_context_timers.end()) {
        for (auto& [_, timer] : timers->value) {
            timer->on_timeout = {};
            timer->stop();
        }
        m_context_timers.remove(timers);
    }
    m_core.destroy_context(context_id);
    m_presented_contexts.remove(context_id);
    update_scroll_capabilities();
    update_unpainted_video_update_scheduling();
}

// What a client shows is rooted at the context presented to it, and no context is nested in itself.
bool CompositorState::context_can_be_nested_in(Web::CompositorContextId context_id, Web::CompositorContextId parent_context_id) const
{
    auto const* context = presented_context_if_present(context_id);
    VERIFY(context);
    return !context->presenter.presents_to_client() && parent_context_id != context_id;
}

void CompositorState::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
{
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);

    if (parent_context_id.has_value()) {
        VERIFY(!context->presenter.presents_to_client());
        VERIFY(*parent_context_id != context_id);
        // The parent's context belongs to the process hosting the parent, which can destroy it before this context's
        // process hears that the parent is gone. The context is left without a parent then, as destroying the parent
        // would have left it.
        // NB: The UI process creates a context here before it hands the context's ID out, so a parent this doesn't know
        //     has been destroyed.
        if (!presented_context_if_present(*parent_context_id))
            parent_context_id = {};
    }

    auto current_parent_context_id = context->parent_context_id;
    if (current_parent_context_id.has_value() == parent_context_id.has_value()
        && (!current_parent_context_id.has_value() || *current_parent_context_id == *parent_context_id))
        return;

    clear_parent_context(context_id, *context);
    context->parent_context_id = parent_context_id;
    m_core.set_parent_context(context_id, parent_context_id);
    update_scroll_capabilities();

    if (!parent_context_id.has_value())
        return;

    if (!context->presenter.latest_rendered_surface() && !context->presenter.needs_rasterization())
        return;

    auto* parent_context = presented_context_if_present(*parent_context_id);
    VERIFY(parent_context);
    present_current_frame(*parent_context_id, *parent_context);
}

void CompositorState::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);
    unbind_presentation(context_id);
    m_core.stop_presenting_to_client(context_id);
    context->presenter.stop_presenting_to_client();
}

bool CompositorState::update_display_list(Web::CompositorContextId context_id, NonnullRefPtr<Compositing::DisplayList> display_list, IPC::Encoded<Optional<Compositing::DisplayList::AsyncScrollingMetadata>> const& async_scrolling_metadata, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction, ReadonlyBytes encoded_scroll_state)
{
    auto now = MonotonicTime::now();
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);

    if (display_list->compatible_visual_context_tree_structural_epoch() != visual_context_tree.structural_epoch()) {
        dbgln("Compositor: Dropping inconsistent display list update (display list epoch {}, tree epoch {})",
            display_list->compatible_visual_context_tree_structural_epoch(),
            visual_context_tree.structural_epoch());
        return true;
    }
    if (auto validation = Compositing::validate_display_list_references_live_visual_context_nodes(*display_list, visual_context_tree); validation.is_error()) {
        dbgln("Compositor: Dropping display list update: {}", validation.error());
        return true;
    }

    if (!m_core.install_display_list_update(context_id, *display_list, async_scrolling_metadata, visual_context_tree, encoded_scroll_state, now))
        return false;
    context->presenter.apply_resource_transaction(move(resource_transaction));
    context->presenter.set_display_list(display_list);
    resolve_video_sinks(*context);
    show_animated_image_frames_in(*context);

    update_unpainted_video_update_scheduling();
    return true;
}

void CompositorState::update_display_list_resources(Web::CompositorContextId context_id, Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);
    context->presenter.apply_resource_transaction(move(resource_transaction));
    show_animated_image_frames_in(*context);
}

bool CompositorState::update_visual_context_tree(Web::CompositorContextId context_id, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction, ReadonlyBytes encoded_scroll_state, IPC::Encoded<Compositing::KeyboardScrollState> const& keyboard_scroll_state)
{
    auto now = MonotonicTime::now();
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);

    switch (m_core.update_visual_context_tree(context_id, visual_context_tree, encoded_scroll_state, keyboard_scroll_state, now)) {
    case CompositorCore::VisualContextTreeUpdateResult::TreeReplaced:
        context->presenter.apply_resource_transaction(move(resource_transaction));
        break;
    case CompositorCore::VisualContextTreeUpdateResult::TreeDropped:
        break;
    case CompositorCore::VisualContextTreeUpdateResult::InvalidScrollState:
        return false;
    }
    show_animated_image_frames_in(*context);
    return true;
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
    for (auto& context_entry : m_presented_contexts) {
        if (context_entry.value->web_content_client.web_content_client_id() == client.web_content_client_id())
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

void CompositorState::add_animated_image(CompositorStateWebContentClient& client, Compositing::ImageFrameResourceId id, AnimatedImage::Description description)
{
    m_animated_images.ensure(&client).set(id.value(), make<AnimatedImage>(move(description)));
}

void CompositorState::remove_animated_image(CompositorStateWebContentClient& client, Compositing::ImageFrameResourceId id)
{
    if (auto images = m_animated_images.find(&client); images != m_animated_images.end())
        images->value.remove(id.value());
}

AnimatedImage* CompositorState::animated_image_mutable(CompositorStateWebContentClient& client, Compositing::ImageFrameResourceId id)
{
    auto images = m_animated_images.find(&client);
    if (images == m_animated_images.end())
        return nullptr;
    auto image = images->value.find(id.value());
    return image == images->value.end() ? nullptr : image->value.ptr();
}

AnimatedImage const* CompositorState::animated_image(CompositorStateWebContentClient& client, Compositing::ImageFrameResourceId id) const
{
    return const_cast<CompositorState*>(this)->animated_image_mutable(client, id);
}

AnimatedImage* CompositorState::animated_image_with_session(CompositorStateWebContentClient& client, i64 session_id)
{
    auto images = m_animated_images.find(&client);
    if (images == m_animated_images.end())
        return nullptr;
    for (auto& image : images->value) {
        if (image.value->session_id() == session_id)
            return image.value.ptr();
    }
    return nullptr;
}

void CompositorState::start_animated_image(CompositorStateWebContentClient& client, Compositing::ImageFrameResourceId id)
{
    if (auto* image = animated_image_mutable(client, id)) {
        image->start(MonotonicTime::now());
        schedule_animated_image_update({});
    }
}

void CompositorState::stop_animated_image(CompositorStateWebContentClient& client, Compositing::ImageFrameResourceId id)
{
    if (auto* image = animated_image_mutable(client, id))
        image->stop();
}

void CompositorState::animated_image_frames_decoded(CompositorStateWebContentClient& client, i64 session_id, Vector<NonnullRefPtr<Gfx::Bitmap>> bitmaps)
{
    // NB: Frames that brought nothing new must wait for the next tick, or the same request would go out again at once.
    if (auto* image = animated_image_with_session(client, session_id); image && image->receive_frames(move(bitmaps)))
        update_animated_images();
}

void CompositorState::animated_image_decode_failed(CompositorStateWebContentClient& client, i64 session_id)
{
    if (auto* image = animated_image_with_session(client, session_id))
        image->decoding_failed();
}

void CompositorState::animated_image_decoder_lost(CompositorStateWebContentClient& client)
{
    if (auto images = m_animated_images.find(&client); images != m_animated_images.end()) {
        for (auto& image : images->value)
            image.value->lose_decoder();
    }
}

bool CompositorState::animated_image_is_drawn(CompositorStateWebContentClient* client, Compositing::ImageFrameResourceId id)
{
    for (auto& context_entry : m_presented_contexts) {
        auto& context = *context_entry.value;
        if (context.web_content_client.web_content_client_id() == client->web_content_client_id() && context_is_effectively_visible(context) && context.presenter.resource_storage().has_image_frame(id))
            return true;
    }
    return false;
}

void CompositorState::show_animated_image_frame(CompositorStateWebContentClient* client, Compositing::ImageFrameResourceId id, Gfx::DecodedImageFrame const& frame)
{
    for (auto& context_entry : m_presented_contexts) {
        auto& context = *context_entry.value;
        if (context.web_content_client.web_content_client_id() != client->web_content_client_id() || !context.presenter.resource_storage().has_image_frame(id))
            continue;
        context.presenter.resource_storage().replace_image_frame_pixels(id, frame);
        if (!context_is_effectively_visible(context))
            continue;
        if (auto rect = context.presenter.self_present_rect(); rect.has_value())
            schedule_present_frame(context_entry.key, context, PendingFrame::repainting_changes(*rect));
    }
}

void CompositorState::show_animated_image_frames_in(PresentedContext& context)
{
    auto images = m_animated_images.find(&context.web_content_client);
    if (images == m_animated_images.end())
        return;
    for (auto& image_entry : images->value) {
        Compositing::ImageFrameResourceId id { image_entry.key };
        auto const& frame = image_entry.value->shown_frame();
        if (frame.has_value() && context.presenter.resource_storage().has_image_frame(id) && !context.presenter.resource_storage().image_frame(id).has_same_pixels_as(*frame))
            context.presenter.resource_storage().replace_image_frame_pixels(id, *frame);
    }
    // An image that nothing drew has been waiting for a context to draw it.
    if (!images->value.is_empty())
        schedule_animated_image_update({});
}

void CompositorState::update_animated_images()
{
    auto now = MonotonicTime::now();
    Optional<MonotonicTime> next_update;
    for (auto& client_entry : m_animated_images) {
        auto* client = client_entry.key;
        for (auto& image_entry : client_entry.value) {
            Compositing::ImageFrameResourceId id { image_entry.key };
            auto& image = *image_entry.value;

            // An image nothing shows costs nothing. It finds its place by the clock once something shows it again.
            if (!animated_image_is_drawn(client, id))
                continue;

            if (image.advance_to(now))
                show_animated_image_frame(client, id, *image.shown_frame());

            auto is_animating = image.is_animating(now);
            if (is_animating || image.shown_frame_index() != image.frame_index_at(now)) {
                if (auto request = image.take_frame_request(now); request.has_value())
                    client->request_animated_image_frames(*request);
            }
            if (is_animating) {
                auto next_frame_time = image.next_frame_time_after(now);
                if (!next_update.has_value() || next_frame_time < *next_update)
                    next_update = next_frame_time;
            }
        }
    }
    if (next_update.has_value())
        schedule_animated_image_update(*next_update - now);
}

void CompositorState::schedule_animated_image_update(AK::Duration delay)
{
    if (!m_animated_image_update_timer) {
        m_animated_image_update_timer = Core::Timer::create_single_shot(AK::Duration::from_milliseconds(0), [this] {
            update_animated_images();
        });
    }
    m_animated_image_update_timer->restart(delay);
}

void CompositorState::present_contexts_drawing_video_sink(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle)
{
    for (auto& context_entry : m_presented_contexts) {
        auto& context = *context_entry.value;
        if (context.web_content_client.web_content_client_id() != client.web_content_client_id())
            continue;
        if (!context_is_effectively_visible(context))
            continue;
        for (auto const& resource_entry : context.presenter.resource_storage().video_sink_handles()) {
            if (resource_entry.value == handle) {
                if (auto rect = context.presenter.self_present_rect(); rect.has_value())
                    schedule_present_frame(context_entry.key, context, PendingFrame::repainting_changes(*rect));
                break;
            }
        }
    }
}

void CompositorState::present_contexts_drawing_canvas(CompositorStateWebContentClient& client, Compositing::CanvasId canvas_id)
{
    for (auto& context_entry : m_presented_contexts) {
        auto& context = *context_entry.value;
        if (context.web_content_client.web_content_client_id() != client.web_content_client_id())
            continue;
        if (!context_is_effectively_visible(context) || !context.presenter.draws_canvas(canvas_id))
            continue;
        if (auto rect = context.presenter.self_present_rect(); rect.has_value())
            schedule_present_frame(context_entry.key, context, PendingFrame::repainting_changes(*rect));
    }
}

void CompositorState::resolve_video_sinks(PresentedContext& context)
{
    auto& client = context.web_content_client;
    for (auto const& resource_entry : context.presenter.resource_storage().video_sink_handles()) {
        auto* sink_state = video_sink_state(client, resource_entry.value);
        context.presenter.resource_storage().set_video_sink(Compositing::VideoSinkResourceId { resource_entry.key }, sink_state ? sink_state->sink : nullptr);
    }
}

bool CompositorState::video_sink_is_painted_by_any_context(CompositorStateWebContentClient* client, Media::VideoSinkHandle handle) const
{
    for (auto const& context_entry : m_presented_contexts) {
        auto const& context = *context_entry.value;
        if (context.web_content_client.web_content_client_id() != client->web_content_client_id())
            continue;
        for (auto const& resource_entry : context.presenter.resource_storage().video_sink_handles()) {
            if (resource_entry.value == handle)
                return true;
        }
    }
    return false;
}

int CompositorState::unpainted_video_update_interval_ms() const
{
    auto max_refresh_rate = 60.0;
    for (auto const& context_entry : m_presented_contexts)
        max_refresh_rate = max(max_refresh_rate, context_entry.value->display_refresh_rate);
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
    for (auto& context_entry : m_presented_contexts) {
        auto& context = *context_entry.value;
        if (display_id_for_context(context) != display_id)
            continue;
        if (!context_is_effectively_visible(context))
            continue;
        for (auto const& resource_entry : context.presenter.resource_storage().video_sink_handles()) {
            auto* sink_state = video_sink_state(context.web_content_client, resource_entry.value);
            if (sink_state != nullptr && sink_state->requires_updates) {
                vsync_scheduler_for_display(display_id).schedule(display_refresh_rate_for_context(context));
                break;
            }
        }
    }
}

Optional<u64> CompositorState::display_id_for_context(PresentedContext const& context) const
{
    auto current_context = &context;
    while (current_context) {
        if (current_context->display_id.has_value())
            return current_context->display_id;
        auto parent_context_id = current_context->parent_context_id;
        if (!parent_context_id.has_value())
            break;
        current_context = presented_context_if_present(*parent_context_id);
    }
    return {};
}

CompositorState::PresentedContext const* CompositorState::root_context_of(PresentedContext const& context) const
{
    auto const* current_context = &context;
    while (true) {
        auto parent_context_id = current_context->parent_context_id;
        if (!parent_context_id.has_value())
            return current_context;
        auto const* parent_context = presented_context_if_present(*parent_context_id);
        if (!parent_context)
            return current_context;
        current_context = parent_context;
    }
}

Web::CompositorContextId CompositorState::root_context_id_of(Web::CompositorContextId context_id) const
{
    auto const* current_context = presented_context_if_present(context_id);
    VERIFY(current_context);
    while (true) {
        auto parent_context_id = current_context->parent_context_id;
        if (!parent_context_id.has_value())
            return context_id;
        auto const* parent_context = presented_context_if_present(*parent_context_id);
        if (!parent_context)
            return context_id;
        context_id = *parent_context_id;
        current_context = parent_context;
    }
}

bool CompositorState::context_is_effectively_visible(PresentedContext const& context) const
{
    return root_context_of(context)->visibility == Compositing::ContextVisibility::Visible;
}

double CompositorState::display_refresh_rate_for_context(PresentedContext const& context) const
{
    auto current_context = &context;
    while (current_context) {
        if (current_context->display_id.has_value())
            return current_context->display_refresh_rate;
        auto parent_context_id = current_context->parent_context_id;
        if (!parent_context_id.has_value())
            break;
        current_context = presented_context_if_present(*parent_context_id);
    }
    return context.display_refresh_rate;
}

void CompositorState::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    auto* context = presented_context_if_present(context_id);
    if (!context)
        return;

    auto only_the_pixel_ratio_changed = viewport_size == context->presenter.viewport_size() && device_pixel_ratio != context->presenter.device_pixel_ratio();
    m_core.viewport_size_updated(context_id, viewport_size);
    auto is_page_presentation_context = context->page_id.has_value() && !context->parent_context_id.has_value();
    context->presenter.set_viewport_size(viewport_size, device_pixel_ratio, is_page_presentation_context ? window_resize_in_progress : Compositing::WindowResizingInProgress::No);
    resize_backing_stores_if_needed(context_id, *context);

    // The paused debugger's page will not paint again. Any other page's next frame damages the whole viewport when its
    // size changes, but a new pixel ratio alone need not damage the overlays.
    auto const& overlays = context->presenter.ui_overlays();
    if (overlays.paused_debugger.has_value() || (only_the_pixel_ratio_changed && overlays.picture_in_picture_controls.has_value())) {
        if (auto viewport_rect = context->presenter.viewport_rect_for_ui_overlay(); viewport_rect.has_value())
            schedule_present_frame(context_id, *context, *viewport_rect);
    }
    if (context->presenter.should_shrink_backing_stores_after_resize())
        schedule_backing_store_shrink(context_id, *context);
}

void CompositorState::set_ui_overlays(Web::CompositorContextId context_id, Compositing::UIOverlays overlays)
{
    auto* context = presented_context_if_present(context_id);
    if (!context)
        return;
    if (context->presenter.ui_overlays() == overlays)
        return;
    m_core.set_paused_debugger_overlay_is_shown(context_id, overlays.paused_debugger.has_value());
    context->presenter.set_ui_overlays(move(overlays));

    if (auto viewport_rect = context->presenter.viewport_rect_for_ui_overlay(); viewport_rect.has_value())
        schedule_present_frame(context_id, *context, *viewport_rect);
}

void CompositorState::set_display_metadata(Web::CompositorContextId context_id, Optional<u64> display_id, double refresh_rate)
{
    auto* context = presented_context_if_present(context_id);
    if (!context)
        return;

    VERIFY(refresh_rate == refresh_rate);
    VERIFY(refresh_rate > 0);
    VERIFY(refresh_rate < AK::Infinity<double>);

    context->display_id = display_id;
    context->display_refresh_rate = refresh_rate;
    if (context->presenter.pending_present_frame_scheduled()) {
        schedule_pending_present_frame(context_id, *context);
        if (m_unpainted_video_update_timer)
            m_unpainted_video_update_timer->set_interval(AK::Duration::from_milliseconds(unpainted_video_update_interval_ms()));
    }

    if ((context->rendering_opportunity_request.is_requested() || context->clock_tick_request.is_requested()) && context_is_effectively_visible(*context))
        vsync_scheduler_for_display(display_id_for_context(*context)).schedule(display_refresh_rate_for_context(*context));
}

void CompositorState::set_context_visibility(Web::CompositorContextId context_id, Compositing::ContextVisibility visibility)
{
    auto now = MonotonicTime::now();
    auto* context = presented_context_if_present(context_id);
    if (!context)
        return;
    m_core.set_visibility(context_id, visibility, now);
    schedule_requested_presents();
    if (context->visibility == visibility)
        return;
    context->visibility = visibility;

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} became {}", context_id, visibility == Compositing::ContextVisibility::Visible ? "visible" : "hidden");

    if (visibility == Compositing::ContextVisibility::Visible)
        resume_presentation_after_becoming_visible(context_id, *context);
}

void CompositorState::set_window_server_may_show_high_dynamic_range(bool may_show)
{
    if (m_window_server_may_show_high_dynamic_range == may_show)
        return;
    m_window_server_may_show_high_dynamic_range = may_show;
    for (auto& context_entry : m_presented_contexts) {
        context_entry.value->presenter.set_window_server_may_show_high_dynamic_range(may_show);
        present_current_frame(context_entry.key, *context_entry.value);
    }
}

void CompositorState::set_context_presents_from_cpu_memory(Web::CompositorContextId context_id, bool presents_from_cpu_memory)
{
    auto* context = presented_context_if_present(context_id);
    if (!context || context->presents_from_cpu_memory == presents_from_cpu_memory)
        return;
    auto previous_gpu_sharing = gpu_sharing_for_context(context_id);
    context->presents_from_cpu_memory = presents_from_cpu_memory;
    // NB: Without a GPU, the backing stores are in CPU memory either way.
    if (!m_skia_backend_context || gpu_sharing_for_context(context_id) == previous_gpu_sharing)
        return;

    // NB: Otherwise, the presentation would read every frame back from GPU memory on its main thread.
    context->presenter.invalidate_backing_stores();
    resize_backing_stores_if_needed(context_id, *context);
}

void CompositorState::resume_presentation_after_becoming_visible(Web::CompositorContextId root_context_id, PresentedContext& root_context)
{
    schedule_animated_image_update({});

    for (auto& context_entry : m_presented_contexts) {
        auto& context = *context_entry.value;
        if (root_context_of(context) != &root_context)
            continue;
        schedule_animation_frames_if_needed(context_entry.key, context);
        if ((context.rendering_opportunity_request.is_requested() || context.clock_tick_request.is_requested()))
            vsync_scheduler_for_display(display_id_for_context(context)).schedule(display_refresh_rate_for_context(context));
    }

    if (auto frame_rect_to_present = root_context.presenter.frame_rect_to_repaint(); frame_rect_to_present.has_value())
        schedule_present_frame(root_context_id, root_context, *frame_rect_to_present);
}

void CompositorState::request_rendering_opportunity(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);

    if (!context->rendering_opportunity_request.request(maximum_frames_per_second))
        return;
    if (!context_is_effectively_visible(*context))
        return;

    auto display_id = display_id_for_context(*context);
    auto display_refresh_rate = display_refresh_rate_for_context(*context);
    auto& scheduler = vsync_scheduler_for_display(display_id);
    // INTEROP: Like Chromium's missed BeginFrame delivery, reuse a recent display tick when a context requests its
    //          next opportunity after the tick has already happened. This keeps heavy frames from waiting an extra tick.
    if (auto frame_time = scheduler.most_recent_tick_time(MonotonicTime::now(), display_refresh_rate);
        frame_time.has_value() && context->rendering_opportunity_request.is_due(*frame_time, display_refresh_rate)) {
        auto frame_interval = context->rendering_opportunity_request.frame_interval(display_refresh_rate);
        context->rendering_opportunity_request.did_deliver(*frame_time);
        context->web_content_client.rendering_opportunity(context_id, frame_time->nanoseconds(), frame_interval);
        return;
    }

    scheduler.schedule(display_refresh_rate);
}

void CompositorState::request_clock_tick(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    auto* context = presented_context_if_present(context_id);
    if (!context || !context->clock_tick_request.request(maximum_frames_per_second) || !context_is_effectively_visible(*context))
        return;
    vsync_scheduler_for_display(display_id_for_context(*context)).schedule(display_refresh_rate_for_context(*context));
}

void CompositorState::hurry_rendering_opportunity(Web::CompositorContextId context_id)
{
    auto* context = presented_context_if_present(context_id);
    if (!context || !context->rendering_opportunity_request.is_requested() || !context_is_effectively_visible(*context))
        return;
    auto display_refresh_rate = display_refresh_rate_for_context(*context);
    auto frame_interval = context->rendering_opportunity_request.frame_interval(display_refresh_rate);
    auto frame_time = MonotonicTime::now();
    context->rendering_opportunity_request.did_deliver(frame_time);
    context->web_content_client.rendering_opportunity(context_id, frame_time.nanoseconds(), frame_interval);
}

void CompositorState::present_frame(Web::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);
    context->size_laid_out_by_page = viewport_rect.size();
    // The frame joins whatever is queued, so a frame queued earlier cannot be presented after it.
    context->presenter.queue_present_frame({ viewport_rect, {} });
    if (try_present_frame_during_resize(context_id, *context))
        return;
    schedule_pending_present_frame(context_id, *context);
}

void CompositorState::present_frame(Web::CompositorContextId context_id, PresentedContext& context, PendingFrame pending_frame)
{
    auto composited_context_resolver = resolver_for(context_id);
    auto prepared_frame = context.presenter.prepare_frame(*m_display_list_player, pending_frame, m_core.frame_inputs(context_id, MonotonicTime::now()), &composited_context_resolver);
    if (!prepared_frame.has_value())
        return;

    m_pending_async_presents.append(context_id, pending_frame.viewport_rect, prepared_frame->damage_rect, prepared_frame->bitmap_id, prepared_frame->device_pixel_ratio);
    auto* pending_present = &m_pending_async_presents.last();
    pending_present->shows_in_remote_layer = context.presenter.backing_store_publication_mode() == BackingStoreManager::PublicationMode::ShownInRemoteLayer;
    pending_present->publish_to_presentation = !pending_present->shows_in_remote_layer && presentation_for_context(context_id) != nullptr;
    pending_present->keeps_shown_store = !prepared_frame->rendered_surface;
    pending_present->sublayers = move(prepared_frame->sublayers);

    auto& event_loop = Core::EventLoop::current();
    auto self = NonnullRefPtr { *this };
    auto did_finish = [self = move(self), &event_loop, pending_present] {
        event_loop.deferred_invoke([self = move(self), pending_present] {
            self->did_finish_async_present(*pending_present);
        });
    };
    // A frame without a store still waits for what was drawn into the surfaces that the window server shows above the
    // store, such as a canvas that presented.
    if (prepared_frame->rendered_surface)
        m_display_list_player->flush_async(*prepared_frame->rendered_surface, move(did_finish));
    else
        m_skia_backend_context->flush_and_submit_async(move(did_finish));
    context.presenter.did_submit_prepared_frame(pending_frame.viewport_rect);
    // Handing the frame to the client before its GPU work finishes keeps it ahead of the window server's deadline for
    // the next refresh. A client that reads frames from CPU memory does not wait for that work, so it gets each frame
    // once it is done.
    if (context.presenter.presents_to_client() && m_skia_backend_context && !context.presents_from_cpu_memory && frame_hand_off_for(context) == WindowServer::FrameHandOff::OnceGpuWorkIsScheduled)
        hand_off_frame_once_gpu_work_is_scheduled(*pending_present);
    schedule_gpu_completion_check();
}

WindowServer::FrameHandOff CompositorState::frame_hand_off_for(PresentedContext const& context) const
{
    if (context.remote_layer_id.has_value()) {
        if (auto remote_layer = m_remote_layers.find(*context.remote_layer_id); remote_layer != m_remote_layers.end())
            return remote_layer->value.window_server->frame_hand_off();
    }
    return WindowServer::frame_hand_off_through_presentation();
}

void CompositorState::hand_off_frame_once_gpu_work_is_scheduled([[maybe_unused]] PendingAsyncPresent& pending_present)
{
#ifdef AK_OS_MACOS
    // The window server's reads of an IOSurface wait for GPU work that is already scheduled to write it.
    auto& event_loop = Core::EventLoop::current();
    m_skia_backend_context->metal_context().when_committed_work_is_scheduled([self = NonnullRefPtr { *this }, &event_loop, pending_present = &pending_present]() mutable {
        event_loop.deferred_invoke([self = move(self), pending_present] {
            for (auto& entry : self->m_pending_async_presents) {
                if (&entry == pending_present) {
                    self->deliver_rendered_frame(entry);
                    return;
                }
            }
        });
    });
#else
    VERIFY_NOT_REACHED();
#endif
}

void CompositorState::schedule_requested_presents()
{
    auto requests = m_core.take_present_requests();
    for (auto const& request : requests) {
        auto* context = presented_context_if_present(request.context_id);
        if (!context)
            continue;
        auto viewport_rect = request.viewport_rect;
        if (!viewport_rect.has_value())
            viewport_rect = context->presenter.viewport_rect_for_ui_overlay();
        if (!viewport_rect.has_value())
            continue;
        switch (request.scheduling) {
        case PresentScheduling::OnNextDisplayTick:
            schedule_present_frame(request.context_id, *context, PendingFrame { *viewport_rect, request.forced_damage_rect });
            break;
        case PresentScheduling::WithDisplayTickInProgress:
            context->presenter.queue_present_frame(PendingFrame { *viewport_rect, request.forced_damage_rect });
            break;
        }
    }
}

void CompositorState::schedule_present_frame(Web::CompositorContextId context_id, PresentedContext& context, PendingFrame pending_frame)
{
    context.presenter.queue_present_frame(pending_frame);
    schedule_pending_present_frame(context_id, context);
}

void CompositorState::schedule_present_frame(Web::CompositorContextId context_id, PresentedContext& context, Gfx::IntRect viewport_rect)
{
    schedule_present_frame(context_id, context, PendingFrame::repainting_everything(viewport_rect));
}

void CompositorState::schedule_pending_present_frame(Web::CompositorContextId context_id, PresentedContext& context)
{
    if (!context.presenter.presents_to_client()) {
        schedule_containing_context_present(context);
        // A nested context is presented through its containing context, but its
        // own async animations still need a vsync source. The containing frame
        // may already be up to date (and therefore not schedule a new present),
        // so explicitly keep the effective display's scheduler ticking while
        // a nested animation is active.
        schedule_animation_frames_if_needed(context_id, context);
        return;
    }

    schedule_pending_present_frame_on_vsync(context_id, context);
}

bool CompositorState::needs_animation_frames(Web::CompositorContextId context_id, PresentedContext const& context, MonotonicTime now)
{
    return m_core.needs_animation_frames(context_id, now) || context.presenter.ui_overlay_fades_need_frame();
}

bool CompositorState::has_pending_present_frame_scheduled_on(PresentedContext const& context, Optional<u64> display_id)
{
    return context.presenter.pending_present_frame_scheduled() && context.display_id == display_id;
}

void CompositorState::schedule_animation_frames_if_needed(Web::CompositorContextId context_id, PresentedContext const& context)
{
    if (needs_animation_frames(context_id, context, MonotonicTime::now()) && context_is_effectively_visible(context))
        vsync_scheduler_for_display(display_id_for_context(context)).schedule(display_refresh_rate_for_context(context));
}

void CompositorState::schedule_pending_present_frame_on_vsync(Web::CompositorContextId, PresentedContext& context)
{
    if (!context_is_effectively_visible(context))
        return;
    context.presenter.mark_pending_present_frame_scheduled();
    vsync_scheduler_for_display(context.display_id).schedule(context.display_refresh_rate);
}

void CompositorState::schedule_containing_context_present(PresentedContext& context)
{
    auto parent_context_id = context.parent_context_id;
    if (!parent_context_id.has_value())
        return;

    auto* parent_context = presented_context_if_present(*parent_context_id);
    VERIFY(parent_context);
    present_current_frame(*parent_context_id, *parent_context);
}

void CompositorState::schedule_pending_present_frame_if_unblocked(Web::CompositorContextId context_id, PresentedContext& context)
{
    if (!context.presenter.can_schedule_pending_present_frame_if_unblocked())
        return;

    if (try_present_frame_during_resize(context_id, context))
        return;
    schedule_pending_present_frame(context_id, context);
}

bool CompositorState::try_present_frame_during_resize(Web::CompositorContextId context_id, PresentedContext& context)
{
    if (!context.presenter.window_resize_in_progress() || !context.presenter.presents_to_client() || !context_is_effectively_visible(context))
        return false;
    auto pending_frame = context.presenter.take_pending_present_frame_if_unblocked();
    if (!pending_frame.has_value())
        return false;

    // Rasterize resize frames as soon as a backing store is available, so the previous size does not
    // remain visible for another display tick. prepare_frame() owns requeuing any blocked frame.
    present_frame(context_id, context, *pending_frame);
    // The display tick does more than present frames, such as updating the video sinks that are painted.
    vsync_scheduler_for_display(display_id_for_context(context)).schedule(display_refresh_rate_for_context(context));
    return true;
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

    for (auto& context_entry : m_presented_contexts) {
        auto context_id = context_entry.key;
        auto& context = *context_entry.value;
        auto now = MonotonicTime::now();
        if (!context_is_effectively_visible(context)) {
            context.presenter.unschedule_pending_present_frame();
            continue;
        }

        if (context.rendering_opportunity_request.is_requested() && display_id_for_context(context) == display_id) {
            auto display_refresh_rate = display_refresh_rate_for_context(context);
            if (context.rendering_opportunity_request.is_due(frame_time, display_refresh_rate)) {
                auto frame_interval = context.rendering_opportunity_request.frame_interval(display_refresh_rate);
                context.rendering_opportunity_request.did_deliver(frame_time);
                context.web_content_client.rendering_opportunity(context_id, frame_time.nanoseconds(), frame_interval);
            } else {
                vsync_scheduler_for_display(display_id).schedule(display_refresh_rate);
            }
        }

        auto context_is_on_tick_display = display_id_for_context(context) == display_id;
        DisplayTickInputs inputs {
            .context_is_on_tick_display = context_is_on_tick_display,
            .present_is_scheduled_on_tick_display = has_pending_present_frame_scheduled_on(context, display_id),
            .ui_overlay_fades_need_frame = context.presenter.ui_overlay_fades_need_frame(),
            .clock_tick_interval_in_milliseconds = {},
        };
        if (context.clock_tick_request.is_requested() && context_is_on_tick_display) {
            auto display_refresh_rate = display_refresh_rate_for_context(context);
            if (context.clock_tick_request.is_due(frame_time, display_refresh_rate)) {
                inputs.clock_tick_interval_in_milliseconds = context.clock_tick_request.frame_interval(display_refresh_rate);
                context.clock_tick_request.did_deliver(frame_time);
            } else {
                vsync_scheduler_for_display(display_id).schedule(display_refresh_rate);
            }
        }

        auto tick = m_core.display_tick(context_id, frame_time, now, inputs);
        schedule_requested_presents();
        if (!tick.presents_on_display)
            continue;
        if (auto fade_frame = context.presenter.advance_ui_overlay_fades(frame_time); fade_frame.has_value())
            context.presenter.queue_present_frame(*fade_frame);

        add_backing_store_for_pending_frame_if_needed(context_id, context);
        auto needs_animation_frames = tick.needs_animation_frames || context.presenter.ui_overlay_fades_need_frame();
        auto pending_present_frame = context.presenter.take_pending_present_frame_if_unblocked();
        if (!pending_present_frame.has_value()) {
            if (has_pending_present_frame_scheduled_on(context, display_id) || (needs_animation_frames && context_is_on_tick_display))
                vsync_scheduler_for_display(display_id).schedule(display_refresh_rate_for_context(context));
            continue;
        }
        if (needs_animation_frames)
            schedule_present_frame(context_id, context, PendingFrame::repainting_changes(pending_present_frame->viewport_rect));
        present_frame(context_id, context, *pending_present_frame);
    }
}

bool CompositorState::request_screenshot(Web::CompositorContextId context_id, Gfx::ShareableBitmap& target_bitmap)
{
    auto now = MonotonicTime::now();
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);

    if (!context->presenter.has_display_list() || !target_bitmap.is_valid() || !target_bitmap.bitmap())
        return false;

    auto composited_context_resolver = resolver_for(context_id);
    auto screenshot_inputs = m_core.frame_inputs_for_screenshot(context_id, now);
    context->presenter.paint_screenshot(*m_display_list_player, target_bitmap, screenshot_inputs.frame_inputs, &composited_context_resolver);
    if (!screenshot_inputs.scrollbar_fade_damage_rect.is_empty()) {
        if (auto viewport_rect = context->presenter.viewport_rect_for_ui_overlay(); viewport_rect.has_value())
            schedule_present_frame(context_id, *context, { *viewport_rect, screenshot_inputs.scrollbar_fade_damage_rect });
    }
    return true;
}

// How long a screenshot waits for the processes that paint its context's descendants. One whose script never yields
// keeps its connection open without answering, and the screenshot then goes ahead with the frames it last handed over.
static constexpr auto SCREENSHOT_PREPARATION_LIMIT = AK::Duration::from_seconds(10);

void CompositorState::prepare_remote_descendants_for_screenshot(CompositorStateWebContentClient& requester, Web::CompositorContextId context_id, u64 request_id)
{
    HashMap<Web::CompositorContextId, Vector<Web::CompositorContextId>> children;
    for (auto const& [child_id, child] : m_presented_contexts) {
        if (child->parent_context_id.has_value())
            children.ensure(*child->parent_context_id).append(child_id);
    }

    // From the first context of a branch that another process owns on down, each process paints the contexts it owns
    // there, the requester's included. Above that point, the requester paints its own as it takes the screenshot.
    HashMap<WebContentClientId, Vector<Web::CompositorContextId>> contexts_by_painter;
    HashTable<Web::CompositorContextId> visited;
    Vector<Web::CompositorContextId> to_visit { context_id };
    HashTable<Web::CompositorContextId> below_another_process;
    while (!to_visit.is_empty()) {
        auto id = to_visit.take_last();
        if (visited.set(id) != HashSetResult::InsertedNewEntry)
            continue;
        auto context = m_presented_contexts.get(id);
        if (!context.has_value())
            continue;
        auto owner = (*context)->web_content_client.web_content_client_id();
        bool painted_by_owner = below_another_process.contains(id) || owner != requester.web_content_client_id();
        if (painted_by_owner)
            contexts_by_painter.ensure(owner).append(id);
        auto child_ids = children.find(id);
        if (child_ids == children.end())
            continue;
        for (auto child_id : child_ids->value) {
            if (painted_by_owner)
                below_another_process.set(child_id);
            to_visit.append(child_id);
        }
    }

    if (contexts_by_painter.is_empty()) {
        requester.tell_remote_descendants_prepared_for_screenshot(request_id);
        return;
    }

    auto preparation_id = m_next_screenshot_preparation_id++;
    ScreenshotPreparation preparation { requester.web_content_client_id(), request_id, {}, {} };
    for (auto const& [painter, _] : contexts_by_painter)
        preparation.awaited_painters.set(painter);
    preparation.deadline = Core::Timer::create_single_shot(SCREENSHOT_PREPARATION_LIMIT, [this, preparation_id] {
        finish_screenshot_preparation(preparation_id);
    });
    preparation.deadline->start();
    m_screenshot_preparations.set(preparation_id, move(preparation));
    for (auto const& [painter, context_ids] : contexts_by_painter) {
        if (auto* client = web_content_client_if_present(painter))
            client->ask_to_paint_contexts_for_screenshot(preparation_id, context_ids);
    }
}

void CompositorState::did_paint_contexts_for_screenshot(CompositorStateWebContentClient& painter, u64 preparation_id)
{
    auto preparation = m_screenshot_preparations.get(preparation_id);
    if (!preparation.has_value() || !preparation->awaited_painters.remove(painter.web_content_client_id()))
        return;
    finish_screenshot_preparation_if_done(preparation_id);
}

void CompositorState::finish_screenshot_preparation_if_done(u64 preparation_id)
{
    auto preparation = m_screenshot_preparations.get(preparation_id);
    if (!preparation.has_value() || !preparation->awaited_painters.is_empty())
        return;
    finish_screenshot_preparation(preparation_id);
}

void CompositorState::finish_screenshot_preparation(u64 preparation_id)
{
    auto finished = m_screenshot_preparations.take(preparation_id);
    if (!finished.has_value())
        return;
    if (finished->deadline)
        finished->deadline->stop();
    if (auto* requester = web_content_client_if_present(finished->requester))
        requester->tell_remote_descendants_prepared_for_screenshot(finished->request_id);
}

Optional<Gfx::ShareableBitmap> CompositorState::take_frame_snapshot(Web::CompositorContextId context_id, Optional<Gfx::IntSize> maximum_size)
{
    auto* context = presented_context_if_present(context_id);
    if (!context || !context->presenter.presents_to_client())
        return {};
    return context->presenter.snapshot_of_presented_frame(*m_display_list_player, maximum_size);
}

void CompositorState::presented_bitmap_ready_to_paint(Web::CompositorContextId context_id, i32 bitmap_id)
{
    auto* context = presented_context_if_present(context_id);
    if (!context)
        return;

    if (!context->presenter.acknowledge_presented_bitmap(bitmap_id))
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

static void log_presented_frame(Optional<u64> page_id, Gfx::IntRect damage)
{
    auto* log = Core::StatsLog::the();
    if (!log || !page_id.has_value())
        return;
    JsonObject data;
    data.set("page_id"sv, *page_id);
    data.set("changed"sv, !damage.is_empty());
    data.set("damage_x"sv, damage.x());
    data.set("damage_y"sv, damage.y());
    data.set("damage_width"sv, damage.width());
    data.set("damage_height"sv, damage.height());
    log->event("frame"sv, move(data));
}

void CompositorState::show_frame_in_remote_layer(Web::CompositorContextId context_id, PresentedContext& context, PendingAsyncPresent& pending_present)
{
    VERIFY(context.remote_layer_id.has_value());
    auto remote_layer = m_remote_layers.find(*context.remote_layer_id);
    VERIFY(remote_layer != m_remote_layers.end());
    auto* store = pending_present.keeps_shown_store ? nullptr : context.presenter.window_server_surface_of_latest_rendered_store();
    VERIFY(store || pending_present.keeps_shown_store);

    auto frame_size = pending_present.viewport_rect.size();
    remote_layer->value.layer->show_frame({
        .store = store ? Optional<PresentableSurface::Lease> { store->lease() } : OptionalNone {},
        .painted_size = frame_size,
        .device_pixel_ratio = pending_present.device_pixel_ratio,
        .fit_within = remote_layer->value.fit_within,
        .sublayers = move(pending_present.sublayers),
    });
    remote_layer->value.size_of_shown_frame_laid_out_by_page = frame_size == context.size_laid_out_by_page ? Optional<Gfx::IntSize> { frame_size } : OptionalNone {};
    report_held_synchronized_resize_frames(remote_layer->key);
    remote_layer->value.presentation->did_present_frame_in_remote_layer(context_id, pending_present.viewport_rect, pending_present.damage_rect, pending_present.device_pixel_ratio);
}

void CompositorState::deliver_rendered_frame(PendingAsyncPresent& pending_present)
{
    if (pending_present.was_cancelled && pending_present.keeps_shown_store && !pending_present.was_delivered) {
        if (auto* context = presented_context_if_present(pending_present.context_id))
            context->presenter.did_deliver_frame_without_store();
    }
    if (pending_present.was_cancelled || exchange(pending_present.was_delivered, true))
        return;

    auto context_id = pending_present.context_id;
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);

    if (pending_present.keeps_shown_store)
        context->presenter.did_deliver_frame_without_store();
    else
        context->presenter.did_finish_gpu_present(pending_present.bitmap_id, pending_present.viewport_rect.size());
    auto* presentation = pending_present.publish_to_presentation ? presentation_for_context(context_id) : nullptr;
    if (pending_present.shows_in_remote_layer) {
        log_presented_frame(context->page_id, pending_present.damage_rect);
        show_frame_in_remote_layer(context_id, *context, pending_present);
    } else if (context->presenter.presents_to_client() && !presentation) {
        // Nobody shows the frame, so it is released here, on another turn, as a presentation would release it.
        Core::deferred_invoke([self = NonnullRefPtr { *this }, context_id, bitmap_id = pending_present.bitmap_id] {
            self->presented_bitmap_ready_to_paint(context_id, bitmap_id);
        });
    } else if (context->presenter.presents_to_client()) {
        log_presented_frame(context->page_id, pending_present.damage_rect);
        presentation->did_present_frame(context_id, pending_present.viewport_rect, pending_present.damage_rect, pending_present.bitmap_id, pending_present.device_pixel_ratio);
    }
    resize_backing_stores_if_needed(context_id, *context);
    if (auto parent_context_id = context->parent_context_id; parent_context_id.has_value()) {
        auto* parent_context = presented_context_if_present(*parent_context_id);
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

CompositorState::PresentedContext* CompositorState::presented_context_if_present(Web::CompositorContextId context_id)
{
    auto it = m_presented_contexts.find(context_id);
    if (it == m_presented_contexts.end())
        return nullptr;
    return it->value.ptr();
}

CompositorState::PresentedContext const* CompositorState::presented_context_if_present(Web::CompositorContextId context_id) const
{
    auto it = m_presented_contexts.find(context_id);
    if (it == m_presented_contexts.end())
        return nullptr;
    return it->value.ptr();
}

void CompositorState::clear_parent_context(Web::CompositorContextId context_id, PresentedContext& context)
{
    auto parent_context_id = context.parent_context_id;
    if (!parent_context_id.has_value())
        return;

    context.parent_context_id = {};
    m_core.set_parent_context(context_id, {});
    auto* parent_context = presented_context_if_present(*parent_context_id);
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
    auto* child_context = presented_context_if_present(child_context_id);
    if (!child_context)
        return {};
    auto child_parent_context_id = child_context->parent_context_id;
    if (!child_parent_context_id.has_value() || *child_parent_context_id != parent_context_id)
        return {};

    if (child_context->presenter.update_composited_raster_transform(destination_rect, canvas_transform)) {
        auto publication = child_context->presenter.resize_backing_stores_if_needed(m_skia_backend_context, BackingStoreManager::GpuSharing::Disallowed, BackingStoreManager::PublicationMode::Unpublished);
        VERIFY(!publication.has_value());
    }

    if (child_context->presenter.needs_rasterization()) {
        auto composited_context_resolver = resolver_for(child_context_id);
        DisplayListPlayerSkia display_list_player { m_skia_backend_context };
        child_context->presenter.present_synchronously(display_list_player, m_core.frame_inputs(child_context_id, MonotonicTime::now()), &composited_context_resolver);
    }

    return child_context->presenter.composited_surface();
}

void CompositorState::resize_backing_stores_if_needed(Web::CompositorContextId context_id, PresentedContext& context)
{
    auto publication_mode = BackingStoreManager::PublicationMode::Unpublished;
    RemoteLayerState const* remote_layer_showing_context = nullptr;
    if (context.remote_layer_id.has_value()) {
        auto remote_layer = m_remote_layers.find(*context.remote_layer_id);
        VERIFY(remote_layer != m_remote_layers.end());
        remote_layer_showing_context = &remote_layer->value;
        publication_mode = BackingStoreManager::PublicationMode::ShownInRemoteLayer;
    } else if (presentation_for_context(context_id)) {
        publication_mode = BackingStoreManager::PublicationMode::Published;
    }

    // The window server reads what it shows above the store straight from GPU memory.
    auto window_server_may_show_media = remote_layer_showing_context && m_skia_backend_context && !context.presents_from_cpu_memory;
    context.presenter.set_remote_layer_that_may_show_media(window_server_may_show_media ? remote_layer_showing_context->layer.ptr() : nullptr);
    auto window_server_showing_stores = remote_layer_showing_context ? RefPtr<WindowServer> { remote_layer_showing_context->window_server } : nullptr;
    if (auto publication = context.presenter.resize_backing_stores_if_needed(m_skia_backend_context, gpu_sharing_for_context(context_id), publication_mode, move(window_server_showing_stores)); publication.has_value()) {
        if (publication_mode == BackingStoreManager::PublicationMode::Published)
            publish_backing_stores(context_id, context, publication.release_value());
        present_current_frame(context_id, context);
    }
}

BackingStoreManager::GpuSharing CompositorState::gpu_sharing_for_context(Web::CompositorContextId context_id) const
{
    if (auto const* context = presented_context_if_present(context_id); context && context->presents_from_cpu_memory)
        return BackingStoreManager::GpuSharing::Disallowed;
#ifdef USE_DIRECTX
    // Shared Direct3D textures can only be opened by the presentation if it presents on the same adapter.
    auto* presentation = presentation_for_context(context_id);
    if (!presentation || !m_skia_backend_context || m_presentations.get(presentation)->gpu_presentation_adapter_luid != m_skia_backend_context->direct3d_context().adapter_luid())
        return BackingStoreManager::GpuSharing::Disallowed;
#else
    if (auto* presentation = presentation_for_context(context_id); presentation && m_presentations.get(presentation)->cannot_present_gpu_images)
        return BackingStoreManager::GpuSharing::Disallowed;
#endif
    return BackingStoreManager::GpuSharing::Allowed;
}

// A context scrolls elastically, takes presses on scrollbar tracks, and ends touchpad gestures as the presentation of
// its root context does. A platform that reports the momentum of a flick reports when the fingers lift for it.
void CompositorState::update_scroll_capabilities()
{
    for (auto& [context_id, context] : m_presented_contexts) {
        CompositorCore::ScrollCapabilities capabilities {};
        if (auto* presentation = presentation_for_context(root_context_id_of(context_id))) {
            auto const& presentation_capabilities = *m_presentations.get(presentation);
            capabilities.synthesizes_scroll_momentum = presentation_capabilities.synthesizes_scroll_momentum;
            capabilities.scrolls_elastically = presentation_capabilities.scrolls_elastically;
            capabilities.scrollbar_track_press_jumps = presentation_capabilities.scrollbar_track_press_jumps;
            capabilities.touchpad_gestures_end_when_fingers_lift = !presentation_capabilities.synthesizes_scroll_momentum;
        }
        m_core.set_scroll_capabilities(context_id, capabilities);
    }
}

void CompositorState::set_scrollbar_track_press_jumps(CompositorStateClient& presentation, bool jumps)
{
    m_presentations.get(&presentation)->scrollbar_track_press_jumps = jumps;
    update_scroll_capabilities();
}

void CompositorState::set_client_gpu_presentation_capability(CompositorStateClient& presentation, bool supported, u64 adapter_luid)
{
    Optional<u64> new_adapter_luid;
    if (supported)
        new_adapter_luid = adapter_luid;
    auto& capabilities = *m_presentations.get(&presentation);
    if (capabilities.gpu_presentation_adapter_luid == new_adapter_luid && capabilities.cannot_present_gpu_images == !supported)
        return;
    capabilities.gpu_presentation_adapter_luid = new_adapter_luid;
    capabilities.cannot_present_gpu_images = !supported;

    // Reallocate the backing stores of the presentation's contexts so they match the new capability.
    for (auto& context_entry : m_presented_contexts) {
        auto& context = *context_entry.value;
        if (presentation_for_context(context_entry.key) != &presentation)
            continue;
        context.presenter.invalidate_backing_stores();
        resize_backing_stores_if_needed(context_entry.key, context);
    }
}

void CompositorState::schedule_backing_store_shrink(Web::CompositorContextId context_id, PresentedContext& context)
{
    context.presenter.schedule_backing_store_shrink([this, context_id] {
        shrink_backing_stores_after_resize(context_id);
    });
}

void CompositorState::shrink_backing_stores_after_resize(Web::CompositorContextId context_id)
{
    auto* context = presented_context_if_present(context_id);
    if (!context)
        return;

    context->presenter.finish_window_resize();
    resize_backing_stores_if_needed(context_id, *context);
}

void CompositorState::add_backing_store_for_pending_frame_if_needed(Web::CompositorContextId context_id, PresentedContext& context)
{
    auto* presentation = presentation_for_context(context_id);
    if (!presentation && !context.remote_layer_id.has_value())
        return;
    auto publication = context.presenter.add_backing_store_for_pending_frame_if_needed(m_skia_backend_context);
    if (!publication.has_value())
        return;

    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} gained backing store {}, as the window server still reads every released one", context_id, publication->bitmap_ids);
    if (presentation && context.presenter.backing_store_publication_mode() == BackingStoreManager::PublicationMode::Published)
        presentation->did_add_backing_stores(context_id, move(publication->bitmap_ids), move(publication->shared_images));
    schedule_surplus_backing_store_retirement(context_id, context);
}

void CompositorState::schedule_surplus_backing_store_retirement(Web::CompositorContextId context_id, PresentedContext& context)
{
    context.presenter.schedule_surplus_backing_store_retirement([this, context_id] {
        retire_idle_surplus_backing_stores(context_id);
    });
}

void CompositorState::retire_idle_surplus_backing_stores(Web::CompositorContextId context_id)
{
    auto* context = presented_context_if_present(context_id);
    if (!context)
        return;

    auto retired_bitmap_ids = context->presenter.retire_idle_surplus_backing_stores();
    auto shows_stores_to_presentation = context->presenter.backing_store_publication_mode() == BackingStoreManager::PublicationMode::Published;
    if (auto* presentation = presentation_for_context(context_id); presentation && shows_stores_to_presentation && !retired_bitmap_ids.is_empty()) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} retired idle backing stores {}", context_id, retired_bitmap_ids);
        presentation->did_retire_backing_stores(context_id, move(retired_bitmap_ids));
    }
    if (context->presenter.has_surplus_backing_stores())
        schedule_surplus_backing_store_retirement(context_id, *context);
}

void CompositorState::present_current_frame(Web::CompositorContextId context_id, PresentedContext& context)
{
    if (auto frame_to_present = context.presenter.frame_rect_to_repaint(); frame_to_present.has_value())
        schedule_present_frame(context_id, context, *frame_to_present);
}

void CompositorState::publish_backing_stores(Web::CompositorContextId context_id, PresentedContext& context, BackingStoreManager::Publication&& publication)
{
    auto* presentation = presentation_for_context(context_id);
    VERIFY(presentation);
    VERIFY(context.presenter.presents_to_client());

    presentation->did_allocate_backing_stores(context_id, move(publication.bitmap_ids), move(publication.shared_images));
}

}
