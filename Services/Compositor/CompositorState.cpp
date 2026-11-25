/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <AK/Random.h>
#include <AK/StdLibExtras.h>
#include <AK/TemporaryChange.h>
#include <Compositor/BackingStoreFactory.h>
#include <Compositor/Canvas2DCommandPlayer.h>
#include <Compositor/CanvasHost.h>
#include <Compositor/CompositorState.h>
#include <Compositor/OpenGLContext.h>
#include <Compositor/SkiaUtils.h>
#include <Compositor/VideoFrameSkia.h>
#include <LibCompositing/CanvasColorSpace.h>
#include <LibCompositing/DisplayList/DisplayListCommand.h>
#include <LibCompositing/DisplayList/RegisteredImageFrames.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/EventLoop.h>
#include <LibCore/StatsLog.h>
#include <LibCore/Timer.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ExportFormat.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMediaCodecs/VideoFrame.h>
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

IPC::RawMessage::OutcomeInRust CompositorState::handle_message_from_control(IPC::RawMessage& message)
{
    return m_core.handle_message_from_control(message);
}

IPC::RawMessage::OutcomeInRust CompositorState::handle_message_from_render_clock(WebContentClientId client_id, IPC::RawMessage& message)
{
    return m_core.handle_message_from_render_clock(client_id, message);
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

void CompositorState::set_timer(u64 timer_number, AK::Duration delay)
{
    auto& timer = m_timers.ensure(timer_number, [&] {
        return Core::Timer::create_single_shot(delay, [this, timer_number] {
            m_core.timer_fired(timer_number);
        });
    });
    timer->restart(delay);
}

void CompositorState::cancel_timer(u64 timer_number)
{
    if (auto timer = m_timers.get(timer_number); timer.has_value())
        (*timer)->stop();
}

void CompositorState::destroy_timer(u64 timer_number)
{
    auto timer = m_timers.take(timer_number);
    if (!timer.has_value())
        return;
    (*timer)->stop();
    // NB: The timer may be the one whose callback is running.
    Core::deferred_invoke([timer = timer.release_value()] { });
}

PresentationId CompositorState::register_presentation(CompositorStateClient& presentation, PresentationCapabilities capabilities)
{
    PresentationId presentation_id { m_next_presentation_id++ };
    m_presentations_by_id.set(presentation_id, &presentation);
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
    m_core.unregister_presentation(presentation.presentation_id());
    m_presentations_by_id.remove(presentation.presentation_id());
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
    m_core.forget_web_content_client(client.web_content_client_id());
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
    m_staged_rasterizations.remove(client.web_content_client_id());

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
    m_pending_screenshots.remove_all_matching([&](auto, auto const& screenshot) {
        return screenshot.requester == client.web_content_client_id();
    });
    Vector<u64> preparations_awaiting_client;
    for (auto& [preparation_id, preparation] : m_screenshot_preparations) {
        if (preparation.awaited_painters.remove(client.web_content_client_id()))
            preparations_awaiting_client.append(preparation_id);
    }
    m_web_content_clients.remove(client.web_content_client_id());
    for (auto preparation_id : preparations_awaiting_client)
        finish_screenshot_preparation_if_done(preparation_id);
    paint_screenshots_whose_sinks_have_caught_up();
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
    VERIFY(presented_context_if_present(context_id));
    m_core.destroy_context(context_id);
    m_presented_contexts.remove(context_id);
    update_unpainted_video_update_scheduling();
}

bool CompositorState::describe_display_list_fonts(Web::CompositorContextId context_id, ReadonlySpan<Compositing::DisplayListFontDescription> fonts, Compositing::RegisteredTypefaces const& registered_typefaces)
{
    return presented_context(context_id).raster_targets.fonts().add_described_fonts_made_of_registered_typefaces(fonts, registered_typefaces);
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

RefPtr<Media::VideoFrame> CompositorState::current_video_frame(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle)
{
    auto* sink_state = video_sink_state(client, handle);
    if (!sink_state || !sink_state->sink)
        return nullptr;
    return sink_state->sink->current_frame();
}

void CompositorState::add_video_sink(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle)
{
    auto& sinks = m_video_sink_states.ensure(&client, [] { return HashMap<Media::VideoSinkHandle, VideoSinkState> {}; });
    if (sinks.contains(handle))
        return;
    sinks.set(handle, VideoSinkState { .sink = nullptr, .requires_updates = false, .was_ticking = false, .published_frame_is_up_to_date = false, .up_to_date_request_token = {} });
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
    if (auto client_sinks = m_video_sink_states.get(&client); client_sinks.has_value()) {
        // The client gave up on the sink, so it isn't waiting for it either.
        if (auto sink_state = client_sinks->get(handle); sink_state.has_value() && sink_state->up_to_date_request_token.has_value())
            client.tell_video_sink_is_up_to_date(*sink_state->up_to_date_request_token);
        client_sinks->remove(handle);
    }
    client.release_video_edge(handle);
    paint_screenshots_whose_sinks_have_caught_up();
}

void CompositorState::video_sink_freshness_changed(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle)
{
    auto* sink_state = video_sink_state(client, handle);
    if (!sink_state || !sink_state->sink)
        return;
    auto is_ticking = sink_state->sink->is_ticking();
    if (is_ticking != exchange(sink_state->was_ticking, is_ticking)) {
        if (is_ticking)
            present_contexts_drawing_video_sink(client, handle);
        update_unpainted_video_update_scheduling();
    }
    publish_video_sink_freshness(client, handle, *sink_state);
}

void CompositorState::publish_video_sink_freshness(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle, VideoSinkState& sink_state)
{
    // Whatever changed the sink's frame may also have finished a screenshot's wait for it.
    paint_screenshots_whose_sinks_have_caught_up();

    auto frame_is_up_to_date = sink_state.sink && sink_state.sink->frame_is_up_to_date();
    if (frame_is_up_to_date != sink_state.published_frame_is_up_to_date) {
        sink_state.published_frame_is_up_to_date = frame_is_up_to_date;
        client.video_sink_frame_freshness_changed(handle, frame_is_up_to_date);
    }
    if (!frame_is_up_to_date)
        return;
    if (auto token = exchange(sink_state.up_to_date_request_token, {}); token.has_value())
        client.tell_video_sink_is_up_to_date(*token);
}

void CompositorState::bring_video_sink_up_to_date(CompositorStateWebContentClient& client, Media::VideoSinkHandle handle, u64 token)
{
    auto* sink_state = video_sink_state(client, handle);
    if (!sink_state || (sink_state->sink && sink_state->sink->frame_is_up_to_date())) {
        client.tell_video_sink_is_up_to_date(token);
        return;
    }
    if (auto superseded_token = exchange(sink_state->up_to_date_request_token, token); superseded_token.has_value())
        client.tell_video_sink_is_up_to_date(*superseded_token);
}

// The sink's owner decides on ticking, and the sink hears of it across its edge. A sink with no edge yet cannot be
// ticked, which can only delay ticking rather than stop it.
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
    sink->set_on_present_needed([this, &client, handle] {
        auto* sink_state = video_sink_state(client, handle);
        if (sink_state && sink_state->sink)
            client.video_sink_presented_frame(handle, sink_state->sink->current_frame(), sink_state->sink->take_frame_counts());
        present_contexts_drawing_video_sink(client, handle);
        update_unpainted_video_update_scheduling();
        if (sink_state)
            publish_video_sink_freshness(client, handle, *sink_state);
    });
    sink->set_on_freshness_changed([this, &client, handle] {
        video_sink_freshness_changed(client, handle);
    });
    present_contexts_drawing_video_sink(client, handle);
    update_unpainted_video_update_scheduling();
    publish_video_sink_freshness(client, handle, *sink_state);
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
        if (context->web_content_client.web_content_client_id() == client->web_content_client_id() && context->raster_targets.image_frames().contains(id) && m_core.context_is_effectively_visible(context_id))
            return true;
    }
    return false;
}

void CompositorState::show_animated_image_frame(CompositorStateWebContentClient* client, Compositing::ImageFrameResourceId id, Gfx::DecodedImageFrame const& frame)
{
    for (auto& [context_id, context] : m_presented_contexts) {
        auto& image_frames = context->raster_targets.image_frames();
        if (context->web_content_client.web_content_client_id() != client->web_content_client_id() || !image_frames.contains(id))
            continue;
        // NB: Contexts that draw the image frame hold the same one, unless it was registered anew since one took it.
        auto& image_frame = image_frames.image_frame(id);
        if (!image_frame.frame().has_same_pixels_as(frame))
            image_frame.replace_pixels(frame);
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
        auto& image_frames = context.raster_targets.image_frames();
        if (frame.has_value() && image_frames.contains(id) && !image_frames.image_frame(id).frame().has_same_pixels_as(*frame))
            image_frames.image_frame(id).replace_pixels(*frame);
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
            publish_video_sink_freshness(*client_entry.key, sink_entry.key, sink_state);
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
    for (auto handle : context->raster_targets.drawn_video_sinks()) {
        if (auto* sink_state = video_sink_state(context->web_content_client, handle); sink_state && sink_state->requires_updates)
            return true;
    }
    return false;
}

Optional<OverlayLabel> CompositorState::shape_overlay_label(Web::CompositorContextId context_id, Optional<StringView> font_family, float font_size_in_points, double device_pixel_ratio, StringView text)
{
    auto* context = presented_context_if_present(context_id);
    if (!context)
        return {};
    return context->raster_targets.shape_overlay_label(font_family, font_size_in_points, device_pixel_ratio, text);
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

// How long a screenshot waits for the video sinks it draws to catch up to their clocks, after which it shows the frames
// they have.
static constexpr auto SCREENSHOT_VIDEO_SINK_LIMIT = AK::Duration::from_seconds(2);

void CompositorState::request_screenshot(CompositorStateWebContentClient& requester, Web::CompositorContextId context_id, Compositing::ScreenshotRequestId request_id, Gfx::ShareableBitmap target_bitmap)
{
    VERIFY(presented_context_if_present(context_id));

    PendingScreenshot screenshot { requester.web_content_client_id(), context_id, request_id, move(target_bitmap), {}, {} };
    catch_up_newly_drawn_screenshot_sinks(screenshot);

    auto screenshot_id = m_next_pending_screenshot_id++;
    screenshot.deadline = Core::Timer::create_single_shot(SCREENSHOT_VIDEO_SINK_LIMIT, [this, screenshot_id] {
        paint_pending_screenshot(screenshot_id);
    });
    screenshot.deadline->start();
    m_pending_screenshots.set(screenshot_id, move(screenshot));
    paint_screenshots_whose_sinks_have_caught_up();
}

// The screenshot draws the video sinks of its context and of the contexts nested in it, which can change while it waits.
// Each sink is caught up once, as a sink that isn't ticking falls behind its clock again as soon as it plays on.
void CompositorState::catch_up_newly_drawn_screenshot_sinks(PendingScreenshot& screenshot)
{
    for (auto& sink : screenshot.sinks)
        sink.is_drawn = false;
    HashMap<Web::CompositorContextId, Vector<Web::CompositorContextId>> children;
    for (auto const& [child_id, _] : m_presented_contexts) {
        if (auto parent_context_id = m_core.parent_context(child_id); parent_context_id.has_value())
            children.ensure(*parent_context_id).append(child_id);
    }
    HashTable<Web::CompositorContextId> visited;
    Vector<Web::CompositorContextId> to_visit { screenshot.context_id };
    while (!to_visit.is_empty()) {
        auto id = to_visit.take_last();
        if (visited.set(id) != HashSetResult::InsertedNewEntry)
            continue;
        auto* context = presented_context_if_present(id);
        if (!context)
            continue;
        for (auto handle : context->raster_targets.drawn_video_sinks()) {
            auto client_id = context->web_content_client.web_content_client_id();
            auto* sink_state = video_sink_state(context->web_content_client, handle);
            if (!sink_state)
                continue;
            auto index = screenshot.sinks.find_first_index_if([&](auto const& sink) { return sink.client == client_id && sink.handle == handle; });
            if (!index.has_value()) {
                index = screenshot.sinks.size();
                screenshot.sinks.append({ client_id, handle });
            }
            screenshot.sinks[*index].is_drawn = true;
            auto& screenshot_sink = screenshot.sinks[*index];
            // A sink found before its edge attached or its ticking was decided is caught up once it can be.
            if (screenshot_sink.was_asked_to_catch_up || !sink_state->sink || !sink_state->sink->ticking_is_decided())
                continue;
            screenshot_sink.was_asked_to_catch_up = true;
            if (!sink_state->sink->frame_is_up_to_date())
                sink_state->sink->catch_up_to_clock();
        }
        if (auto child_ids = children.find(id); child_ids != children.end())
            to_visit.extend(child_ids->value);
    }
}

CompositorState::VideoSinkState* CompositorState::screenshot_sink_state(ScreenshotSink const& sink)
{
    auto* client = web_content_client_if_present(sink.client);
    if (!client)
        return nullptr;
    return video_sink_state(*client, sink.handle);
}

bool CompositorState::screenshot_sinks_have_caught_up(PendingScreenshot const& screenshot)
{
    return all_of(screenshot.sinks, [&](auto const& sink) {
        if (!sink.is_drawn)
            return true;
        // A sink without an edge, or one whose input has yet to start, has no frame to catch up.
        auto* sink_state = screenshot_sink_state(sink);
        return !sink_state || !sink_state->sink || !sink_state->sink->is_catching_up();
    });
}

// Painting may update the sinks, so it waits until whatever found them caught up is done with them.
void CompositorState::paint_screenshots_whose_sinks_have_caught_up()
{
    if (m_pending_screenshots.is_empty() || m_screenshot_check_is_scheduled)
        return;
    m_screenshot_check_is_scheduled = true;
    Core::deferred_invoke([self = NonnullRefPtr(*this)] {
        self->m_screenshot_check_is_scheduled = false;
        Vector<u64> screenshots_to_paint;
        for (auto& [screenshot_id, screenshot] : self->m_pending_screenshots) {
            self->catch_up_newly_drawn_screenshot_sinks(screenshot);
            if (self->screenshot_sinks_have_caught_up(screenshot))
                screenshots_to_paint.append(screenshot_id);
        }
        for (auto screenshot_id : screenshots_to_paint)
            self->paint_pending_screenshot(screenshot_id);
    });
}

void CompositorState::paint_pending_screenshot(u64 screenshot_id)
{
    auto screenshot = m_pending_screenshots.take(screenshot_id);
    if (!screenshot.has_value())
        return;
    screenshot->deadline->stop();

    auto painted = false;
    if (presented_context_if_present(screenshot->context_id) && screenshot->target_bitmap.is_valid() && screenshot->target_bitmap.bitmap()) {
        auto target_surface = Gfx::PaintingSurface::wrap_bitmap(*screenshot->target_bitmap.bitmap());
        painted = m_core.paint_screenshot(screenshot->context_id, *target_surface);
    }

    if (auto* requester = web_content_client_if_present(screenshot->requester))
        requester->tell_screenshot_result(screenshot->request_id, painted);
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

Optional<Gfx::ShareableBitmap> CompositorState::take_frame_snapshot(Web::CompositorContextId context_id)
{
    if (!presented_context_if_present(context_id))
        return {};
    auto presented_frame = m_core.presented_frame_for_snapshot(context_id);
    if (!presented_frame.has_value())
        return {};

    auto bitmap_or_error = Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, presented_frame->frame_size);
    if (bitmap_or_error.is_error())
        return {};
    auto bitmap = bitmap_or_error.release_value();
    presented_frame->surface->read_into_bitmap(*bitmap);
    return Gfx::ShareableBitmap { move(bitmap), Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
}

void CompositorState::presented_bitmap_ready_to_paint(Web::CompositorContextId context_id, i32 bitmap_id)
{
    if (!presented_context_if_present(context_id))
        return;
    m_core.presented_store_released(context_id, bitmap_id);
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

Compositing::RustFFI::FfiRasterPainter CompositorState::begin_painting(Web::CompositorContextId context_id, Gfx::PaintingSurface& surface, RustFFI::PaintingResources resources, bool allows_lcd_text)
{
    auto& context = presented_context(context_id);
    auto& raster_targets = context.raster_targets;
    if (m_painting_depth == m_display_list_players.size())
        m_display_list_players.append(make<DisplayListPlayerSkia>(m_skia_backend_context));
    auto& player = *m_display_list_players[m_painting_depth++];
    switch (resources) {
    case RustFFI::PaintingResources::Page:
        return player.begin_painting(surface, raster_targets.fonts(), &raster_targets.image_frames(), raster_targets.raster_cache(), &raster_targets.canvas_surfaces(), [this, &client = context.web_content_client](Media::VideoSinkHandle handle) { return current_video_frame(client, handle); }, allows_lcd_text);
    case RustFFI::PaintingResources::Overlays:
        return player.begin_painting(surface, raster_targets.overlay_fonts(), nullptr, raster_targets.overlay_raster_cache(), nullptr, nullptr);
    }
    VERIFY_NOT_REACHED();
}

void CompositorState::end_painting()
{
    VERIFY(m_painting_depth > 0);
    m_display_list_players[--m_painting_depth]->end_painting();
}

namespace {

// An image of a store that the Rust core holds until it publishes the store.
struct SharedImageToPublish {
    AK_ALLOC_WITH_KMALLOC;

    Gfx::SharedImage image;
};

}

bool CompositorState::create_store(Web::CompositorContextId context_id, RustFFI::StoreKind kind, Gfx::IntSize size, WindowServer* window_server_showing_stores, RustFFI::FfiNewStore& new_store)
{
    auto store_or_error = create_backing_store(kind, size, m_skia_backend_context, window_server_showing_stores);
    if (store_or_error.is_error()) {
        if (kind == RustFFI::StoreKind::ShownByWindowServer)
            dbgln("Compositor: Failed to allocate a store of context {} that a remote layer shows, which its presentation shows again: {}", context_id, store_or_error.error());
        return false;
    }
    auto store = store_or_error.release_value();
    new_store.surface = &store.surface.leak_ref();
    if (store.window_server_surface)
        new_store.window_server_surface = &store.window_server_surface.release_nonnull().leak_ref();
    if (store.shared_image_buffer) {
        new_store.shared_image_buffer = store.shared_image_buffer.leak_ptr();
        new_store.shared_image_buffer_is_in_use = [](void* buffer) { return shared_image_buffer_is_in_use(*static_cast<Gfx::SharedImageBuffer const*>(buffer)); };
        new_store.destroy_shared_image_buffer = [](void* buffer) { (void)adopt_own(*static_cast<Gfx::SharedImageBuffer*>(buffer)); };
    }
    if (store.shared_image.has_value()) {
        new_store.shared_image = make<SharedImageToPublish>(store.shared_image.release_value()).leak_ptr();
        new_store.destroy_shared_image = [](void* image) { (void)adopt_own(*static_cast<SharedImageToPublish*>(image)); };
    }
    return true;
}

void CompositorState::publish_stores(PresentationId presentation_id, Web::CompositorContextId context_id, bool added, ReadonlySpan<i32> bitmap_ids, ReadonlySpan<void*> images)
{
    Vector<Gfx::SharedImage> shared_images;
    shared_images.ensure_capacity(images.size());
    for (auto* image : images) {
        auto owned_image = adopt_own(*static_cast<SharedImageToPublish*>(image));
        shared_images.unchecked_append(move(owned_image->image));
    }
    auto* presentation = presentation_if_present(presentation_id);
    if (!presentation)
        return;
    Vector<i32> ids { bitmap_ids };
    if (added)
        presentation->did_add_backing_stores(context_id, move(ids), move(shared_images));
    else
        presentation->did_allocate_backing_stores(context_id, move(ids), move(shared_images));
}

void CompositorState::retire_published_stores(PresentationId presentation_id, Web::CompositorContextId context_id, ReadonlySpan<i32> bitmap_ids)
{
    if (auto* presentation = presentation_if_present(presentation_id))
        presentation->did_retire_backing_stores(context_id, Vector<i32> { bitmap_ids });
}

bool CompositorState::take_registered_image_frames(Web::CompositorContextId context_id, ReadonlySpan<u64> image_frame_ids)
{
    auto& context = presented_context(context_id);
    auto& image_frames = context.raster_targets.image_frames();
    auto image_frame_count_before = image_frames.size();
    bool holds_every_image_frame = image_frames.take_registered(image_frame_ids, context.web_content_client.registered_image_frames());
    if (image_frames.size() != image_frame_count_before)
        show_animated_image_frames_in(context);
    return holds_every_image_frame;
}

void CompositorState::set_drawn_image_frames(Web::CompositorContextId context_id, ReadonlySpan<u64> image_frame_ids)
{
    presented_context(context_id).raster_targets.retain_only_image_frames(image_frame_ids);
}

void CompositorState::set_drawn_video_sinks(Web::CompositorContextId context_id, ReadonlySpan<u64> handles)
{
    if (presented_context(context_id).raster_targets.set_drawn_video_sinks(handles))
        update_unpainted_video_update_scheduling();
}

void CompositorState::set_drawn_vector_images(Web::CompositorContextId context_id, ReadonlySpan<u64> vector_image_ids)
{
    presented_context(context_id).raster_targets.raster_cache().forget_vector_images_other_than(vector_image_ids);
}

void CompositorState::set_drawn_fonts(Web::CompositorContextId context_id, ReadonlySpan<u64> font_ids)
{
    presented_context(context_id).raster_targets.retain_only_fonts(font_ids);
}

bool CompositorState::context_holds_fonts(Web::CompositorContextId context_id, ReadonlySpan<u64> font_ids)
{
    auto const& fonts = presented_context(context_id).raster_targets.fonts();
    for (auto id : font_ids) {
        if (!fonts.contains(Compositing::FontResourceId { id }))
            return false;
    }
    return true;
}

bool CompositorState::stage_rasterization(CompositorStateWebContentClient& client, u64 token, ReadonlySpan<Compositing::DisplayListFontDescription> fonts, ReadonlySpan<Compositing::ImageFrameResourceId> image_frame_ids, Compositing::RegisteredTypefaces const& registered_typefaces, Gfx::ShareableBitmap target_bitmap)
{
    // WebContent waits for each rasterization it stages, so it has only the one in flight per thread that rasterizes.
    static constexpr size_t maximum_staged_rasterizations_per_client = 16;
    auto& staged = m_staged_rasterizations.ensure(client.web_content_client_id());
    if (staged.contains(token) || staged.size() >= maximum_staged_rasterizations_per_client)
        return false;
    auto rasterization = make<StagedRasterization>();
    if (!rasterization->fonts.add_described_fonts_made_of_registered_typefaces(fonts, registered_typefaces))
        return false;
    Vector<u64> ids_of_image_frames;
    ids_of_image_frames.ensure_capacity(image_frame_ids.size());
    for (auto image_frame_id : image_frame_ids)
        ids_of_image_frames.unchecked_append(image_frame_id.value());
    if (!rasterization->image_frames.take_registered(ids_of_image_frames, client.registered_image_frames()))
        return false;
    rasterization->target_bitmap = move(target_bitmap);
    staged.set(token, move(rasterization));
    return true;
}

CompositorState::StagedRasterization* CompositorState::staged_rasterization(WebContentClientId client, u64 token)
{
    auto staged = m_staged_rasterizations.find(client);
    if (staged == m_staged_rasterizations.end())
        return nullptr;
    auto rasterization = staged->value.find(token);
    if (rasterization == staged->value.end())
        return nullptr;
    return rasterization->value.ptr();
}

bool CompositorState::staged_rasterization_has_resources(WebContentClientId client, u64 token, ReadonlySpan<u64> font_ids, ReadonlySpan<u64> image_frame_ids)
{
    auto* rasterization = staged_rasterization(client, token);
    if (!rasterization)
        return false;
    for (auto id : font_ids) {
        if (!rasterization->fonts.contains(Compositing::FontResourceId { id }))
            return false;
    }
    for (auto id : image_frame_ids) {
        if (!rasterization->image_frames.contains(Compositing::ImageFrameResourceId { id }))
            return false;
    }
    return true;
}

// The largest frame WebContent rasterizes for an SVG image.
static constexpr int max_rasterization_target_dimension = 16384;

CanvasHost& CompositorState::canvas_host_of(WebContentClientId client_id)
{
    auto* client = web_content_client_if_present(client_id);
    VERIFY(client);
    return client->canvas_host();
}

void CompositorState::begin_canvas_2d_painting(WebContentClientId client_id, Compositing::CanvasId canvas_id, RustFFI::FfiCanvas2DPainter& painter)
{
    auto* client = web_content_client_if_present(client_id);
    VERIFY(client);
    VERIFY(!m_canvas_host_painting_2d_context);
    m_canvas_host_painting_2d_context = &client->canvas_host();
    auto& player = m_canvas_host_painting_2d_context->begin_2d_painting(canvas_id, client->registered_image_frames());
    painter = {
        .player = &player,
        .play_command = [](void* player, Compositing::DisplayListCommandType command_type, ReadonlyBytes payload) { static_cast<Canvas2DCommandPlayer*>(player)->play_command(command_type, payload); },
    };
}

bool CompositorState::end_canvas_2d_painting_reporting_whether_video_frames_were_sampled_on_gpu()
{
    return exchange(m_canvas_host_painting_2d_context, nullptr)->end_2d_painting_reporting_whether_video_frames_were_sampled_on_gpu();
}

bool CompositorState::begin_staged_rasterization(WebContentClientId client, u64 token, Compositing::RustFFI::FfiRasterPainter& painter)
{
    auto* rasterization = staged_rasterization(client, token);
    if (!rasterization || !rasterization->target_bitmap.is_valid() || !rasterization->target_bitmap.bitmap())
        return false;
    auto& target = *rasterization->target_bitmap.bitmap();
    if (target.format() != Gfx::BitmapFormat::BGRA8888 || target.alpha_type() != Gfx::AlphaType::Premultiplied) {
        dbgln("Compositor: Not rasterizing display list from WebContent: Target bitmap is not BGRA8888 with premultiplied alpha");
        return false;
    }
    if (target.width() > max_rasterization_target_dimension || target.height() > max_rasterization_target_dimension) {
        dbgln("Compositor: Not rasterizing display list from WebContent: Target bitmap is too large");
        return false;
    }
    __builtin_memset(target.scanline_u8(0), 0, target.size_in_bytes());
    rasterization->target_surface = Gfx::PaintingSurface::wrap_bitmap(target);
    rasterization->player = make<DisplayListPlayerSkia>(RefPtr<Gfx::SkiaBackendContext> {});
    painter = rasterization->player->begin_painting(*rasterization->target_surface, rasterization->fonts, &rasterization->image_frames, rasterization->raster_cache, nullptr, nullptr);
    return true;
}

void CompositorState::finish_staged_rasterization(WebContentClientId client, u64 token)
{
    auto staged = m_staged_rasterizations.find(client);
    if (staged == m_staged_rasterizations.end())
        return;
    auto rasterization = staged->value.take(token);
    if (!rasterization.has_value())
        return;
    if (auto& player = (*rasterization)->player) {
        player->end_painting();
        DisplayListPlayerSkia::flush(*(*rasterization)->target_surface);
    }
}

static bool video_frame_holds_high_dynamic_range(Media::VideoFrame const& frame)
{
    auto transfer_characteristics = frame.cicp().transfer_characteristics();
    return transfer_characteristics == Media::TransferCharacteristics::SMPTE2084 || transfer_characteristics == Media::TransferCharacteristics::HLG;
}

void CompositorState::report_live_content(Web::CompositorContextId context_id, ReadonlySpan<u64> canvas_ids, void* receiver, void (*receive)(void*, RustFFI::FfiLiveContent const*))
{
    auto& context = presented_context(context_id);
    auto& raster_targets = context.raster_targets;

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
    video_sinks.ensure_capacity(raster_targets.drawn_video_sinks().size());
    for (auto handle : raster_targets.drawn_video_sinks()) {
        auto frame = current_video_frame(context.web_content_client, handle);
        video_sinks.unchecked_append({
            .handle = handle.value(),
            .has_frame = frame != nullptr,
            .frame_id = frame ? frame->id() : 0,
            .frame_holds_high_dynamic_range = frame && video_frame_holds_high_dynamic_range(*frame),
            .frame_has_surface = frame && frame->surface(),
        });
    }
    Vector<RustFFI::FfiImageFrameContent> image_frames;
    raster_targets.image_frames().for_each([&](auto id, auto const& image_frame) {
        if (auto pixel_generation = image_frame.generation_of_replaced_pixels(); pixel_generation.has_value())
            image_frames.append({ .id = id.value(), .pixel_generation = *pixel_generation });
    });

    RustFFI::FfiLiveContent content {
        .canvases = canvases.span(),
        .video_sinks = video_sinks.span(),
        .image_frames = image_frames.span(),
    };
    receive(receiver, &content);
}

Optional<RemoteLayer::Content> CompositorState::remote_layer_content(PresentedContext& context, RustFFI::MediaKind kind, u64 media_id)
{
    switch (kind) {
    case RustFFI::MediaKind::Canvas:
        if (auto surface = context.raster_targets.canvas_surfaces().presentable_canvas_surface(Compositing::CanvasId { media_id }))
            return RemoteLayer::Content { surface.release_nonnull() };
        return {};
    case RustFFI::MediaKind::Video:
        if (auto frame = current_video_frame(context.web_content_client, Media::VideoSinkHandle { media_id }))
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

static Optional<RemoteSublayer::PathClip> path_clip_of(RustFFI::FfiSublayerPlacement const& placement)
{
    if (!placement.is_clipped_by_path)
        return {};
    auto contours = Gfx::PathData::from_ffi(placement.clip_path_contours);
    auto const* path_to_frame = placement.clip_path_to_frame;
    RemoteSublayer::PathClip path_clip;
    path_clip.path_identity = placement.clip_path_identity;
    path_clip.verbs.append(contours.verbs.data(), contours.verbs.size());
    path_clip.points.append(contours.points.data(), contours.points.size());
    path_clip.conic_weights.append(contours.conic_weights.data(), contours.conic_weights.size());
    path_clip.winding_rule = placement.clip_path_fill_rule;
    path_clip.path_to_frame = { path_to_frame[0], path_to_frame[1], path_to_frame[2], path_to_frame[3], path_to_frame[4], path_to_frame[5] };
    return path_clip;
}

bool CompositorState::remote_layer_can_show(RemoteLayer& remote_layer, Web::CompositorContextId context_id, RustFFI::MediaKind kind, u64 media_id, RustFFI::FfiSublayerPlacement const& placement, double device_pixel_ratio)
{
    auto content = remote_layer_content(presented_context(context_id), kind, media_id);
    if (!content.has_value())
        return false;
    WindowServerPlacement window_server_placement {
        .content_rect = placement.content_rect,
        .content_to_viewport = content_to_frame_of(placement),
        .clip_rect = placement.clip_rect,
        .rounded_clip = rounded_clip_of(placement),
        .is_clipped_by_path = placement.is_clipped_by_path,
        .scaling_mode = placement.scaling_mode,
    };
    return remote_layer.can_show(window_server_placement, *content, device_pixel_ratio);
}

Vector<RemoteSublayer>* CompositorState::build_sublayers(Web::CompositorContextId context_id, ReadonlySpan<RustFFI::FfiSublayer> sublayers_to_build)
{
    auto sublayers = make<Vector<RemoteSublayer>>();
    auto& context = presented_context(context_id);
    auto& raster_targets = context.raster_targets;
    // A sublayer without content is left out, and so are the sublayers of a group that is.
    Vector<Optional<size_t>> index_of_built_sublayer;
    index_of_built_sublayer.ensure_capacity(sublayers_to_build.size());
    for (auto const& sublayer : sublayers_to_build) {
        Optional<size_t> group_index;
        bool is_in_group = sublayer.group_index >= 0;
        if (is_in_group && static_cast<size_t>(sublayer.group_index) < index_of_built_sublayer.size())
            group_index = index_of_built_sublayer[sublayer.group_index];
        Optional<decltype(RemoteSublayer::content)> content;
        if (!is_in_group || group_index.has_value()) {
            switch (sublayer.content_kind) {
            case RustFFI::SublayerContentKind::Canvas:
                if (auto surface = raster_targets.canvas_surfaces().presentable_canvas_surface(Compositing::CanvasId { sublayer.media_id }))
                    content = surface->lease();
                break;
            case RustFFI::SublayerContentKind::Video:
                if (auto frame = current_video_frame(context.web_content_client, Media::VideoSinkHandle { sublayer.media_id }); frame && frame->surface())
                    content = frame.release_nonnull();
                break;
            case RustFFI::SublayerContentKind::PresentableSurface:
                content = const_cast<PresentableSurface*>(sublayer.presentable_surface)->lease();
                break;
            case RustFFI::SublayerContentKind::Group: {
                auto const* to_parent = sublayer.group.to_parent;
                content = RemoteSublayer::Group {
                    .to_parent = { to_parent[0], to_parent[1], to_parent[2], to_parent[3], to_parent[4], to_parent[5] },
                    .clip_rect = sublayer.group.clips ? Optional<Gfx::FloatRect> { sublayer.group.clip_rect } : OptionalNone {},
                    .corner_radii = sublayer.group.corner_radii,
                    .opacity = sublayer.group.opacity,
                };
                break;
            }
            case RustFFI::SublayerContentKind::Tile:
                content = RemoteSublayer::Tile {
                    .surface = const_cast<PresentableSurface*>(sublayer.presentable_surface)->lease(),
                    .rect_in_surface = sublayer.rect_in_surface,
                    .every_pixel_is_opaque = sublayer.every_pixel_is_opaque,
                };
                break;
            case RustFFI::SublayerContentKind::Color:
                content = sublayer.color;
                break;
            case RustFFI::SublayerContentKind::Backdrop: {
                auto const& backdrop = sublayer.backdrop;
                RemoteSublayer::Backdrop built_backdrop;
                built_backdrop.region = backdrop.region;
                auto append_color_matrix = [&](float const* color_matrix) {
                    RemoteSublayer::Backdrop::Filter filter;
                    filter.kind = RemoteSublayer::Backdrop::Filter::Kind::ColorMatrix;
                    for (size_t index = 0; index < filter.color_matrix.size(); ++index)
                        filter.color_matrix[index] = color_matrix[index];
                    built_backdrop.filters.append(filter);
                };
                if (backdrop.has_color_matrix_before_blur)
                    append_color_matrix(backdrop.color_matrix_before_blur);
                if (backdrop.has_blur) {
                    RemoteSublayer::Backdrop::Filter blur;
                    blur.kind = RemoteSublayer::Backdrop::Filter::Kind::Blur;
                    blur.blur_standard_deviation = backdrop.blur_standard_deviation;
                    built_backdrop.filters.append(blur);
                }
                if (backdrop.has_color_matrix_after_blur)
                    append_color_matrix(backdrop.color_matrix_after_blur);
                content = move(built_backdrop);
                break;
            }
            }
        }
        if (!content.has_value()) {
            index_of_built_sublayer.unchecked_append({});
            continue;
        }
        index_of_built_sublayer.unchecked_append(sublayers->size());
        auto const& placement = sublayer.placement;
        bool is_placed_by_group = content->has<RemoteSublayer::Tile>() || content->has<Gfx::Color>();
        sublayers->append({
            .identity = sublayer.identity,
            .group_index = group_index,
            .content = content.release_value(),
            .content_rect = is_placed_by_group ? sublayer.rect : placement.content_rect,
            .content_to_frame = content_to_frame_of(placement),
            .clip_rect = placement.clip_rect,
            .rounded_clip = rounded_clip_of(placement),
            .path_clip = path_clip_of(placement),
            .scaling_mode = placement.scaling_mode,
        });
    }
    return sublayers.leak_ptr();
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

namespace {

struct VideoFrameForRust {
    AK_ALLOC_WITH_KMALLOC;

    NonnullRefPtr<Media::VideoFrame> frame;
};

struct VideoFrameImageForRust {
    AK_ALLOC_WITH_KMALLOC;

    sk_sp<SkImage> image;
};

struct ConvertedPixelsForRust {
    AK_ALLOC_WITH_KMALLOC;

    ByteBuffer buffer;
};

// NB: `ExportFormat` of libcompositing_rust lists the formats in this order.
static_assert(to_underlying(Gfx::ExportFormat::Luminance8) == 0);
static_assert(to_underlying(Gfx::ExportFormat::RGBA8888) == 7);

struct SharedMemoryMapping {
    AK_ALLOC_WITH_KMALLOC;

    explicit SharedMemoryMapping(Core::AnonymousBuffer buffer)
        : buffer(move(buffer))
    {
    }

    Core::AnonymousBuffer buffer;
};

}

static CompositorState& state_of(void* host)
{
    return *static_cast<CompositorState*>(host);
}

static SkCanvas& canvas_of(Gfx::PaintingSurface const* surface)
{
    return surface->canvas();
}

static Gfx::PaintingSurface& mutable_surface(Gfx::PaintingSurface const* surface)
{
    return const_cast<Gfx::PaintingSurface&>(*surface);
}

static Optional<u64> display_of(bool has_display_id, u64 display_id)
{
    if (!has_display_id)
        return {};
    return display_id;
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
        .create_surface = [](void* host, Gfx::IntSize size, Gfx::PaintingSurface const* like, bool inherits_lcd_text) -> Gfx::PaintingSurface const* {
            auto skia_backend_context = like ? like->skia_backend_context() : state_of(host).m_skia_backend_context;
            auto text_antialiasing = Gfx::PaintingSurface::TextAntialiasing::Grayscale;
            if (inherits_lcd_text && like && canvas_of(like).getBaseProps().pixelGeometry() != kUnknown_SkPixelGeometry)
                text_antialiasing = Gfx::PaintingSurface::TextAntialiasing::LCD;
            auto surface = Gfx::PaintingSurface::try_create_with_size(size, Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, skia_backend_context, {}, text_antialiasing);
            if (surface.is_error())
                return nullptr;
            return &surface.release_value().leak_ref();
        },
        .surface_save = [](Gfx::PaintingSurface const* surface) { return canvas_of(surface).save(); },
        .surface_restore_to_count = [](Gfx::PaintingSurface const* surface, i32 count) { canvas_of(surface).restoreToCount(count); },
        .surface_translate = [](Gfx::PaintingSurface const* surface, float x, float y) { canvas_of(surface).translate(x, y); },
        .surface_scale = [](Gfx::PaintingSurface const* surface, float x, float y) { canvas_of(surface).scale(x, y); },
        .surface_clip_rect = [](Gfx::PaintingSurface const* surface, Gfx::IntRect rect) { canvas_of(surface).clipIRect(SkIRect::MakeXYWH(rect.x(), rect.y(), rect.width(), rect.height())); },
        .surface_clear = [](Gfx::PaintingSurface const* surface, Gfx::Color color) { canvas_of(surface).clear(Gfx::to_skia_color(color)); },
        .surface_draw_surface = [](Gfx::PaintingSurface const* target, Gfx::PaintingSurface const* source, Gfx::IntRect source_rect, Gfx::IntRect target_rect, RustFFI::SurfaceBlend blend, RustFFI::SurfaceSampling sampling) {
            auto image = source->sk_image_snapshot<sk_sp<SkImage>>();
            SkPaint paint;
            paint.setBlendMode(blend == RustFFI::SurfaceBlend::Source ? SkBlendMode::kSrc : SkBlendMode::kSrcOver);
            // Only tiles are drawn filtered.
            Gfx::draw_tile_image_rect(canvas_of(target), *image, Gfx::to_skia_rect(source_rect), Gfx::to_skia_rect(target_rect), sampling == RustFFI::SurfaceSampling::Linear, paint); },
        .flush_surface = [](Gfx::PaintingSurface const* surface) { DisplayListPlayerSkia::flush(mutable_surface(surface)); },
        .begin_painting = [](void* host, u64 context_id, Gfx::PaintingSurface const* surface, RustFFI::PaintingResources resources, bool allows_lcd_text, Compositing::RustFFI::FfiRasterPainter* painter) { *painter = state_of(host).begin_painting(Context { context_id }, mutable_surface(surface), resources, allows_lcd_text); },
        .end_painting = [](void* host) { state_of(host).end_painting(); },
        .start_raster_cache_frame = [](void* host, u64 context_id) { state_of(host).presented_context(Context { context_id }).raster_targets.raster_cache().drop_what_was_not_used_since_last_frame(); },
        .create_store = [](void* host, u64 context_id, RustFFI::StoreKind kind, Gfx::IntSize size, void* window_server, RustFFI::FfiNewStore* new_store) { return state_of(host).create_store(Context { context_id }, kind, size, static_cast<WindowServer*>(window_server), *new_store); },
        .publish_stores = [](void* host, u64 presentation_id, u64 context_id, bool added, ReadonlySpan<i32> bitmap_ids, ReadonlySpan<void*> images) { state_of(host).publish_stores(PresentationId { presentation_id }, Context { context_id }, added, bitmap_ids, images); },
        .retire_published_stores = [](void* host, u64 presentation_id, u64 context_id, ReadonlySpan<i32> bitmap_ids) { state_of(host).retire_published_stores(PresentationId { presentation_id }, Context { context_id }, bitmap_ids); },
        .create_presentable_surface = [](void*, Gfx::IntSize size, Gfx::PaintingSurface const* like) -> PresentableSurface const* {
            auto surface = PresentableSurfacePool::create_surface(size, like->skia_backend_context());
            if (surface.is_error())
                return nullptr;
            return &surface.release_value().leak_ref();
        },
        .window_server_compositing = [](void* window_server) -> RustFFI::FfiWindowServerCompositing {
            auto const& compositing_window_server = *static_cast<WindowServer*>(window_server);
            return {
                .shows_tiles_of_painted_layers = compositing_window_server.shows_tiles_of_painted_layers(),
                .filters_what_is_below_layers = compositing_window_server.filters_what_is_below_layers(),
            };
        },
        .live_content = [](void* host, u64 context_id, ReadonlySpan<u64> canvas_ids, void* receiver, void (*receive)(void*, RustFFI::FfiLiveContent const*)) { state_of(host).report_live_content(Context { context_id }, canvas_ids, receiver, receive); },
        .remote_layer_can_show = [](void* host, void* remote_layer, u64 context_id, RustFFI::MediaKind kind, u64 media_id, RustFFI::FfiSublayerPlacement const* placement, double device_pixel_ratio) { return state_of(host).remote_layer_can_show(*static_cast<RemoteLayer*>(remote_layer), Context { context_id }, kind, media_id, *placement, device_pixel_ratio); },
        .build_sublayers = [](void* host, u64 context_id, ReadonlySpan<RustFFI::FfiSublayer> sublayers) -> void* {
            return state_of(host).build_sublayers(Context { context_id }, sublayers);
        },
        .destroy_sublayers = [](void* sublayers) { delete static_cast<Vector<RemoteSublayer>*>(sublayers); },
        .submit_frame = [](void* host, u64, Gfx::PaintingSurface const* surface) { return state_of(host).submit_frame(const_cast<Gfx::PaintingSurface*>(surface)); },
        .hand_off_once_gpu_work_is_scheduled = [](void* host, u64 token) { state_of(host).hand_off_once_gpu_work_is_scheduled(token); },
        .create_window_server = [](void*, RustFFI::FfiCppObject* window_server) {
            auto created = WindowServer::try_create_for_presentation();
            if (!created)
                return false;
            window_server->object = &created.release_nonnull().leak_ref();
            window_server->destroy = [](void* object) { static_cast<WindowServer*>(object)->unref(); };
            return true; },
        .window_server_frame_hand_off = [](void* window_server) { return static_cast<WindowServer*>(window_server)->frame_hand_off() == WindowServer::FrameHandOff::OnceGpuWorkIsScheduled
                                                                      ? RustFFI::FrameHandOff::OnceGpuWorkIsScheduled
                                                                      : RustFFI::FrameHandOff::OnceGpuWorkIsFinished; },
        .window_server_hold_changes_on_screen = [](void* window_server) { static_cast<WindowServer*>(window_server)->hold_changes_on_screen(); },
        .window_server_release_changes_held_off_screen = [](void* window_server) { static_cast<WindowServer*>(window_server)->release_changes_held_off_screen(); },
        .create_remote_layer = [](void* window_server, u64 remote_layer_id, RustFFI::FfiCppObject* remote_layer, RustFFI::FfiRemoteLayerHostHandle* host_handle) {
            auto layer = static_cast<WindowServer*>(window_server)->try_create_remote_layer(remote_layer_id);
            if (!layer)
                return false;
            auto handle = layer->host_handle();
            host_handle->kind = to_underlying(handle.kind);
            host_handle->id = handle.id;
            remote_layer->object = layer.leak_ptr();
            remote_layer->destroy = [](void* object) { (void)adopt_own(*static_cast<RemoteLayer*>(object)); };
            return true; },
        .remote_layer_place_frame = [](void* remote_layer, bool has_fit_within, Gfx::IntSize fit_within) { static_cast<RemoteLayer*>(remote_layer)->place_frame(has_fit_within ? Optional<Gfx::IntSize> { fit_within } : OptionalNone {}); },
        .remote_layer_show_frame = [](void* remote_layer, RustFFI::FfiRemoteLayerFrame const* frame, void* sublayers) {
            auto owned_sublayers = adopt_own(*static_cast<Vector<RemoteSublayer>*>(sublayers));
            auto* store = const_cast<PresentableSurface*>(frame->store);
            auto shown = static_cast<RemoteLayer*>(remote_layer)->show_frame({
                .store = store ? Optional<PresentableSurface::Lease> { store->lease() } : OptionalNone {},
                .shows_no_store = frame->shows_no_store,
                .painted_size = frame->painted_size,
                .device_pixel_ratio = frame->device_pixel_ratio,
                .fit_within = frame->has_fit_within ? Optional<Gfx::IntSize> { frame->fit_within } : OptionalNone {},
                .sublayers = move(*owned_sublayers),
            });
            if (!shown)
                dbgln("Compositor: A remote layer failed to show a frame, which its presentation shows again");
            return shown; },
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
        .map_shared_memory = [](void*, i32 descriptor, u64 size, u8** data, size_t* data_size) -> void* {
            auto buffer = Core::AnonymousBuffer::create_from_anon_fd(descriptor, size);
            if (buffer.is_error()) {
                dbgln("Compositor: Could not map shared memory from WebContent: {}", buffer.error());
                return nullptr;
            }
            if (auto validation = buffer.value().validate_backing_size(); validation.is_error()) {
                dbgln("Compositor: Could not map shared memory from WebContent: {}", validation.error());
                return nullptr;
            }
            auto mapping = make<SharedMemoryMapping>(buffer.release_value());
            *data = mapping->buffer.data<u8>();
            *data_size = mapping->buffer.bytes().size();
            return mapping.leak_ptr();
        },
        .unmap_shared_memory = [](void* mapping) { delete static_cast<SharedMemoryMapping*>(mapping); },
        .take_registered_image_frames = [](void* host, u64 context_id, ReadonlySpan<u64> image_frame_ids) { return state_of(host).take_registered_image_frames(Context { context_id }, image_frame_ids); },
        .set_drawn_image_frames = [](void* host, u64 context_id, ReadonlySpan<u64> image_frame_ids) { state_of(host).set_drawn_image_frames(Context { context_id }, image_frame_ids); },
        .set_drawn_video_sinks = [](void* host, u64 context_id, ReadonlySpan<u64> handles) { state_of(host).set_drawn_video_sinks(Context { context_id }, handles); },
        .set_drawn_vector_images = [](void* host, u64 context_id, ReadonlySpan<u64> vector_image_ids) { state_of(host).set_drawn_vector_images(Context { context_id }, vector_image_ids); },
        .set_drawn_fonts = [](void* host, u64 context_id, ReadonlySpan<u64> font_ids) { state_of(host).set_drawn_fonts(Context { context_id }, font_ids); },
        .context_holds_fonts = [](void* host, u64 context_id, ReadonlySpan<u64> font_ids) { return state_of(host).context_holds_fonts(Context { context_id }, font_ids); },
        .staged_rasterization_has_resources = [](void* host, u64 client, u64 token, ReadonlySpan<u64> font_ids, ReadonlySpan<u64> image_frame_ids) { return state_of(host).staged_rasterization_has_resources(WebContentClientId { client }, token, font_ids, image_frame_ids); },
        .begin_staged_rasterization = [](void* host, u64 client, u64 token, Compositing::RustFFI::FfiRasterPainter* painter) { return state_of(host).begin_staged_rasterization(WebContentClientId { client }, token, *painter); },
        .finish_staged_rasterization = [](void* host, u64 client, u64 token) { state_of(host).finish_staged_rasterization(WebContentClientId { client }, token); },
        .reserve_canvas_ids = [](void* host, u64 count) { return state_of(host).m_canvas_surface_registry.reserve_canvas_ids(count).value(); },
        .create_canvas_2d_context = [](void* host, u64 client, Compositing::CanvasId canvas_id, Gfx::IntSize size, bool alpha, bool will_read_frequently, Web::Bindings::PredefinedColorSpace color_space) { return state_of(host).canvas_host_of(WebContentClientId { client }).create_2d_context(canvas_id, size, alpha, will_read_frequently, Compositing::gfx_color_space_for(color_space)); },
        .destroy_canvas_2d_context = [](void* host, u64 client, Compositing::CanvasId canvas_id) { state_of(host).canvas_host_of(WebContentClientId { client }).destroy_2d_context(canvas_id); },
        .read_back_canvas_2d_pixels = [](void* host, u64 client, Compositing::CanvasId canvas_id, Gfx::IntRect rect, u8 alpha_type, Web::Bindings::PredefinedColorSpace color_space, u8* pixels, size_t pixels_size, Gfx::IntSize* size) {
            VERIFY(Gfx::is_valid_alpha_type(alpha_type));
            auto read_size = state_of(host).canvas_host_of(WebContentClientId { client }).read_back_canvas_2d_pixels(canvas_id, rect, static_cast<Gfx::AlphaType>(alpha_type), Compositing::gfx_color_space_for(color_space), { pixels, pixels_size });
            if (!read_size.has_value())
                return false;
            *size = *read_size;
            return true; },
        .begin_canvas_2d_painting = [](void* host, u64 client, Compositing::CanvasId canvas_id, RustFFI::FfiCanvas2DPainter* painter) { state_of(host).begin_canvas_2d_painting(WebContentClientId { client }, canvas_id, *painter); },
        .end_canvas_2d_painting = [](void* host) { return state_of(host).end_canvas_2d_painting_reporting_whether_video_frames_were_sampled_on_gpu(); },
        .present_canvas_2d_context = [](void* host, u64 client, Compositing::CanvasId canvas_id) { return state_of(host).canvas_host_of(WebContentClientId { client }).try_present_2d_context(canvas_id); },
        .flush_canvas_2d_contexts = [](void* host, u64 client, ReadonlySpan<Compositing::CanvasId> canvas_ids) { state_of(host).canvas_host_of(WebContentClientId { client }).flush_2d_contexts(canvas_ids); },
        .gl_proc_address = [](void*, char const* name) { return OpenGLContext::address_of_function(name); },
        .create_webgl_context = [](void* host, u64 client, Compositing::CanvasId canvas_id, RustFFI::ContextOptions options) {
            auto version = options.webgl2 ? Compositing::WebGL::WebGLVersion::WebGL2 : Compositing::WebGL::WebGLVersion::WebGL1;
            return state_of(host).canvas_host_of(WebContentClientId { client }).create_webgl_context(canvas_id, version, { .depth = options.depth, .stencil = options.stencil, .antialias = options.antialias }, options.size); },
        .destroy_webgl_context = [](void* host, u64 client, Compositing::CanvasId canvas_id) { state_of(host).canvas_host_of(WebContentClientId { client }).destroy_webgl_context(canvas_id); },
        .make_webgl_context_current = [](void* host, u64 client, Compositing::CanvasId canvas_id, RustFFI::DefaultBuffers* defaults) {
            auto& gl_context = state_of(host).canvas_host_of(WebContentClientId { client }).webgl_context(canvas_id);
            gl_context.make_current();
            *defaults = { .framebuffer = gl_context.default_framebuffer(), .renderbuffer = gl_context.default_renderbuffer() }; },
        .set_webgl_drawing_buffer_size = [](void* host, u64 client, Compositing::CanvasId canvas_id, Gfx::IntSize size) { state_of(host).canvas_host_of(WebContentClientId { client }).webgl_context(canvas_id).set_size(size); },
        .present_webgl_context = [](void* host, u64 client, Compositing::CanvasId canvas_id, bool preserve_drawing_buffer, bool show_presented_frame) {
            switch (state_of(host).canvas_host_of(WebContentClientId { client }).present_webgl_context(canvas_id, preserve_drawing_buffer, show_presented_frame)) {
            case CanvasHost::WebGLPresentOutcome::Presented:
                return RustFFI::PresentOutcome::Presented;
            case CanvasHost::WebGLPresentOutcome::ShowedPresentedFrame:
                return RustFFI::PresentOutcome::ShowedPresentedFrame;
            case CanvasHost::WebGLPresentOutcome::NoDrawingBuffer:
                return RustFFI::PresentOutcome::NoDrawingBuffer;
            }
            VERIFY_NOT_REACHED(); },
        .read_back_webgl_drawing_buffer = [](void* host, u64 client, Compositing::CanvasId canvas_id, bool from_presented_frame, Gfx::IntRect rect, u8* pixels, size_t pixels_size, Gfx::IntSize* size) {
            auto read_size = state_of(host).canvas_host_of(WebContentClientId { client }).read_back_webgl_drawing_buffer(canvas_id, from_presented_frame, rect, { pixels, pixels_size });
            if (!read_size.has_value())
                return false;
            *size = *read_size;
            return true; },
        .webgl_can_copy_images_into_textures = [](void* host, u64 client, Compositing::CanvasId canvas_id) { return state_of(host).canvas_host_of(WebContentClientId { client }).webgl_context(canvas_id).can_copy_images_into_textures(); },
        .held_video_frame = [](void* host, u64 client, u64 sink_handle, u64 generation, RustFFI::FfiCppObject* frame, Gfx::IntSize* size) {
            auto held_frame = state_of(host).canvas_host_of(WebContentClientId { client }).held_video_frame(Media::VideoSinkHandle { sink_handle }, generation);
            if (!held_frame)
                return false;
            *size = held_frame->size().to_type<int>();
            frame->object = make<VideoFrameForRust>(held_frame.release_nonnull()).leak_ptr();
            frame->destroy = [](void* object) { delete static_cast<VideoFrameForRust*>(object); };
            return true; },
        .video_frame_image = [](void* host, void* frame, RustFFI::FfiCppObject* image) {
            auto sk_image = sk_image_from_video_frame(*static_cast<VideoFrameForRust*>(frame)->frame, state_of(host).m_skia_backend_context);
            if (!sk_image)
                return;
            image->object = make<VideoFrameImageForRust>(move(sk_image)).leak_ptr();
            image->destroy = [](void* object) { delete static_cast<VideoFrameImageForRust*>(object); }; },
        .copy_video_frame_image_into_bound_texture = [](void* host, u64 client, Compositing::CanvasId canvas_id, void* image, RustFFI::VideoFrameCopy copy) {
            auto& gl_context = state_of(host).canvas_host_of(WebContentClientId { client }).webgl_context(canvas_id);
            return gl_context.copy_image_into_bound_texture(*static_cast<VideoFrameImageForRust*>(image)->image, copy.source_size, copy.size, copy.target, copy.level, copy.xoffset, copy.yoffset, copy.flip_y, copy.premultiply_alpha); },
        .convert_image_source_for_webgl = [](void* host, u64 client_id, RustFFI::FfiImageSource source, RustFFI::FfiImageConversion conversion, RustFFI::FfiConvertedPixels* converted) {
            auto* client = state_of(host).web_content_client_if_present(WebContentClientId { client_id });
            VERIFY(client);
            VERIFY(conversion.export_format <= to_underlying(Gfx::ExportFormat::RGBA8888));
            auto image_source = source.is_held_video_frame
                ? CanvasHost::WebGLImageSource { CanvasHost::HeldVideoFrame { Media::VideoSinkHandle { source.id }, source.generation } }
                : CanvasHost::WebGLImageSource { Compositing::ImageFrameResourceId { source.id } };
            int export_flags = 0;
            if (conversion.flip_y)
                export_flags |= Gfx::ExportFlags::FlipY;
            if (conversion.premultiply_alpha)
                export_flags |= Gfx::ExportFlags::PremultiplyAlpha;
            Optional<Gfx::IntSize> target_size;
            if (conversion.has_target_size)
                target_size = conversion.target_size;
            Gfx::BitmapExportResult pixels;
            switch (client->canvas_host().convert_image_for_webgl_upload(image_source, client->registered_image_frames(), static_cast<Gfx::ExportFormat>(conversion.export_format), export_flags, target_size, pixels)) {
            case CanvasHost::WebGLImageConversion::UnknownImageFrame:
                return RustFFI::FfiImageConversionOutcome::UnknownImageFrame;
            case CanvasHost::WebGLImageConversion::Failed:
                return RustFFI::FfiImageConversionOutcome::Failed;
            case CanvasHost::WebGLImageConversion::Converted:
                break;
            }
            auto owned_pixels = make<ConvertedPixelsForRust>(move(pixels.buffer));
            *converted = {
                .pixels = { .object = owned_pixels.ptr(), .destroy = [](void* object) { delete static_cast<ConvertedPixelsForRust*>(object); } },
                .data = owned_pixels->buffer.data(),
                .size = owned_pixels->buffer.size(),
                .width = pixels.width,
                .height = pixels.height,
            };
            (void)owned_pixels.leak_ptr();
            return RustFFI::FfiImageConversionOutcome::Converted; },
    };
}

}
