/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Random.h>
#include <AK/StdLibExtras.h>
#include <AK/TemporaryChange.h>
#include <Compositor/CanvasHost.h>
#include <Compositor/CompositorState.h>
#include <Compositor/SkiaUtils.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StatsLog.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMediaCodecs/VideoSurface.h>
#include <core/SkCanvas.h>
#include <core/SkImage.h>

namespace Compositor {

static constexpr int gpu_completion_check_interval_ms = 1;

NonnullRefPtr<CompositorState> CompositorState::create(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, bool scrollbars_always_shown)
{
    return adopt_ref(*new CompositorState(move(skia_backend_context), scrollbars_always_shown));
}

CompositorState::CompositorState(RefPtr<Gfx::SkiaBackendContext> skia_backend_context, bool scrollbars_always_shown)
    : m_skia_backend_context(move(skia_backend_context))
    , m_core(*this, raster_host(*this), scrollbars_always_shown, gpu_backend(m_skia_backend_context))
{
}

RustFFI::GpuBackend CompositorState::gpu_backend([[maybe_unused]] RefPtr<Gfx::SkiaBackendContext> const& skia_backend_context)
{
    RustFFI::GpuBackend backend {
        .has_gpu = skia_backend_context != nullptr,
        .shared_stores_need_the_same_adapter = false,
        .adapter_luid = 0,
    };
#ifdef USE_DIRECTX
    backend.shared_stores_need_the_same_adapter = true;
    if (skia_backend_context)
        backend.adapter_luid = skia_backend_context->direct3d_context().adapter_luid();
#endif
    return backend;
}

IPC::RawMessage::OutcomeInRust CompositorState::handle_message_from_web_content(WebContentClientId client_id, IPC::RawMessage& message)
{
    return m_core.handle_message_from_web_content(client_id, message);
}

IPC::RawMessage::OutcomeInRust CompositorState::handle_message_from_presentation(PresentationId presentation_id, IPC::RawMessage& message)
{
    return m_core.handle_message_from_presentation(presentation_id, message);
}

CompositorState::PresentedContext::PresentedContext(CompositorStateWebContentClient& web_content_client, OwnedCanvasSurfaces canvas_surfaces)
    : web_content_client(web_content_client)
    , raster_targets(canvas_surfaces)
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
    if (auto* presentation = presentation_if_present(presentation_id))
        presentation->post_message_to_presentation(buffer);
}

CompositorStateClient* CompositorState::presentation_if_present(PresentationId presentation_id) const
{
    return m_presentations_by_id.get(presentation_id).value_or(nullptr);
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
    m_core.timer_fired(context_id, timer);
}

PresentationId CompositorState::register_presentation(CompositorStateClient& presentation, PresentationCapabilities capabilities)
{
    PresentationId presentation_id { m_next_presentation_id++ };
    m_presentations_by_id.set(presentation_id, &presentation);
    if (auto window_server = WindowServer::try_create_for_presentation())
        m_window_servers.set(&presentation, window_server.release_nonnull());
    m_core.register_presentation(presentation_id,
        {
            .synthesizes_scroll_momentum = capabilities.synthesizes_scroll_momentum,
            .scrolls_elastically = capabilities.scrolls_elastically,
            .scrollbar_track_press_jumps = capabilities.scrollbar_track_press_jumps,
            .has_gpu_presentation_adapter = capabilities.gpu_presentation_adapter_luid.has_value(),
            .gpu_presentation_adapter_luid = capabilities.gpu_presentation_adapter_luid.value_or(0),
            .cannot_present_gpu_images = capabilities.cannot_present_gpu_images,
        });
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

    m_core.unregister_presentation(presentation.presentation_id());
    m_presentations_by_id.remove(presentation.presentation_id());
}

void CompositorState::bind_presentation(Web::CompositorContextId context_id, CompositorStateClient& presentation)
{
    VERIFY(presentation_if_present(presentation.presentation_id()) == &presentation);
    if (!presented_context_if_present(context_id))
        return;
    m_core.bind_presentation(context_id, presentation.presentation_id());
}

Compositing::RemoteLayerHostHandle CompositorState::create_remote_layer(CompositorStateClient& presentation, u64 remote_layer_id)
{
    VERIFY(presentation_if_present(presentation.presentation_id()) == &presentation);
    if (auto existing = m_remote_layers.find(remote_layer_id); existing != m_remote_layers.end()) {
        dbgln("Compositor: Ignoring creation of remote layer {}, which already exists", remote_layer_id);
        return {};
    }
    auto window_server = m_window_servers.get(&presentation);
    if (!window_server.has_value())
        return {};
    auto layer = (*window_server)->try_create_remote_layer(remote_layer_id);
    if (!layer)
        return {};
    auto host_handle = layer->host_handle();
    m_remote_layers.set(remote_layer_id, { .presentation = &presentation, .window_server = **window_server, .layer = move(layer), .shown_context_id = {}, .fit_within = {}, .size_of_shown_frame_laid_out_by_page = {} });
    return host_handle;
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
        if (auto* shown_context = presented_context_if_present(*shown_context_id)) {
            shown_context->remote_layer_id = {};
            m_core.set_shown_in_remote_layer(*shown_context_id, false);
        }
    }
    if (!context_id.has_value())
        return;

    auto* context = presented_context_if_present(*context_id);
    if (!context || !m_core.presents_to_client(*context_id)) {
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
    m_core.set_shown_in_remote_layer(*context_id, true);
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

CompositorStateClient* CompositorState::presentation_for_context(Web::CompositorContextId context_id) const
{
    auto presentation_id = m_core.bound_presentation(context_id);
    if (!presentation_id.has_value())
        return nullptr;
    return presentation_if_present(*presentation_id);
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
    m_core.canvas_content_changed(placeholder.owner->web_content_client_id(), canvas_id);

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
    m_presented_contexts.ensure(context_id, [&] {
        return make<PresentedContext>(web_content_client, OwnedCanvasSurfaces { m_canvas_surface_registry, web_content_client.canvas_surface_owner_id() });
    });
    m_core.create_context(context_id, page_id, web_content_client.web_content_client_id());
}

void CompositorState::destroy_context(Web::CompositorContextId context_id)
{
    auto& context = presented_context(context_id);
    m_core.destroy_context(context_id);
    if (context.remote_layer_id.has_value()) {
        // The layer keeps the context's last frame until another context is shown in it.
        if (auto remote_layer = m_remote_layers.find(*context.remote_layer_id); remote_layer != m_remote_layers.end())
            remote_layer->value.shown_context_id = {};
    }
    if (auto timers = m_context_timers.find(context_id); timers != m_context_timers.end()) {
        for (auto& [_, timer] : timers->value) {
            timer->on_timeout = {};
            timer->stop();
        }
        m_context_timers.remove(timers);
    }
    m_presented_contexts.remove(context_id);
    update_unpainted_video_update_scheduling();
}

bool CompositorState::context_can_be_nested_in(Web::CompositorContextId context_id, Web::CompositorContextId parent_context_id) const
{
    VERIFY(presented_context_if_present(context_id));
    return m_core.context_can_be_nested_in(context_id, parent_context_id);
}

void CompositorState::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
{
    VERIFY(presented_context_if_present(context_id));
    m_core.set_parent_context(context_id, parent_context_id);
}

void CompositorState::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    VERIFY(presented_context_if_present(context_id));
    m_core.stop_presenting_to_client(context_id);
}

bool CompositorState::update_display_list(Web::CompositorContextId context_id, NonnullRefPtr<Compositing::DisplayList> display_list, IPC::Encoded<Optional<Compositing::DisplayList::AsyncScrollingMetadata>> const& async_scrolling_metadata, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction, ReadonlyBytes encoded_scroll_state)
{
    auto& context = presented_context(context_id);

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

    if (!m_core.install_display_list_update(context_id, *display_list, async_scrolling_metadata, visual_context_tree, encoded_scroll_state))
        return false;
    context.raster_targets.apply_resource_transaction(move(resource_transaction));
    resolve_video_sinks(context);
    show_animated_image_frames_in(context);
    update_unpainted_video_update_scheduling();
    return true;
}

void CompositorState::update_display_list_resources(Web::CompositorContextId context_id, Compositing::DisplayListResourceTransaction&& resource_transaction)
{
    auto& context = presented_context(context_id);
    context.raster_targets.apply_resource_transaction(move(resource_transaction));
    show_animated_image_frames_in(context);
}

bool CompositorState::update_visual_context_tree(Web::CompositorContextId context_id, Compositing::AccumulatedVisualContextTree visual_context_tree, Compositing::DisplayListResourceTransaction&& resource_transaction, ReadonlyBytes encoded_scroll_state, IPC::Encoded<Compositing::KeyboardScrollState> const& keyboard_scroll_state)
{
    auto& context = presented_context(context_id);

    switch (m_core.update_visual_context_tree(context_id, visual_context_tree, encoded_scroll_state, keyboard_scroll_state)) {
    case CompositorCore::VisualContextTreeUpdateResult::TreeReplaced:
        context.raster_targets.apply_resource_transaction(move(resource_transaction));
        break;
    case CompositorCore::VisualContextTreeUpdateResult::TreeDropped:
        break;
    case CompositorCore::VisualContextTreeUpdateResult::InvalidScrollState:
        return false;
    }
    show_animated_image_frames_in(context);
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
    for (auto& [_, context] : m_presented_contexts) {
        if (context->web_content_client.web_content_client_id() == client.web_content_client_id())
            resolve_video_sinks(*context);
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
    for (auto& [context_id, context] : m_presented_contexts) {
        if (context->web_content_client.web_content_client_id() == client->web_content_client_id() && context->raster_targets.resource_storage().has_image_frame(id) && m_core.context_is_effectively_visible(context_id))
            return true;
    }
    return false;
}

void CompositorState::show_animated_image_frame(CompositorStateWebContentClient* client, Compositing::ImageFrameResourceId id, Gfx::DecodedImageFrame const& frame)
{
    for (auto& [context_id, context] : m_presented_contexts) {
        auto& resource_storage = context->raster_targets.resource_storage();
        if (context->web_content_client.web_content_client_id() != client->web_content_client_id() || !resource_storage.has_image_frame(id))
            continue;
        resource_storage.replace_image_frame_pixels(id, frame);
        m_core.context_content_changed(context_id);
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
        if (frame.has_value() && context.raster_targets.resource_storage().has_image_frame(id) && !context.raster_targets.resource_storage().image_frame(id).has_same_pixels_as(*frame))
            context.raster_targets.resource_storage().replace_image_frame_pixels(id, *frame);
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
    for (auto& [context_id, context] : m_presented_contexts) {
        if (context->web_content_client.web_content_client_id() != client.web_content_client_id() || !context->raster_targets.draws_video_sink(handle))
            continue;
        // NB: While the video sinks are updated for a display tick, the tick presents the contexts it is told about.
        if (m_contexts_with_new_video_frames)
            m_contexts_with_new_video_frames->append(context_id.value());
        else
            m_core.context_content_changed(context_id);
    }
}

void CompositorState::resolve_video_sinks(PresentedContext& context)
{
    auto& client = context.web_content_client;
    auto& resource_storage = context.raster_targets.resource_storage();
    for (auto const& [id, handle] : resource_storage.video_sink_handles()) {
        auto* sink_state = video_sink_state(client, handle);
        resource_storage.set_video_sink(Compositing::VideoSinkResourceId { id }, sink_state ? sink_state->sink : nullptr);
    }
}

bool CompositorState::video_sink_is_painted_by_any_context(CompositorStateWebContentClient* client, Media::VideoSinkHandle handle) const
{
    for (auto const& [_, context] : m_presented_contexts) {
        if (context->web_content_client.web_content_client_id() == client->web_content_client_id() && context->raster_targets.draws_video_sink(handle))
            return true;
    }
    return false;
}

int CompositorState::unpainted_video_update_interval_ms() const
{
    return max(1, static_cast<int>(1000.0 / m_maximum_display_refresh_rate));
}

void CompositorState::set_maximum_display_refresh_rate(double refresh_rate)
{
    if (m_maximum_display_refresh_rate == refresh_rate)
        return;
    m_maximum_display_refresh_rate = refresh_rate;
    if (m_unpainted_video_update_timer)
        m_unpainted_video_update_timer->set_interval(AK::Duration::from_milliseconds(unpainted_video_update_interval_ms()));
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

bool CompositorState::context_video_sinks_require_updates(Web::CompositorContextId context_id)
{
    auto* context = presented_context_if_present(context_id);
    if (!context)
        return false;
    for (auto const& [_, handle] : context->raster_targets.resource_storage().video_sink_handles()) {
        if (auto* sink_state = video_sink_state(context->web_content_client, handle); sink_state && sink_state->requires_updates)
            return true;
    }
    return false;
}

void CompositorState::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (!presented_context_if_present(context_id))
        return;
    m_core.viewport_size_updated(context_id, viewport_size, device_pixel_ratio, window_resize_in_progress);
}

Optional<OverlayLabel> CompositorState::shape_overlay_label(Web::CompositorContextId context_id, Optional<StringView> font_family, double device_pixel_ratio, StringView text)
{
    auto* context = presented_context_if_present(context_id);
    if (!context)
        return {};
    return context->raster_targets.shape_overlay_label(font_family, device_pixel_ratio, text);
}

void CompositorState::set_ui_overlays(Web::CompositorContextId context_id, Compositing::UIOverlays overlays)
{
    if (!presented_context_if_present(context_id))
        return;
    m_core.set_ui_overlays(context_id, overlays);
}

void CompositorState::set_display_metadata(Web::CompositorContextId context_id, Optional<u64> display_id, double refresh_rate)
{
    if (!presented_context_if_present(context_id))
        return;

    VERIFY(refresh_rate == refresh_rate);
    VERIFY(refresh_rate > 0);
    VERIFY(refresh_rate < AK::Infinity<double>);
    m_core.set_display_metadata(context_id, display_id, refresh_rate);
}

void CompositorState::set_context_visibility(Web::CompositorContextId context_id, Compositing::ContextVisibility visibility)
{
    if (!presented_context_if_present(context_id))
        return;
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} is {}", context_id, visibility == Compositing::ContextVisibility::Visible ? "visible" : "hidden");
    // An image that was hidden finds its place by the clock again.
    if (m_core.set_visibility(context_id, visibility))
        schedule_animated_image_update({});
}

void CompositorState::set_window_server_may_show_high_dynamic_range(bool may_show)
{
    m_core.set_window_server_may_show_high_dynamic_range(may_show);
}

void CompositorState::set_context_presents_from_cpu_memory(Web::CompositorContextId context_id, bool presents_from_cpu_memory)
{
    if (!presented_context_if_present(context_id))
        return;
    m_core.set_presents_from_cpu_memory(context_id, presents_from_cpu_memory);
}

void CompositorState::request_rendering_opportunity(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    VERIFY(presented_context_if_present(context_id));
    m_core.request_rendering_opportunity(context_id, maximum_frames_per_second);
}

void CompositorState::request_clock_tick(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    if (!presented_context_if_present(context_id))
        return;
    m_core.request_clock_tick(context_id, maximum_frames_per_second);
}

void CompositorState::hurry_rendering_opportunity(Web::CompositorContextId context_id)
{
    if (!presented_context_if_present(context_id))
        return;
    m_core.hurry_rendering_opportunity(context_id);
}

void CompositorState::present_frame(Web::CompositorContextId context_id, Gfx::IntRect viewport_rect)
{
    VERIFY(presented_context_if_present(context_id));
    m_core.present_frame_of_page(context_id, viewport_rect);
}

bool CompositorState::request_screenshot(Web::CompositorContextId context_id, Gfx::ShareableBitmap& target_bitmap)
{
    VERIFY(presented_context_if_present(context_id));
    if (!target_bitmap.is_valid() || !target_bitmap.bitmap())
        return false;
    auto target_surface = Gfx::PaintingSurface::wrap_bitmap(*target_bitmap.bitmap());
    return m_core.paint_screenshot(context_id, *target_surface);
}

// How long a screenshot waits for the processes that paint its context's descendants. One whose script never yields
// keeps its connection open without answering, and the screenshot then goes ahead with the frames it last handed over.
static constexpr auto SCREENSHOT_PREPARATION_LIMIT = AK::Duration::from_seconds(10);

void CompositorState::prepare_remote_descendants_for_screenshot(CompositorStateWebContentClient& requester, Web::CompositorContextId context_id, u64 request_id)
{
    HashMap<Web::CompositorContextId, Vector<Web::CompositorContextId>> children;
    for (auto const& [child_id, _] : m_presented_contexts) {
        if (auto parent_context_id = m_core.parent_context(child_id); parent_context_id.has_value())
            children.ensure(*parent_context_id).append(child_id);
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

static Gfx::IntSize size_fitting_within(Gfx::IntSize size, Gfx::IntSize maximum_size)
{
    if (size.width() <= maximum_size.width() && size.height() <= maximum_size.height())
        return size;
    auto scale = min(static_cast<double>(maximum_size.width()) / size.width(), static_cast<double>(maximum_size.height()) / size.height());
    return { max(1, static_cast<int>(round(size.width() * scale))), max(1, static_cast<int>(round(size.height() * scale))) };
}

Optional<Gfx::ShareableBitmap> CompositorState::take_frame_snapshot(Web::CompositorContextId context_id, Optional<Gfx::IntSize> maximum_size)
{
    if (!presented_context_if_present(context_id))
        return {};
    auto presented_frame = m_core.presented_frame_for_snapshot(context_id);
    if (!presented_frame.has_value())
        return {};
    auto const& source = *presented_frame->surface;
    auto frame_size = presented_frame->frame_size;
    auto snapshot_size = frame_size;
    if (maximum_size.has_value() && !maximum_size->is_empty())
        snapshot_size = size_fitting_within(frame_size, *maximum_size);

    auto bitmap_or_error = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, snapshot_size);
    if (bitmap_or_error.is_error())
        return {};
    auto bitmap = bitmap_or_error.release_value();

    if (snapshot_size == frame_size) {
        source.read_into_bitmap(*bitmap);
        return Gfx::ShareableBitmap { move(bitmap), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
    }

    // A linear filter reads only the pixels around each one it writes, so a frame scaled down to a fraction of its size
    // in one step would skip most of them. Halving it first lets every pixel count.
    auto image = source.sk_image_snapshot<sk_sp<SkImage>>();
    auto image_size = frame_size;
    SkSamplingOptions const sampling { SkFilterMode::kLinear };
    auto draw_scaled = [&](Gfx::IntSize size) -> RefPtr<Gfx::PaintingSurface> {
        auto surface_or_error = Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, source.skia_backend_context());
        if (surface_or_error.is_error())
            return nullptr;
        auto surface = surface_or_error.release_value();
        surface->canvas().drawImageRect(image.get(), SkRect::MakeIWH(image_size.width(), image_size.height()), SkRect::MakeIWH(size.width(), size.height()), sampling, nullptr, SkCanvas::kStrict_SrcRectConstraint);
        return surface;
    };
    while (image_size.width() / 2 >= snapshot_size.width() && image_size.height() / 2 >= snapshot_size.height()) {
        Gfx::IntSize half_size { image_size.width() / 2, image_size.height() / 2 };
        auto half = draw_scaled(half_size);
        if (!half)
            return {};
        image = half->sk_image_snapshot<sk_sp<SkImage>>();
        image_size = half_size;
    }
    auto snapshot_surface = draw_scaled(snapshot_size);
    if (!snapshot_surface)
        return {};
    snapshot_surface->read_into_bitmap(*bitmap);
    return Gfx::ShareableBitmap { move(bitmap), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
}

void CompositorState::presented_bitmap_ready_to_paint(Web::CompositorContextId context_id, i32 bitmap_id)
{
    if (!presented_context_if_present(context_id))
        return;
    m_core.presented_store_released(context_id, bitmap_id);
}

void CompositorState::set_scrollbar_track_press_jumps(CompositorStateClient& presentation, bool jumps)
{
    m_core.set_scrollbar_track_press_jumps(presentation.presentation_id(), jumps);
}

void CompositorState::set_client_gpu_presentation_capability(CompositorStateClient& presentation, bool supported, u64 adapter_luid)
{
    m_core.set_presentation_gpu_capability(presentation.presentation_id(), supported, adapter_luid);
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

CompositorState::PresentedContext& CompositorState::presented_context(Web::CompositorContextId context_id)
{
    auto* context = presented_context_if_present(context_id);
    VERIFY(context);
    return *context;
}

Compositing::RustFFI::FfiRasterPainter CompositorState::begin_painting(Web::CompositorContextId context_id, Gfx::PaintingSurface& surface, RustFFI::PaintingResources resources)
{
    auto& raster_targets = presented_context(context_id).raster_targets;
    if (m_painting_depth == m_display_list_players.size())
        m_display_list_players.append(make<DisplayListPlayerSkia>(m_skia_backend_context));
    auto& player = *m_display_list_players[m_painting_depth++];
    switch (resources) {
    case RustFFI::PaintingResources::Page:
        return player.begin_painting(surface, raster_targets.resource_storage(), raster_targets.raster_cache(), &raster_targets.canvas_surfaces());
    case RustFFI::PaintingResources::Overlays:
        return player.begin_painting(surface, raster_targets.overlay_resource_storage(), raster_targets.overlay_raster_cache(), nullptr);
    }
    VERIFY_NOT_REACHED();
}

void CompositorState::end_painting()
{
    VERIFY(m_painting_depth > 0);
    m_display_list_players[--m_painting_depth]->end_painting();
}

static BackingStoreManager::PublicationMode publication_mode_of(RustFFI::StorePublication publication)
{
    switch (publication) {
    case RustFFI::StorePublication::Unpublished:
        return BackingStoreManager::PublicationMode::Unpublished;
    case RustFFI::StorePublication::Published:
        return BackingStoreManager::PublicationMode::Published;
    case RustFFI::StorePublication::ShownInRemoteLayer:
        return BackingStoreManager::PublicationMode::ShownInRemoteLayer;
    }
    VERIFY_NOT_REACHED();
}

RustFFI::StoreResizeOutcome CompositorState::resize_stores(Web::CompositorContextId context_id, RustFFI::FfiStoreResize const& resize)
{
    auto& backing_stores = presented_context(context_id).raster_targets.backing_stores();
    auto publication_mode = publication_mode_of(resize.publication);
    auto window_resize_in_progress = resize.window_resize_in_progress ? Compositing::WindowResizingInProgress::Yes : Compositing::WindowResizingInProgress::No;
    auto allocation = backing_stores.resize_backing_stores_if_needed(resize.raster_size, window_resize_in_progress, publication_mode);
    if (!allocation.has_value())
        return RustFFI::StoreResizeOutcome::Unchanged;

    RefPtr<WindowServer> window_server_showing_stores;
    if (publication_mode == BackingStoreManager::PublicationMode::ShownInRemoteLayer) {
        auto* remote_layer = remote_layer_showing(context_id);
        VERIFY(remote_layer);
        window_server_showing_stores = remote_layer->window_server;
    }
    auto gpu_sharing = resize.gpu_sharing_allowed ? BackingStoreManager::GpuSharing::Allowed : BackingStoreManager::GpuSharing::Disallowed;
    auto publication = backing_stores.allocate_backing_stores(*allocation, m_skia_backend_context, publication_mode, gpu_sharing, move(window_server_showing_stores));
    if (publication.is_error()) {
        dbgln("Compositor: Failed to allocate the stores of context {} that a remote layer shows, which its presentation shows again: {}", context_id, publication.error());
        return RustFFI::StoreResizeOutcome::Failed;
    }
    if (!publication.value().has_value())
        return RustFFI::StoreResizeOutcome::ReallocatedOffscreen;
    if (publication_mode == BackingStoreManager::PublicationMode::Published) {
        auto* presentation = resize.has_presentation ? presentation_if_present(PresentationId { resize.presentation_id }) : nullptr;
        VERIFY(presentation);
        auto stores = publication.release_value().release_value();
        presentation->did_allocate_backing_stores(context_id, move(stores.bitmap_ids), move(stores.shared_images));
    }
    return RustFFI::StoreResizeOutcome::ReallocatedForViewer;
}

bool CompositorState::add_store_if_window_server_reads_every_released_store(Web::CompositorContextId context_id, Optional<PresentationId> publishing_presentation)
{
    auto& backing_stores = presented_context(context_id).raster_targets.backing_stores();
    auto publication = backing_stores.add_backing_store_if_window_server_still_reads_every_released_store(m_skia_backend_context);
    if (!publication.has_value())
        return false;
    dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} gained backing store {}, as the window server still reads every released one", context_id, publication->bitmap_ids);
    if (auto* presentation = publishing_presentation.has_value() ? presentation_if_present(*publishing_presentation) : nullptr)
        presentation->did_add_backing_stores(context_id, move(publication->bitmap_ids), move(publication->shared_images));
    return true;
}

bool CompositorState::retire_idle_surplus_stores(Web::CompositorContextId context_id, Optional<PresentationId> publishing_presentation)
{
    auto& backing_stores = presented_context(context_id).raster_targets.backing_stores();
    auto retired_bitmap_ids = backing_stores.retire_idle_surplus_backing_stores();
    if (auto* presentation = publishing_presentation.has_value() ? presentation_if_present(*publishing_presentation) : nullptr; presentation && !retired_bitmap_ids.is_empty()) {
        dbgln_if(COMPOSITOR_DEBUG, "[Compositor] Context {} retired idle backing stores {}", context_id, retired_bitmap_ids);
        presentation->did_retire_backing_stores(context_id, move(retired_bitmap_ids));
    }
    return backing_stores.has_surplus_backing_stores();
}

static bool video_frame_holds_high_dynamic_range(Media::VideoFrame const& frame)
{
    auto transfer_characteristics = frame.cicp().transfer_characteristics();
    return transfer_characteristics == Media::TransferCharacteristics::SMPTE2084 || transfer_characteristics == Media::TransferCharacteristics::HLG;
}

void CompositorState::report_live_content(Web::CompositorContextId context_id, ReadonlySpan<u64> canvas_ids, void* receiver, void (*receive)(void*, RustFFI::FfiLiveContent const*))
{
    auto& raster_targets = presented_context(context_id).raster_targets;
    auto const& resource_storage = raster_targets.resource_storage();

    Vector<RustFFI::FfiCanvasContent> canvases;
    canvases.ensure_capacity(canvas_ids.size());
    for (auto id : canvas_ids) {
        Compositing::CanvasId canvas_id { id };
        canvases.unchecked_append({
            .id = id,
            .content_generation = raster_targets.canvas_surfaces().canvas_content_generation(canvas_id),
            .has_presentable_surface = raster_targets.canvas_surfaces().presentable_canvas_surface(canvas_id) != nullptr,
        });
    }
    Vector<RustFFI::FfiVideoSinkContent> video_sinks;
    video_sinks.ensure_capacity(resource_storage.video_sink_handles().size());
    for (auto const& [id, handle] : resource_storage.video_sink_handles()) {
        auto frame = raster_targets.current_video_frame(Compositing::VideoSinkResourceId { id });
        video_sinks.unchecked_append({
            .id = id,
            .has_frame = frame != nullptr,
            .frame_id = frame ? frame->id() : 0,
            .frame_holds_high_dynamic_range = frame && video_frame_holds_high_dynamic_range(*frame),
            .frame_has_surface = frame && frame->surface(),
        });
    }
    Vector<RustFFI::FfiImageFrameContent> image_frames;
    image_frames.ensure_capacity(resource_storage.image_frame_pixel_generations().size());
    for (auto const& [id, pixel_generation] : resource_storage.image_frame_pixel_generations())
        image_frames.unchecked_append({ .id = id, .pixel_generation = pixel_generation });

    RustFFI::FfiLiveContent content {
        .canvases = canvases.span(),
        .video_sinks = video_sinks.span(),
        .image_frames = image_frames.span(),
    };
    receive(receiver, &content);
}

CompositorState::RemoteLayerState* CompositorState::remote_layer_showing(Web::CompositorContextId context_id)
{
    auto* context = presented_context_if_present(context_id);
    if (!context || !context->remote_layer_id.has_value())
        return nullptr;
    auto remote_layer = m_remote_layers.find(*context->remote_layer_id);
    if (remote_layer == m_remote_layers.end())
        return nullptr;
    return &remote_layer->value;
}

Optional<RemoteLayer::Content> CompositorState::remote_layer_content(PresentedContext& context, RustFFI::MediaKind kind, u64 media_id)
{
    switch (kind) {
    case RustFFI::MediaKind::Canvas:
        if (auto surface = context.raster_targets.canvas_surfaces().presentable_canvas_surface(Compositing::CanvasId { media_id }))
            return RemoteLayer::Content { surface.release_nonnull() };
        return {};
    case RustFFI::MediaKind::Video:
        if (auto frame = context.raster_targets.current_video_frame(Compositing::VideoSinkResourceId { media_id }))
            return RemoteLayer::Content { frame.release_nonnull() };
        return {};
    }
    VERIFY_NOT_REACHED();
}

static Optional<RemoteSublayer::RoundedClip> rounded_clip_of(RustFFI::FfiSublayerPlacement const& placement)
{
    if (!placement.has_rounded_clip)
        return {};
    return RemoteSublayer::RoundedClip {
        .rect = placement.rounded_clip_rect,
        .radius = placement.rounded_clip_radius,
        .rounds_top_left = placement.rounds_top_left,
        .rounds_top_right = placement.rounds_top_right,
        .rounds_bottom_right = placement.rounds_bottom_right,
        .rounds_bottom_left = placement.rounds_bottom_left,
    };
}

static Gfx::AffineTransform content_to_frame_of(RustFFI::FfiSublayerPlacement const& placement)
{
    auto const* transform = placement.content_to_frame;
    return { transform[0], transform[1], transform[2], transform[3], transform[4], transform[5] };
}

bool CompositorState::remote_layer_can_show(Web::CompositorContextId context_id, RustFFI::MediaKind kind, u64 media_id, RustFFI::FfiSublayerPlacement const& placement, double device_pixel_ratio)
{
    auto* remote_layer = remote_layer_showing(context_id);
    if (!remote_layer)
        return false;
    auto content = remote_layer_content(presented_context(context_id), kind, media_id);
    if (!content.has_value())
        return false;
    WindowServerPlacement window_server_placement {
        .content_rect = placement.content_rect,
        .content_to_viewport = content_to_frame_of(placement),
        .clip_rect = placement.clip_rect,
        .rounded_clip = rounded_clip_of(placement),
        .scaling_mode = placement.scaling_mode,
    };
    return remote_layer->layer->can_show(window_server_placement, *content, device_pixel_ratio);
}

Vector<RemoteSublayer>* CompositorState::build_sublayers(Web::CompositorContextId context_id, ReadonlySpan<RustFFI::FfiSublayer> sublayers_to_build)
{
    auto sublayers = make<Vector<RemoteSublayer>>();
    auto& raster_targets = presented_context(context_id).raster_targets;
    for (auto const& sublayer : sublayers_to_build) {
        Optional<Variant<NonnullRefPtr<Media::VideoFrame>, PresentableSurface::Lease>> content;
        switch (sublayer.content_kind) {
        case RustFFI::SublayerContentKind::Canvas:
            if (auto surface = raster_targets.canvas_surfaces().presentable_canvas_surface(Compositing::CanvasId { sublayer.media_id }))
                content = surface->lease();
            break;
        case RustFFI::SublayerContentKind::Video:
            if (auto frame = raster_targets.current_video_frame(Compositing::VideoSinkResourceId { sublayer.media_id }); frame && frame->surface())
                content = frame.release_nonnull();
            break;
        case RustFFI::SublayerContentKind::PresentableSurface:
            content = static_cast<PresentableSurface*>(sublayer.presentable_surface)->lease();
            break;
        }
        if (!content.has_value())
            continue;
        auto const& placement = sublayer.placement;
        sublayers->append({
            .identity = sublayer.identity,
            .content = content.release_value(),
            .content_rect = placement.content_rect,
            .content_to_frame = content_to_frame_of(placement),
            .clip_rect = placement.clip_rect,
            .rounded_clip = rounded_clip_of(placement),
            .scaling_mode = placement.scaling_mode,
        });
    }
    return sublayers.leak_ptr();
}

WindowServer::FrameHandOff CompositorState::frame_hand_off(Web::CompositorContextId context_id)
{
    if (auto* remote_layer = remote_layer_showing(context_id))
        return remote_layer->window_server->frame_hand_off();
    return WindowServer::frame_hand_off_through_presentation();
}

u64 CompositorState::submit_frame(Gfx::PaintingSurface* surface)
{
    auto token = m_next_gpu_work_token++;
    m_gpu_work_in_flight.set(token);
    auto& event_loop = Core::EventLoop::current();
    auto did_finish = [self = NonnullRefPtr { *this }, &event_loop, token] {
        event_loop.deferred_invoke([self, token] {
            self->gpu_work_finished(token);
        });
    };
    if (surface)
        DisplayListPlayerSkia::flush_async(*surface, move(did_finish));
    else if (m_skia_backend_context)
        m_skia_backend_context->flush_and_submit_async(move(did_finish));
    else
        did_finish();
    schedule_gpu_completion_check();
    return token;
}

void CompositorState::gpu_work_finished(u64 token)
{
    m_gpu_work_in_flight.remove(token);
    if (m_gpu_work_in_flight.is_empty() && m_gpu_completion_timer)
        m_gpu_completion_timer->stop();
    m_core.gpu_work_finished(token);
}

void CompositorState::hand_off_once_gpu_work_is_scheduled([[maybe_unused]] u64 token)
{
#if defined(AK_OS_MACOS) || (defined(AK_OS_WINDOWS) && defined(USE_DIRECTX))
    auto& event_loop = Core::EventLoop::current();
    auto deliver = [self = NonnullRefPtr { *this }, &event_loop, token] {
        event_loop.deferred_invoke([self, token] {
            self->m_core.frame_ready_for_hand_off(token);
        });
    };
#endif
#ifdef AK_OS_MACOS
    // The window server's reads of an IOSurface wait for GPU work that is already scheduled to write it.
    m_skia_backend_context->metal_context().when_committed_work_is_scheduled(move(deliver));
#elif defined(AK_OS_WINDOWS) && defined(USE_DIRECTX)
    deliver();
#else
    VERIFY_NOT_REACHED();
#endif
}

Optional<PresentationId> CompositorState::show_frame_in_remote_layer(Web::CompositorContextId context_id, RustFFI::FfiRemoteLayerFrame const& frame, NonnullOwnPtr<Vector<RemoteSublayer>> sublayers)
{
    auto* remote_layer = remote_layer_showing(context_id);
    if (!remote_layer)
        return {};
    auto& context = presented_context(context_id);
    auto* store = frame.uses_store ? context.raster_targets.backing_stores().window_server_surface_of_latest_rendered_store() : nullptr;
    VERIFY(store || !frame.uses_store);

    auto remote_layer_id = *context.remote_layer_id;
    auto shown = remote_layer->layer->show_frame({
        .store = store ? Optional<PresentableSurface::Lease> { store->lease() } : OptionalNone {},
        .painted_size = frame.painted_size,
        .device_pixel_ratio = frame.device_pixel_ratio,
        .fit_within = remote_layer->fit_within,
        .sublayers = move(*sublayers),
    });
    if (!shown) {
        dbgln("Compositor: Remote layer {} failed to show a frame of context {}, which its presentation shows again", remote_layer_id, context_id);
        stop_showing_in_remote_layer(context_id);
        return {};
    }
    remote_layer->size_of_shown_frame_laid_out_by_page = frame.laid_out_by_page ? Optional<Gfx::IntSize> { frame.painted_size } : OptionalNone {};
    report_held_synchronized_resize_frames(remote_layer_id);
    return remote_layer->presentation->presentation_id();
}

void CompositorState::stop_showing_in_remote_layer(Web::CompositorContextId context_id)
{
    auto& context = presented_context(context_id);
    auto remote_layer_id = exchange(context.remote_layer_id, {});
    if (!remote_layer_id.has_value())
        return;
    if (auto remote_layer = m_remote_layers.find(*remote_layer_id); remote_layer != m_remote_layers.end()) {
        remote_layer->value.shown_context_id = {};
        remote_layer->value.size_of_shown_frame_laid_out_by_page = {};
    }
}

void CompositorState::release_store_later(Web::CompositorContextId context_id, i32 bitmap_id)
{
    Core::deferred_invoke([self = NonnullRefPtr { *this }, context_id, bitmap_id] {
        self->presented_bitmap_ready_to_paint(context_id, bitmap_id);
    });
}

static void log_presented_frame(u64 page_id, Gfx::IntRect damage)
{
    auto* log = Core::StatsLog::the();
    if (!log)
        return;
    JsonObject data;
    data.set("page_id"sv, page_id);
    data.set("changed"sv, !damage.is_empty());
    data.set("damage_x"sv, damage.x());
    data.set("damage_y"sv, damage.y());
    data.set("damage_width"sv, damage.width());
    data.set("damage_height"sv, damage.height());
    log->event("frame"sv, move(data));
}

void CompositorState::schedule_gpu_completion_check()
{
    if (!m_skia_backend_context || m_gpu_work_in_flight.is_empty())
        return;
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
    if (m_gpu_work_in_flight.is_empty()) {
        if (m_gpu_completion_timer)
            m_gpu_completion_timer->stop();
        return;
    }

    if (m_skia_backend_context)
        m_skia_backend_context->check_async_work_completion();

    if (m_gpu_work_in_flight.is_empty() && m_gpu_completion_timer)
        m_gpu_completion_timer->stop();
}

VSyncScheduler& CompositorState::vsync_scheduler_for_display(Optional<u64> display_id)
{
    return *m_vsync_schedulers_by_display.ensure(display_id, [this, display_id] {
        return create_vsync_scheduler(display_id, [this, display_id](MonotonicTime frame_time) {
            m_core.vsync(display_id, frame_time);
        });
    });
}

static CompositorState& state_of(void* host)
{
    return *static_cast<CompositorState*>(host);
}

static Gfx::PaintingSurface& painting_surface_of(void* surface)
{
    return *static_cast<Gfx::PaintingSurface*>(surface);
}

static SkCanvas& canvas_of(void* surface)
{
    return painting_surface_of(surface).canvas();
}

static Optional<u64> display_of(bool has_display_id, u64 display_id)
{
    if (!has_display_id)
        return {};
    return display_id;
}

static Optional<PresentationId> presentation_of(bool has_presentation, u64 presentation_id)
{
    if (!has_presentation)
        return {};
    return PresentationId { presentation_id };
}

static MonotonicTime time_of(i64 nanoseconds)
{
    return MonotonicTime {} + AK::Duration::from_nanoseconds(nanoseconds);
}

RustFFI::FfiRasterHost CompositorState::raster_host(CompositorState& state)
{
    using Context = Web::CompositorContextId;
    return {
        .host = &state,
        .surface_retain = [](void* surface) { painting_surface_of(surface).ref(); },
        .surface_release = [](void* surface) { painting_surface_of(surface).unref(); },
        .surface_size = [](void* surface, Gfx::IntSize* size) { *size = painting_surface_of(surface).size(); },
        .create_surface = [](void* host, Gfx::IntSize size, void* like) -> void* {
            auto skia_backend_context = like ? painting_surface_of(like).skia_backend_context() : state_of(host).m_skia_backend_context;
            auto surface = Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context);
            if (surface.is_error())
                return nullptr;
            return &surface.release_value().leak_ref();
        },
        .surfaces_share_backend = [](void* a, void* b) { return painting_surface_of(a).skia_backend_context() == painting_surface_of(b).skia_backend_context(); },
        .surface_uses_gpu = [](void* surface) { return painting_surface_of(surface).skia_backend_context() != nullptr; },
        .surface_save = [](void* surface) { return canvas_of(surface).save(); },
        .surface_restore_to_count = [](void* surface, i32 count) { canvas_of(surface).restoreToCount(count); },
        .surface_translate = [](void* surface, float x, float y) { canvas_of(surface).translate(x, y); },
        .surface_scale = [](void* surface, float x, float y) { canvas_of(surface).scale(x, y); },
        .surface_clip_rect = [](void* surface, Gfx::IntRect rect) { canvas_of(surface).clipIRect(SkIRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height())); },
        .surface_clear = [](void* surface, Gfx::Color color) { canvas_of(surface).clear(Gfx::to_skia_color(color)); },
        .surface_draw_surface = [](void* target, void* source, Gfx::IntRect source_rect, Gfx::IntRect target_rect, RustFFI::SurfaceBlend blend) {
            auto image = painting_surface_of(source).sk_image_snapshot<sk_sp<SkImage>>();
            SkPaint paint;
            paint.setBlendMode(blend == RustFFI::SurfaceBlend::Source ? SkBlendMode::kSrc : SkBlendMode::kSrcOver);
            canvas_of(target).drawImageRect(image.get(), Gfx::to_skia_rect(source_rect), Gfx::to_skia_rect(target_rect), SkSamplingOptions {}, &paint, SkCanvas::kStrict_SrcRectConstraint); },
        .flush_surface = [](void*, void* surface) { DisplayListPlayerSkia::flush(painting_surface_of(surface)); },
        .begin_painting = [](void* host, u64 context_id, void* surface, RustFFI::PaintingResources resources, Compositing::RustFFI::FfiRasterPainter* painter) { *painter = state_of(host).begin_painting(Context { context_id }, painting_surface_of(surface), resources); },
        .end_painting = [](void* host) { state_of(host).end_painting(); },
        .start_raster_cache_frame = [](void* host, u64 context_id) { state_of(host).presented_context(Context { context_id }).raster_targets.raster_cache().drop_what_was_not_used_since_last_frame(); },
        .store_state = [](void* host, u64 context_id) -> RustFFI::FfiStoreState {
            auto const& backing_stores = state_of(host).presented_context(Context { context_id }).raster_targets.backing_stores();
            return { .is_valid = backing_stores.is_valid(), .is_rendering = backing_stores.is_rendering(), .has_available_store = backing_stores.has_available_buffer() };
        },
        .resize_stores = [](void* host, u64 context_id, RustFFI::FfiStoreResize const* resize) { return state_of(host).resize_stores(Context { context_id }, *resize); },
        .invalidate_stores = [](void* host, u64 context_id) { state_of(host).presented_context(Context { context_id }).raster_targets.backing_stores().invalidate(); },
        .acquire_store = [](void* host, u64 context_id, RustFFI::FfiAcquiredStore* acquired) {
            auto render_target = state_of(host).presented_context(Context { context_id }).raster_targets.backing_stores().acquire_render_target();
            if (!render_target.has_value())
                return false;
            acquired->bitmap_id = render_target->bitmap_id;
            acquired->surface = &render_target->surface;
            return true; },
        .complete_rendering = [](void* host, u64 context_id, i32 bitmap_id) { state_of(host).presented_context(Context { context_id }).raster_targets.backing_stores().complete_rendering(bitmap_id); },
        .release_store = [](void* host, u64 context_id, i32 bitmap_id) { return state_of(host).presented_context(Context { context_id }).raster_targets.backing_stores().release_buffer(bitmap_id); },
        .release_all_presented_stores = [](void* host, u64 context_id) { state_of(host).presented_context(Context { context_id }).raster_targets.backing_stores().release_all_presented_buffers(); },
        .latest_rendered_store_surface = [](void* host, u64 context_id) -> void* { return state_of(host).presented_context(Context { context_id }).raster_targets.backing_stores().latest_rendered_surface().ptr(); },
        .add_store_if_window_server_reads_every_released_store = [](void* host, u64 context_id, bool has_presentation, u64 presentation_id) { return state_of(host).add_store_if_window_server_reads_every_released_store(Context { context_id }, presentation_of(has_presentation, presentation_id)); },
        .retire_idle_surplus_stores = [](void* host, u64 context_id, bool has_presentation, u64 presentation_id) { return state_of(host).retire_idle_surplus_stores(Context { context_id }, presentation_of(has_presentation, presentation_id)); },
        .next_presentable_surface = [](void* host, u64 context_id, u32 pool, Gfx::IntSize size, void* like) -> void* {
            auto& state = state_of(host);
            auto skia_backend_context = like ? painting_surface_of(like).skia_backend_context() : state.m_skia_backend_context;
            auto surface = state.presented_context(Context { context_id }).raster_targets.next_presentable_surface(pool, size, skia_backend_context);
            if (!surface)
                return nullptr;
            return surface.leak_ref();
        },
        .drop_presentable_surface_pool = [](void* host, u64 context_id, u32 pool) { state_of(host).presented_context(Context { context_id }).raster_targets.drop_presentable_surface_pool(pool); },
        .presentable_surface_retain = [](void* surface) { static_cast<PresentableSurface*>(surface)->ref(); },
        .presentable_surface_release = [](void* surface) { static_cast<PresentableSurface*>(surface)->unref(); },
        .presentable_surface_painting_surface = [](void* surface) -> void* { return &static_cast<PresentableSurface*>(surface)->painting_surface(); },
        .live_content = [](void* host, u64 context_id, ReadonlySpan<u64> canvas_ids, void* receiver, void (*receive)(void*, RustFFI::FfiLiveContent const*)) { state_of(host).report_live_content(Context { context_id }, canvas_ids, receiver, receive); },
        .remote_layer_can_show = [](void* host, u64 context_id, RustFFI::MediaKind kind, u64 media_id, RustFFI::FfiSublayerPlacement const* placement, double device_pixel_ratio) { return state_of(host).remote_layer_can_show(Context { context_id }, kind, media_id, *placement, device_pixel_ratio); },
        .remote_layer_can_show_high_dynamic_range = [](void* host, u64 context_id) {
            auto* remote_layer = state_of(host).remote_layer_showing(Context { context_id });
            return remote_layer && remote_layer->layer->can_show_high_dynamic_range(); },
        .build_sublayers = [](void* host, u64 context_id, ReadonlySpan<RustFFI::FfiSublayer> sublayers) -> void* {
            return state_of(host).build_sublayers(Context { context_id }, sublayers);
        },
        .destroy_sublayers = [](void* sublayers) { delete static_cast<Vector<RemoteSublayer>*>(sublayers); },
        .frame_hand_off = [](void* host, u64 context_id) { return state_of(host).frame_hand_off(Context { context_id }) == WindowServer::FrameHandOff::OnceGpuWorkIsScheduled
                                                               ? RustFFI::FrameHandOff::OnceGpuWorkIsScheduled
                                                               : RustFFI::FrameHandOff::OnceGpuWorkIsFinished; },
        .submit_frame = [](void* host, u64, void* surface) { return state_of(host).submit_frame(static_cast<Gfx::PaintingSurface*>(surface)); },
        .hand_off_once_gpu_work_is_scheduled = [](void* host, u64 token) { state_of(host).hand_off_once_gpu_work_is_scheduled(token); },
        .show_frame_in_remote_layer = [](void* host, u64 context_id, RustFFI::FfiRemoteLayerFrame const* frame, void* sublayers, u64* presentation_id) {
            auto shown_in = state_of(host).show_frame_in_remote_layer(Context { context_id }, *frame, adopt_own(*static_cast<Vector<RemoteSublayer>*>(sublayers)));
            if (!shown_in.has_value())
                return false;
            *presentation_id = shown_in->value();
            return true; },
        .stop_showing_in_remote_layer = [](void* host, u64 context_id) { state_of(host).stop_showing_in_remote_layer(Context { context_id }); },
        .log_presented_frame = [](void*, u64 page_id, Gfx::IntRect damage_rect) { log_presented_frame(page_id, damage_rect); },
        .release_store_later = [](void* host, u64 context_id, i32 bitmap_id) { state_of(host).release_store_later(Context { context_id }, bitmap_id); },
        .request_vsync = [](void* host, bool has_display_id, u64 display_id, double refresh_rate) { state_of(host).vsync_scheduler_for_display(display_of(has_display_id, display_id)).schedule(refresh_rate); },
        .most_recent_tick_time = [](void* host, bool has_display_id, u64 display_id, i64 now_nanoseconds, double refresh_rate, i64* tick_time_nanoseconds) {
            auto tick_time = state_of(host).vsync_scheduler_for_display(display_of(has_display_id, display_id)).most_recent_tick_time(time_of(now_nanoseconds), refresh_rate);
            if (!tick_time.has_value())
                return false;
            *tick_time_nanoseconds = tick_time->nanoseconds();
            return true; },
        .update_video_sinks = [](void* host, void* receiver, void (*receive)(void*, ReadonlySpan<u64>)) {
            auto& state = state_of(host);
            Vector<u64> contexts_with_new_video_frames;
            {
                TemporaryChange collecting { state.m_contexts_with_new_video_frames, &contexts_with_new_video_frames };
                (void)state.update_all_video_sinks();
            }
            receive(receiver, contexts_with_new_video_frames.span()); },
        .context_video_sinks_require_updates = [](void* host, u64 context_id) { return state_of(host).context_video_sinks_require_updates(Context { context_id }); },
        .set_maximum_display_refresh_rate = [](void* host, double refresh_rate) { state_of(host).set_maximum_display_refresh_rate(refresh_rate); },
    };
}

}
