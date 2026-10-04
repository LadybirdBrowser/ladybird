/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/Compositor/CompositorConnection.h>

#include <AK/Debug.h>
#include <AK/Mutex.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibIPC/Limits.h>
#include <LibIPC/Transport.h>
#include <LibMediaClient/Client.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Page/Page.h>

namespace Web::Compositor {

static bool display_list_timing_enabled()
{
    static bool enabled = [] {
        auto value = Core::Environment::get("LADYBIRD_DISPLAY_LIST_TIMING"sv);
        return value.has_value() && !value->is_empty() && *value != "0"sv;
    }();
    return enabled;
}

// Posts frames straight to the connection's transport, which queues messages from any thread for its IO thread to send.
// The rest of the connection stays with the thread that made it.
class CompositorConnectionFrameSink final : public CompositorFrameSink {
public:
    AK_ALLOC_WITH_KMALLOC;

    explicit CompositorConnectionFrameSink(IPC::Transport& transport)
        : m_transport(&transport)
    {
    }

    // Called by the connection once the compositor is lost or the connection goes away.
    void detach()
    {
        MutexLocker locker(m_mutex);
        m_transport = nullptr;
    }

    virtual bool submit(CompositorFrame&&) override;

private:
    bool post(IPC::MessageBuffer&);
    bool post_resource_additions_in_batches(Web::CompositorContextId, Compositing::DisplayListResourceTransaction&);
    bool post_display_list_update(Web::CompositorContextId, CompositorFrame::DisplayListUpdate&);

    // Held while a frame is posted, so that frames submitted from different threads do not interleave.
    Mutex m_mutex;
    IPC::Transport* m_transport { nullptr };
};

bool CompositorConnectionFrameSink::post(IPC::MessageBuffer& buffer)
{
    // A transport that failed to take a message is closed, and the connection learns of that on its own thread. Until
    // then, drop the rest of this frame and every later one.
    if (!m_transport->is_open() || buffer.transfer_message(*m_transport).is_error()) {
        m_transport = nullptr;
        return false;
    }
    return true;
}

void CompositorConnection::die()
{
    did_lose_compositor();
}

void CompositorConnection::ensure_video_presentation_channel()
{
    if (!can_send_message_to_compositor())
        return;

    auto media_client_or_error = MediaClient::Client::acquire();
    if (media_client_or_error.is_error()) {
        dbgln("Failed to reach the media server for a video presentation channel: {}", media_client_or_error.error());
        return;
    }
    auto media_client = media_client_or_error.release_value();
    if (m_video_presentation_channel_media_client_generation == media_client->generation())
        return;

    auto handle_or_error = media_client->create_video_presentation_channel();
    if (handle_or_error.is_error()) {
        dbgln("Failed to create video presentation channel: {}", handle_or_error.error());
        return;
    }

    async_offer_video_presentation_channel(handle_or_error.release_value());
    m_video_presentation_channel_media_client_generation = media_client->generation();
    dbgln_if(VIDEO_PRESENTATION_CHANNEL_DEBUG, "WebContent: offered the media server's video presentation channel to Compositor");
}

void CompositorConnection::set_parent_context(Web::CompositorContextId context_id, Optional<Web::CompositorContextId> parent_context_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_set_parent_context(context_id, parent_context_id);
}

void CompositorConnection::stop_presenting_to_client(Web::CompositorContextId context_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_stop_presenting_to_client(context_id);
}

void CompositorConnection::destroy_context(Web::CompositorContextId context_id)
{
    m_pending_async_scroll_updates.remove(context_id);
    if (!can_send_message_to_compositor())
        return;
    async_destroy_context(context_id);
}

// A font backed by raw font data carries the descriptor of its typeface's buffer, and an image frame the descriptor
// of its shared bitmap. One IPC message holds at most IPC::MAX_MESSAGE_FD_COUNT of them, so a transaction's fonts and
// image frames travel ahead of the display list, in messages of at most this many resources each.
static constexpr size_t max_resources_per_message = 100;
static_assert(max_resources_per_message <= IPC::MAX_MESSAGE_FD_COUNT);

bool CompositorConnectionFrameSink::post_resource_additions_in_batches(Web::CompositorContextId context_id, Compositing::DisplayListResourceTransaction& resource_transaction)
{
    auto fonts = move(resource_transaction.fonts);
    auto image_frames = move(resource_transaction.image_frames);

    // Moves up to `room` resources of one kind into `batch`, starting at `taken`, and returns how many it moved.
    auto take = [](auto& resources, size_t& taken, size_t room, auto& batch) {
        auto count = min(room, resources.size() - taken);
        batch.ensure_capacity(count);
        for (size_t i = 0; i < count; ++i)
            batch.unchecked_append(move(resources[taken + i]));
        taken += count;
        return count;
    };

    size_t fonts_taken = 0;
    size_t image_frames_taken = 0;
    while (fonts_taken < fonts.size() || image_frames_taken < image_frames.size()) {
        Compositing::DisplayListResourceTransaction batch;
        auto room = max_resources_per_message;
        room -= take(fonts, fonts_taken, room, batch.fonts);
        room -= take(image_frames, image_frames_taken, room, batch.image_frames);
        auto encoded_batch = MUST(Messages::CompositorWebContentServer::UpdateDisplayListResources::static_encode(context_id, batch));
        if (!post(encoded_batch))
            return false;
    }
    return true;
}

// Returns false once the compositor is lost. A display list that cannot be placed in shared memory is
// skipped, and the rest of the frame still goes over.
bool CompositorConnectionFrameSink::post_display_list_update(Web::CompositorContextId context_id, CompositorFrame::DisplayListUpdate& update)
{
    if (!post_resource_additions_in_batches(context_id, update.resource_transaction))
        return false;

    // The tape and run table go over in a fresh shared buffer that the Compositor takes ownership of;
    // this process keeps no mapping once the message is posted.
    auto& display_list = *update.display_list;
    auto timer = Core::ElapsedTimer::start_new(Core::TimerType::Precise);
    auto shared_tape_buffer = display_list.copy_to_shared_buffer();
    if (shared_tape_buffer.is_error()) {
        dbgln("WebContent: Could not place a {} byte display list in shared memory: {}", display_list.command_bytes().size(), shared_tape_buffer.error());
        return true;
    }
    auto copy_time = timer.elapsed_time();

    auto encoded_message = MUST(Messages::CompositorWebContentServer::UpdateDisplayList::static_encode(context_id, shared_tape_buffer.value(), display_list.command_bytes().size(), display_list.command_runs().size(), display_list.properties(), update.visual_context_tree, update.resource_transaction, update.scroll_state_snapshot));
    if (!post(encoded_message))
        return false;
    if (display_list_timing_enabled())
        dbgln("DISPLAY_LIST_PUBLISH bytes={} copy={} µs encode+post={} µs", display_list.command_bytes().size(), copy_time.to_microseconds(), (timer.elapsed_time() - copy_time).to_microseconds());
    return true;
}

bool CompositorConnectionFrameSink::submit(CompositorFrame&& frame)
{
    MutexLocker locker(m_mutex);
    if (!m_transport)
        return false;

    auto context_id = frame.context_id;
    if (auto& update = frame.display_list_update; update.has_value()) {
        if (!post_display_list_update(context_id, *update))
            return false;
    }
    if (auto& update = frame.visual_context_tree_update; update.has_value()) {
        if (!post_resource_additions_in_batches(context_id, update->resource_transaction))
            return false;
        auto encoded_message = MUST(Messages::CompositorWebContentServer::UpdateVisualContextTree::static_encode(context_id, update->visual_context_tree, update->resource_transaction));
        if (!post(encoded_message))
            return false;
    }
    if (auto& update = frame.scroll_state_update; update.has_value()) {
        auto encoded_message = MUST(Messages::CompositorWebContentServer::UpdateScrollState::static_encode(context_id, update->scroll_state_snapshot, update->keyboard_scroll_state));
        if (!post(encoded_message))
            return false;
    }
    if (frame.present_viewport_rect.has_value()) {
        auto encoded_message = MUST(Messages::CompositorWebContentServer::PresentFrame::static_encode(context_id, *frame.present_viewport_rect));
        return post(encoded_message);
    }
    return true;
}

CompositorConnection::CompositorConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>(*this, move(transport))
    , m_frame_sink(adopt_ref(*new CompositorConnectionFrameSink(this->transport())))
{
}

CompositorConnection::~CompositorConnection()
{
    m_frame_sink->detach();
}

NonnullRefPtr<CompositorFrameSink> CompositorConnection::frame_sink() const
{
    return m_frame_sink;
}

void CompositorConnection::submit_frame(CompositorFrame&& frame)
{
    if (!can_send_message_to_compositor())
        return;
    if (!m_frame_sink->submit(move(frame)))
        did_lose_compositor();
}

void CompositorConnection::add_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    if (!can_send_message_to_compositor())
        return;
    ensure_video_presentation_channel();
    async_add_video_sink(video_sink_handle);
}

void CompositorConnection::remove_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    if (!can_send_message_to_compositor())
        return;
    async_remove_video_sink(video_sink_handle);
}

void CompositorConnection::set_video_sink_ticking(Media::VideoSinkHandle video_sink_handle, bool should_tick)
{
    if (!can_send_message_to_compositor())
        return;
    async_set_video_sink_ticking(video_sink_handle, should_tick);
}

Optional<Compositing::CanvasId> CompositorConnection::create_canvas_2d_context(Gfx::IntSize size, bool alpha)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::CreateCanvas2dContext>(size, alpha);
    if (!response->success())
        return {};
    return response->canvas_id();
}

void CompositorConnection::update_canvas_2d_stream(Compositing::Canvas2DCommandStream& stream)
{
    // The stream is drained only here, and only when the message can actually
    // be delivered: a flush through a connection that has been lost must leave
    // the segments in place for whoever flushes through a live one.
    if (stream.is_empty() || !can_send_message_to_compositor())
        return;

    auto encoded_message = MUST(Messages::CompositorWebContentServer::UpdateCanvas2dStream::static_encode(stream.take_segments(), stream.take_fonts()));
    if (post_message(encoded_message).is_error())
        did_lose_compositor();
}

void CompositorConnection::destroy_canvas_context(Compositing::CanvasId canvas_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_destroy_canvas_context(canvas_id);
}

Gfx::ShareableBitmap CompositorConnection::get_canvas_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::GetCanvasPixels>(canvas_id, rect);
    return response->take_pixels();
}

Optional<Web::Compositor::PlaceholderCanvasLink> CompositorConnection::allocate_placeholder_canvas()
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::AllocatePlaceholderCanvas>();
    return Web::Compositor::PlaceholderCanvasLink { response->canvas_id(), response->secret() };
}

void CompositorConnection::release_placeholder_canvas(Compositing::CanvasId canvas_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_release_placeholder_canvas(canvas_id);
}

void CompositorConnection::commit_placeholder_canvas(Web::Compositor::PlaceholderCanvasLink link, Optional<Compositing::CanvasId> source_canvas_id, Gfx::IntSize size, bool origin_clean)
{
    if (!can_send_message_to_compositor())
        return;
    async_commit_placeholder_canvas(link.canvas_id, link.secret, source_canvas_id, size, origin_clean);
}

Web::Compositor::PlaceholderCanvasPixels CompositorConnection::get_placeholder_canvas_pixels(Compositing::CanvasId canvas_id, Gfx::IntRect rect)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::GetPlaceholderCanvasPixels>(canvas_id, rect);
    auto pixels = response->take_pixels();
    return { pixels.is_valid() ? pixels.bitmap() : nullptr, response->origin_clean() };
}

void CompositorConnection::invalidate_keyboard_scroll_state(Web::CompositorContextId context_id, u64 generation)
{
    if (!can_send_message_to_compositor())
        return;
    // Input acknowledgments travel over a different connection. Finish invalidating before acknowledging an input
    // that changed focus, so the UI cannot admit the next key against the previous target.
    if (!send_sync_but_allow_failure<Messages::CompositorWebContentServer::InvalidateKeyboardScrollState>(context_id, generation))
        did_lose_compositor();
}

void CompositorConnection::invalidate_wheel_event_listener_state(Web::CompositorContextId context_id, u64 generation)
{
    if (!can_send_message_to_compositor())
        return;
    async_invalidate_wheel_event_listener_state(context_id, generation);
}

Compositing::AsyncScrollEnqueueResult CompositorConnection::async_scroll_by(Web::CompositorContextId context_id, Web::UniqueNodeID document_id, Gfx::FloatPoint position, Gfx::FloatPoint delta, Gfx::IntRect viewport_rect, Web::WheelDeltaPrecision wheel_delta_precision, Web::ScrollGesturePhase scroll_gesture_phase, u32 modifiers, Compositing::AsyncScrollOperationTracking operation_tracking)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::AsyncScrollBy>(context_id, document_id, position, delta, viewport_rect, wheel_delta_precision, scroll_gesture_phase, modifiers, operation_tracking);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    return response->take_result();
}

Compositing::AsyncScrollEnqueueResult CompositorConnection::smooth_scroll_to(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stable_node_id, Gfx::FloatPoint offset, Gfx::FloatPoint main_thread_offset, Gfx::IntRect viewport_rect, Compositing::ScrollAnimationKind animation_kind, Compositing::SmoothScrollInitiator initiator)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::SmoothScrollTo>(context_id, stable_node_id, offset, main_thread_offset, viewport_rect, animation_kind, initiator);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    return response->take_result();
}

void CompositorConnection::cancel_smooth_scroll(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stable_node_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_cancel_smooth_scroll(context_id, stable_node_id);
}

Compositing::PendingAsyncScrollUpdates CompositorConnection::take_pending_async_scroll_updates(Web::CompositorContextId context_id, Compositing::AsyncScrollUpdateFreshness freshness)
{
    // The compositor process pushes its scroll updates as it makes them, in order with the input
    // events it forwards, so a rendering update finds the latest ones here. A reader that needs the
    // compositor's state as of this instant asks for what it has not pushed yet, and first merges
    // the pushes that arrived ahead of that answer but were not dispatched during the wait.
    if (freshness == Compositing::AsyncScrollUpdateFreshness::FromCompositor && can_send_message_to_compositor()) {
        auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::TakePendingAsyncScrollUpdates>(context_id);
        // The pushes that arrived ahead of the answer and the answer itself are publications of the
        // same queue; they merge in the order the compositor published them, whatever order the
        // transport handed them over in, so a gesture state settles as the newest publication says.
        struct Arrival {
            Web::CompositorContextId context_id;
            Compositing::PendingAsyncScrollUpdates updates;
        };
        Vector<Arrival> arrivals;
        for (auto& message : take_unprocessed_messages(Messages::CompositorWebContentClient::AsyncScrollUpdates::ENDPOINT_MAGIC, Messages::CompositorWebContentClient::AsyncScrollUpdates::static_message_id())) {
            auto& pushed = static_cast<Messages::CompositorWebContentClient::AsyncScrollUpdates&>(*message);
            arrivals.append({ pushed.context_id(), pushed.updates() });
        }
        if (!response)
            did_lose_compositor();
        else
            arrivals.append({ context_id, response->take_updates() });
        quick_sort(arrivals, [](auto const& a, auto const& b) { return a.updates.sequence < b.updates.sequence; });
        for (auto& arrival : arrivals)
            merge_async_scroll_updates(arrival.context_id, move(arrival.updates));
    }

    auto pending = m_pending_async_scroll_updates.get(context_id);
    if (!pending.has_value())
        return {};
    Compositing::PendingAsyncScrollUpdates updates;
    updates.sequence = pending->sequence;
    updates.scroll_offsets = move(pending->scroll_offsets);
    updates.completed_operation_ids = move(pending->completed_operation_ids);
    updates.operation_ids_taken_over_by_user_input = move(pending->operation_ids_taken_over_by_user_input);
    updates.started_user_scrolls = move(pending->started_user_scrolls);
    updates.document_id = pending->document_id;
    updates.user_scroll_gesture_in_progress = pending->user_scroll_gesture_in_progress;
    updates.user_scroll_gesture_ended = pending->user_scroll_gesture_ended;
    // Whether a gesture is in progress is a state the compositor process keeps current; the rest
    // was consumed here.
    pending->scroll_offsets.clear();
    pending->completed_operation_ids.clear();
    pending->operation_ids_taken_over_by_user_input.clear();
    pending->started_user_scrolls.clear();
    pending->user_scroll_gesture_ended = false;
    return updates;
}

void CompositorConnection::async_scroll_updates(Web::CompositorContextId context_id, Compositing::PendingAsyncScrollUpdates updates)
{
    merge_async_scroll_updates(context_id, move(updates));
}

void CompositorConnection::merge_async_scroll_updates(Web::CompositorContextId context_id, Compositing::PendingAsyncScrollUpdates updates)
{
    auto& pending = m_pending_async_scroll_updates.ensure(context_id);
    // Whether a gesture is in progress is a state, not an event: the newest publication decides it.
    bool const is_newest = updates.sequence >= pending.sequence;
    pending.sequence = max(pending.sequence, updates.sequence);
    for (auto const& scroll_offset : updates.scroll_offsets) {
        auto existing = pending.scroll_offsets.find_if([&](auto const& existing) { return existing.stable_node_id == scroll_offset.stable_node_id; });
        if (existing != pending.scroll_offsets.end()) {
            existing->compositor_scroll_offset = scroll_offset.compositor_scroll_offset;
            existing->unadopted_scroll_delta.translate_by(scroll_offset.unadopted_scroll_delta);
        } else {
            pending.scroll_offsets.append(scroll_offset);
        }
    }
    pending.completed_operation_ids.extend(move(updates.completed_operation_ids));
    pending.operation_ids_taken_over_by_user_input.extend(move(updates.operation_ids_taken_over_by_user_input));
    pending.started_user_scrolls.extend(move(updates.started_user_scrolls));
    if (is_newest) {
        pending.document_id = updates.document_id;
        pending.user_scroll_gesture_in_progress = updates.user_scroll_gesture_in_progress;
    }
    pending.user_scroll_gesture_ended |= updates.user_scroll_gesture_ended;
}

void CompositorConnection::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (!can_send_message_to_compositor())
        return;
    async_viewport_size_updated(context_id, viewport_size, window_resize_in_progress);
}

bool CompositorConnection::request_rendering_opportunity(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    if (!can_send_message_to_compositor())
        return false;
    async_request_rendering_opportunity(context_id, maximum_frames_per_second);
    return true;
}

void CompositorConnection::hurry_rendering_opportunity(Web::CompositorContextId context_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_hurry_rendering_opportunity(context_id);
}

Optional<Compositing::CanvasId> CompositorConnection::create_webgl_context(Compositing::WebGL::WebGLVersion webgl_version, Gfx::IntSize size, bool depth, bool stencil, bool antialias, Vector<String>& out_supported_extensions)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::CreateWebglContext>(webgl_version, size, depth, stencil, antialias);
    out_supported_extensions = response->take_supported_extensions();
    if (!response->success())
        return {};
    return response->canvas_id();
}

void CompositorConnection::set_webgl_command_buffer(Compositing::CanvasId canvas_id, Core::AnonymousBuffer const& command_buffer)
{
    if (!can_send_message_to_compositor())
        return;

    auto encoded_message = MUST(Messages::CompositorWebContentServer::WebglSetCommandBuffer::static_encode(canvas_id, command_buffer));
    if (post_message(encoded_message).is_error())
        did_lose_compositor();
}

void CompositorConnection::send_webgl_commands_from_shared_buffer(Compositing::CanvasId canvas_id, u64 offset, u64 size_in_bytes, u64 flush_sequence_number, Vector<Gfx::DecodedImageFrame> const& bitmaps)
{
    if (!can_send_message_to_compositor())
        return;

    auto encoded_message = MUST(Messages::CompositorWebContentServer::WebglCommandsFromSharedBuffer::static_encode(canvas_id, offset, size_in_bytes, flush_sequence_number, bitmaps));
    if (post_message(encoded_message).is_error())
        did_lose_compositor();
}

bool CompositorConnection::drain_webgl_command_buffer(Compositing::CanvasId canvas_id)
{
    if (!can_send_message_to_compositor())
        return false;

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::WebglDrainCommandBuffer>(canvas_id);
    if (!response) {
        did_lose_compositor();
        return false;
    }
    return true;
}

void CompositorConnection::send_webgl_commands(Compositing::CanvasId canvas_id, ByteBuffer const& commands, Vector<Gfx::DecodedImageFrame> const& bitmaps)
{
    if (!can_send_message_to_compositor())
        return;

    auto shared_commands = MUST(Core::AnonymousBuffer::create_with_size(commands.size()));
    commands.bytes().copy_to({ shared_commands.data<u8>(), shared_commands.size() });

    auto encoded_message = MUST(Messages::CompositorWebContentServer::WebglCommands::static_encode(canvas_id, shared_commands, bitmaps));
    if (post_message(encoded_message).is_error())
        did_lose_compositor();
}

void CompositorConnection::present_webgl_canvas(Compositing::CanvasId canvas_id, bool preserve_drawing_buffer)
{
    if (!can_send_message_to_compositor())
        return;

    async_webgl_present_canvas(canvas_id, preserve_drawing_buffer);
}

void CompositorConnection::clear_webgl_drawing_buffer(Compositing::CanvasId canvas_id)
{
    if (!can_send_message_to_compositor())
        return;

    async_webgl_clear_drawing_buffer(canvas_id);
}

ByteBuffer CompositorConnection::webgl_sync_call(Compositing::CanvasId canvas_id, ByteBuffer request)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::WebglSyncCall>(canvas_id, move(request));
    return response->take_reply();
}

Compositing::WebGL::ReadPixelsResult CompositorConnection::read_webgl_pixels(Compositing::CanvasId canvas_id, Compositing::WebGL::GLint x, Compositing::WebGL::GLint y, Compositing::WebGL::GLsizei width, Compositing::WebGL::GLsizei height, Compositing::WebGL::GLenum format, Compositing::WebGL::GLenum type, Compositing::WebGL::GLsizei buf_size, Core::AnonymousBuffer const& pixels)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync<Messages::CompositorWebContentServer::WebglReadPixels>(canvas_id, x, y, width, height, format, type, buf_size, pixels);
    return {
        .length = response->length(),
        .columns = response->columns(),
        .rows = response->rows(),
    };
}

bool CompositorConnection::read_webgl_buffer_sub_data(Compositing::CanvasId canvas_id, Compositing::WebGL::GLenum target, Compositing::WebGL::GLintptr offset, Compositing::WebGL::GLintptr size, Core::AnonymousBuffer const& data)
{
    if (!can_send_message_to_compositor())
        return false;

    auto response = send_sync<Messages::CompositorWebContentServer::WebglReadBufferSubData>(canvas_id, target, offset, size, data);
    return response->success();
}

void CompositorConnection::request_screenshot(Web::CompositorContextId context_id, NonnullRefPtr<Gfx::PaintingSurface> target_surface, Function<void()>&& callback)
{
    if (!can_send_message_to_compositor()) {
        if (callback)
            callback();
        return;
    }

    auto target_bitmap = MUST(Gfx::Bitmap::create_shareable(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, target_surface->size()));
    auto shareable_bitmap = Gfx::ShareableBitmap { target_bitmap, Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
    auto request_id = Compositing::ScreenshotRequestId { m_next_screenshot_request_id++ };
    m_screenshots.set(request_id, PendingScreenshot { move(target_surface), move(target_bitmap), move(callback) });
    async_request_screenshot(context_id, request_id, move(shareable_bitmap));
}

void CompositorConnection::key_event(u64 page_id, Web::KeyEvent event)
{
    if (on_key_event)
        on_key_event(Web::PageId { page_id }, move(event));
}

void CompositorConnection::mouse_event(u64 page_id, Web::MouseEvent event)
{
    if (on_mouse_event)
        on_mouse_event(Web::PageId { page_id }, move(event));
}

// NB: A page hosting an isolated iframe paints through the context of that local root, not of a traversable.
void CompositorConnection::request_rendering_update()
{
    for (auto& navigable : Web::HTML::all_local_navigables()) {
        if (navigable->is_local_root() && navigable->has_compositor_context())
            navigable->page().client().request_frame();
    }
}

void CompositorConnection::rendering_opportunity(Web::CompositorContextId context_id, i64 frame_time_nanoseconds, double frame_interval_milliseconds)
{
    for (auto& navigable : Web::HTML::all_local_navigables()) {
        if (!navigable->is_local_root() || !navigable->has_compositor_context())
            continue;
        if (navigable->compositor_context().id() != context_id)
            continue;
        navigable->page().client().rendering_opportunity(frame_time_nanoseconds, frame_interval_milliseconds);
        return;
    }
}

void CompositorConnection::placeholder_canvas_committed(Compositing::CanvasId canvas_id, Gfx::IntSize size, bool origin_clean)
{
    Web::HTML::HTMLCanvasElement::placeholder_frame_committed(canvas_id, size, origin_clean);
}

void CompositorConnection::did_complete_screenshot(Compositing::ScreenshotRequestId request_id)
{
    auto pending_screenshot = take_screenshot(request_id);
    if (!pending_screenshot.has_value())
        return;

    pending_screenshot->target_surface->write_from_bitmap(*pending_screenshot->target_bitmap);
    if (pending_screenshot->callback)
        pending_screenshot->callback();
}

void CompositorConnection::did_fail_screenshot(Compositing::ScreenshotRequestId request_id)
{
    auto pending_screenshot = take_screenshot(request_id);
    if (!pending_screenshot.has_value())
        return;

    if (pending_screenshot->callback)
        pending_screenshot->callback();
}

void CompositorConnection::did_lose_compositor()
{
    if (m_has_lost_compositor)
        return;
    m_has_lost_compositor = true;
    m_frame_sink->detach();

    for (auto& entry : m_screenshots) {
        if (entry.value.callback)
            entry.value.callback();
    }
    m_screenshots.clear();

    if (on_compositor_lost)
        on_compositor_lost();
}

bool CompositorConnection::can_send_message_to_compositor() const
{
    return !m_has_lost_compositor && is_open();
}

Optional<CompositorConnection::PendingScreenshot> CompositorConnection::take_screenshot(Compositing::ScreenshotRequestId request_id)
{
    return m_screenshots.take(request_id);
}

}
