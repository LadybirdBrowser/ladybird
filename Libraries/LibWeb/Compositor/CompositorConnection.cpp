/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/Compositor/CompositorConnection.h>

#include <AK/Debug.h>
#include <AK/Mutex.h>
#include <AK/NeverDestroyed.h>
#include <LibCompositing/CanvasColorSpace.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/ElapsedTimer.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Font/Font.h>
#include <LibGfx/Font/Typeface.h>
#include <LibIPC/File.h>
#include <LibIPC/Limits.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibMediaClient/Client.h>
#include <LibWeb/Compositor/RenderClock.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Canvas/PredefinedColorSpace.h>
#include <LibWeb/HTML/EventLoop/EventLoop.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/Layout/LayoutRustFFI.h>
#include <LibWeb/Page/EventHandler.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

// The Paint stage presents frames, and only the connection hands it the poster it presents them through, and the
// screenshots it asks for after them.
extern "C" {
void paint_stage_adopt_compositor_poster(IPC::FfiThreadSafeMessagePoster poster);
void paint_stage_request_screenshot(void* request);
}

namespace Web::Compositor {

void CompositorConnection::die()
{
    did_lose_compositor();
}

void CompositorConnection::ensure_video_presentation_channel()
{
    if (!can_send_message_to_compositor() || !on_request_video_presentation_channel)
        return;

    // Acquiring the client has the Browser connect this process to a media server, which the Browser then connects to
    // the Compositor on request. A replacement server needs a channel of its own.
    auto media_client_or_error = MediaClient::Client::acquire();
    if (media_client_or_error.is_error()) {
        dbgln("Failed to reach the media server for a video presentation channel: {}", media_client_or_error.error());
        return;
    }
    auto media_client = media_client_or_error.release_value();
    if (m_video_presentation_channel_media_client_generation == media_client->generation())
        return;

    on_request_video_presentation_channel();
    m_video_presentation_channel_media_client_generation = media_client->generation();
    dbgln_if(VIDEO_PRESENTATION_CHANNEL_DEBUG, "WebContent: asked the Browser for a video presentation channel to the Compositor");
}

void CompositorConnection::attach_render_clock(IPC::TransportHandle handle)
{
    if (!can_send_message_to_compositor())
        return;
    if (auto result = RenderClock::the().attach(move(handle)); result.is_error())
        dbgln("Failed to attach the render clock to the compositor: {}", result.error());
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

// An image frame carries the descriptor of its shared bitmap. One IPC message holds at most IPC::MAX_MESSAGE_FD_COUNT
// of them, so image frames are registered in messages of at most this many image frames each.
static constexpr size_t max_image_frames_per_message = 100;
static_assert(max_image_frames_per_message <= IPC::MAX_MESSAGE_FD_COUNT);

struct ResourcesRegisteredWithCompositor {
    HashTable<u64> typeface_ids;
    HashMap<u64, Gfx::DecodedImageFrame::SharedIdentity const*> image_frame_identities_by_id;
};

struct ResourcesRegisteredWithCompositors {
    Mutex mutex;
    HashMap<IPC::ThreadSafeMessagePoster*, ResourcesRegisteredWithCompositor> resources_by_poster;
};

static ResourcesRegisteredWithCompositors& resources_registered_with_compositors()
{
    static NeverDestroyed<ResourcesRegisteredWithCompositors> registry;
    return *registry;
}

static void unregister_destroyed_typeface(u64 typeface_id)
{
    auto& registry = resources_registered_with_compositors();
    MutexLocker locker(registry.mutex);
    for (auto& [poster, resources] : registry.resources_by_poster) {
        if (!resources.typeface_ids.remove(typeface_id))
            continue;
        auto encoded_message = MUST(Messages::CompositorWebContentServer::UnregisterTypeface::static_encode(typeface_id));
        (void)poster->try_post(encoded_message);
    }
}

// A frame decoded again takes the id of the frame it replaces, so an id is unregistered only by the identity that is
// registered under it.
static void unregister_destroyed_image_frame(Gfx::DecodedImageFrame::SharedIdentity const& identity)
{
    auto& registry = resources_registered_with_compositors();
    MutexLocker locker(registry.mutex);
    for (auto& [poster, resources] : registry.resources_by_poster) {
        auto registered_identity = resources.image_frame_identities_by_id.find(identity.id());
        if (registered_identity == resources.image_frame_identities_by_id.end() || registered_identity->value != &identity)
            continue;
        resources.image_frame_identities_by_id.remove(registered_identity);
        auto encoded_message = MUST(Messages::CompositorWebContentServer::UnregisterImageFrame::static_encode(Compositing::ImageFrameResourceId { identity.id() }));
        (void)poster->try_post(encoded_message);
    }
}

static void start_registering_resources_with_compositor(IPC::ThreadSafeMessagePoster& poster)
{
    Gfx::Typeface::set_destruction_observer(unregister_destroyed_typeface);
    Gfx::DecodedImageFrame::set_identity_destruction_observer(unregister_destroyed_image_frame);
    auto& registry = resources_registered_with_compositors();
    MutexLocker locker(registry.mutex);
    registry.resources_by_poster.set(&poster, {});
}

static void stop_registering_resources_with_compositor(IPC::ThreadSafeMessagePoster& poster)
{
    auto& registry = resources_registered_with_compositors();
    MutexLocker locker(registry.mutex);
    registry.resources_by_poster.remove(&poster);
}

// The registration of a typeface is posted while the registry is locked, so that it reaches the compositor ahead of
// whatever any thread then posts that names the typeface.
static bool register_typefaces_of_fonts_with_compositor(IPC::ThreadSafeMessagePoster& poster, ReadonlySpan<Gfx::Font const*> fonts)
{
    auto& registry = resources_registered_with_compositors();
    MutexLocker locker(registry.mutex);
    auto resources = registry.resources_by_poster.find(&poster);
    if (resources == registry.resources_by_poster.end())
        return false;
    for (auto const* font : fonts) {
        auto const& typeface = font->typeface();
        if (resources->value.typeface_ids.contains(typeface.id()))
            continue;
        auto encoded_message = MUST(Messages::CompositorWebContentServer::RegisterTypeface::static_encode(Compositing::TypefaceRegistration { typeface.id(), typeface }));
        if (!poster.try_post(encoded_message))
            return false;
        resources->value.typeface_ids.set(typeface.id());
    }
    return true;
}

// The image frames stay registered for as long as the caller holds them, which has to be until it has posted whatever
// names them. Their registrations are posted while the registry is locked, as those of typefaces are.
static bool register_image_frames_with_compositor(IPC::ThreadSafeMessagePoster& poster, ReadonlySpan<Gfx::DecodedImageFrame const*> image_frames)
{
    auto& registry = resources_registered_with_compositors();
    MutexLocker locker(registry.mutex);
    auto resources = registry.resources_by_poster.find(&poster);
    if (resources == registry.resources_by_poster.end())
        return false;
    auto& registered_identities = resources->value.image_frame_identities_by_id;
    Vector<Compositing::DisplayListImageFrameResource> batch;
    auto post_batch = [&] {
        auto encoded_message = MUST(Messages::CompositorWebContentServer::RegisterImageFrames::static_encode(batch));
        batch.clear();
        return poster.try_post(encoded_message);
    };
    for (auto const* image_frame : image_frames) {
        if (registered_identities.get(image_frame->id()).value_or(nullptr) == &image_frame->identity())
            continue;
        registered_identities.set(image_frame->id(), &image_frame->identity());
        batch.append({ Compositing::ImageFrameResourceId { image_frame->id() }, *image_frame });
        if (batch.size() == max_image_frames_per_message && !post_batch())
            return false;
    }
    return batch.is_empty() || post_batch();
}

static Vector<Compositing::DisplayListFontDescription> descriptions_of_fonts(ReadonlySpan<Gfx::Font const*> fonts)
{
    Vector<Compositing::DisplayListFontDescription> descriptions;
    descriptions.ensure_capacity(fonts.size());
    for (auto const* font : fonts)
        descriptions.unchecked_append(Compositing::DisplayListFontDescription::of(*font));
    return descriptions;
}

// Shared memory Rust writes a message into, which stays mapped until Rust releases it.
struct SharedBufferForRust {
    AK_ALLOC_WITH_KMALLOC;

    Core::AnonymousBuffer buffer;
};

// Registers what `fonts` and `image_frames` need registered, posts the descriptions of `fonts` for the context to
// hold, and answers whether the compositor could still be reached.
static bool post_fonts_and_register_image_frames(IPC::ThreadSafeMessagePoster& poster, Web::CompositorContextId context_id, ReadonlySpan<Gfx::Font const*> fonts, ReadonlySpan<Gfx::DecodedImageFrame const*> image_frames)
{
    if (!register_image_frames_with_compositor(poster, image_frames))
        return false;
    if (fonts.is_empty())
        return true;
    if (!register_typefaces_of_fonts_with_compositor(poster, fonts))
        return false;
    auto encoded_message = MUST(Messages::CompositorWebContentServer::DescribeDisplayListFonts::static_encode(context_id, descriptions_of_fonts(fonts)));
    return poster.try_post(encoded_message);
}

static Atomic<u64> s_next_connection_serial { 1 };

CompositorConnection::CompositorConnection(NonnullOwnPtr<IPC::Transport> transport)
    : IPC::ConnectionToServer<CompositorWebContentClientEndpoint, CompositorWebContentServerEndpoint>(*this, move(transport))
    , m_poster(IPC::ThreadSafeMessagePoster::create(this->transport()))
    , m_serial(s_next_connection_serial++)
{
    start_registering_resources_with_compositor(*m_poster);
}

CompositorConnection::~CompositorConnection()
{
    stop_registering_resources_with_compositor(*m_poster);
    m_poster->detach_from_transport();
}

// A screenshot of what the compositor composes for a context, on its way to the Paint thread, which asks for it after
// the frames handed to it before.
struct ScreenshotRequestForPaintThread {
    AK_ALLOC_WITH_KMALLOC;

    Web::CompositorContextId context_id;
    Compositing::ScreenshotRequestId request_id;
    Gfx::ShareableBitmap target;
};

void CompositorConnection::hand_poster_to_paint_thread()
{
    paint_stage_adopt_compositor_poster(m_poster->to_ffi_with_one_reference());
}

// Gives the compositor a connection of its own to this process's image decoder, so that an animated image keeps getting
// frames while the main thread is busy.
void CompositorConnection::ensure_image_decoder_channel()
{
    auto& image_codec = Platform::ImageCodecPlugin::the();
    if (m_image_decoder_channel_client_generation == image_codec.client_generation())
        return;

    auto channel_or_error = IPC::TransportHandle::create_pair();
    if (channel_or_error.is_error()) {
        dbgln("Failed to create an image decoder channel for the Compositor: {}", channel_or_error.error());
        return;
    }
    auto channel = channel_or_error.release_value();
    image_codec.connect_frame_client(move(channel.first));
    async_connect_to_image_decoder(channel.second);
    m_image_decoder_channel_client_generation = image_codec.client_generation();
}

void CompositorConnection::add_animated_image(u64 frame_id, AnimatedImageRegistration const& image)
{
    if (!can_send_message_to_compositor())
        return;
    ensure_image_decoder_channel();
    // NB: Session ids start over with each decoder, so a session of a replaced decoder could name one of another image.
    auto session_id = image.image_decoder_client_generation == Platform::ImageCodecPlugin::the().client_generation() ? image.session_id : 0;
    async_add_animated_image(frame_id, session_id, image.size, image.color_space, image.frame_durations_in_milliseconds, image.loop_count);
}

void CompositorConnection::remove_animated_image(u64 frame_id)
{
    if (can_send_message_to_compositor())
        async_remove_animated_image(frame_id);
}

void CompositorConnection::start_animated_image(u64 frame_id)
{
    if (can_send_message_to_compositor())
        async_start_animated_image(frame_id);
}

void CompositorConnection::stop_animated_image(u64 frame_id)
{
    if (can_send_message_to_compositor())
        async_stop_animated_image(frame_id);
}

AnimatedImageState CompositorConnection::animated_image_state_for_testing(u64 frame_id)
{
    if (!can_send_message_to_compositor())
        return {};
    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::AnimatedImageStateForTesting>(frame_id);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    return { .is_animating = response->is_animating(), .frame_index = response->frame_index(), .shown_frame_index = response->shown_frame_index(), .loops_completed = response->loops_completed() };
}

void CompositorConnection::add_video_sink(Media::VideoSinkHandle video_sink_handle, NonnullRefPtr<Compositing::VideoFramePresentationPage> presentation_page)
{
    if (!can_send_message_to_compositor())
        return;
    ensure_video_presentation_channel();
    async_add_video_sink(video_sink_handle, move(presentation_page));
}

void CompositorConnection::remove_video_sink(Media::VideoSinkHandle video_sink_handle)
{
    if (!can_send_message_to_compositor())
        return;
    async_remove_video_sink(video_sink_handle);
}

void CompositorConnection::release_video_frame_hold(Media::VideoSinkHandle video_sink_handle, u64 generation)
{
    if (!can_send_message_to_compositor())
        return;
    async_release_video_frame_hold(video_sink_handle, generation);
}

void CompositorConnection::set_video_sink_ticking(Media::VideoSinkHandle video_sink_handle, bool should_tick)
{
    if (!can_send_message_to_compositor())
        return;
    async_set_video_sink_ticking(video_sink_handle, should_tick);
}

RefPtr<IPC::ThreadSafeMessagePoster> CompositorConnection::poster_while_compositor_is_reachable()
{
    if (!can_send_message_to_compositor())
        return nullptr;
    return m_poster;
}

bool CompositorConnection::post_canvas_2d_stream_batch_fonts_and_image_frames(ReadonlySpan<Gfx::Font const*> fonts, ReadonlySpan<Gfx::DecodedImageFrame const*> image_frames)
{
    if (!register_typefaces_of_fonts_with_compositor(*m_poster, fonts) || !register_image_frames_with_compositor(*m_poster, image_frames)) {
        did_lose_compositor();
        return false;
    }
    if (fonts.is_empty())
        return true;
    auto encoded_message = MUST(Messages::CompositorWebContentServer::DescribeCanvas2dFonts::static_encode(descriptions_of_fonts(fonts)));
    if (!m_poster->try_post(encoded_message)) {
        did_lose_compositor();
        return false;
    }
    return true;
}

void CompositorConnection::did_fail_to_post_canvas_2d_message()
{
    did_lose_compositor();
}

bool CompositorConnection::register_image_frames(ReadonlySpan<Gfx::DecodedImageFrame const*> image_frames)
{
    if (!can_send_message_to_compositor())
        return false;
    return register_image_frames_with_compositor(*m_poster, image_frames);
}

Optional<Web::Compositor::PlaceholderCanvasLink> CompositorConnection::allocate_placeholder_canvas()
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::AllocatePlaceholderCanvas>();
    if (!response) {
        did_lose_compositor();
        return {};
    }
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

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::GetPlaceholderCanvasPixels>(canvas_id, rect);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    auto pixels = response->take_pixels();
    return { pixels.is_valid() ? pixels.bitmap() : nullptr, response->origin_clean() };
}

bool CompositorConnection::rasterize_image(Layout::RustFFI::FfiImageRasterization const& image, NonnullRefPtr<Gfx::Bitmap> target)
{
    if (!can_send_message_to_compositor())
        return false;

    // The compositor draws straight into the target, so it has to be memory both processes can map.
    VERIFY(target->anonymous_buffer().is_valid());
    auto token = m_next_rasterization_token++;
    return did_rasterize(Layout::RustFFI::render_state_rasterize_image(to_ffi_with_one_reference(), m_poster->to_ffi_with_one_reference(), token, &image, target.ptr()));
}

bool CompositorConnection::rasterize_last_recording(Layout::RustFFI::DocumentHost const& document_host, Layout::BegunRead const& read, NonnullRefPtr<Gfx::Bitmap> target)
{
    if (!can_send_message_to_compositor())
        return false;

    // The compositor draws straight into the target, so it has to be memory both processes can map.
    VERIFY(target->anonymous_buffer().is_valid());
    auto token = m_next_rasterization_token++;
    return did_rasterize(Layout::RustFFI::render_state_rasterize_last_recording(to_ffi_with_one_reference(), m_poster->to_ffi_with_one_reference(), token, &document_host, &read, target.ptr()));
}

bool CompositorConnection::did_rasterize(Layout::RustFFI::RasterizationOutcome outcome)
{
    switch (outcome) {
    case Layout::RustFFI::RasterizationOutcome::Rasterized:
        return true;
    case Layout::RustFFI::RasterizationOutcome::NotRasterized:
        return false;
    case Layout::RustFFI::RasterizationOutcome::CompositorLost:
        did_lose_compositor();
        return false;
    }
    VERIFY_NOT_REACHED();
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

Compositing::WheelEventAnswer CompositorConnection::take_wheel_event_from_main_thread(Web::CompositorContextId context_id, Web::MouseEvent const& event, Compositing::AsyncScrollOperationTracking operation_tracking, Compositing::WheelEventRouting routing)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::TakeWheelEventFromMainThread>(context_id, event.clone_without_browser_data(), operation_tracking, routing);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    return response->take_answer();
}

Compositing::WheelEventAnswer CompositorConnection::wheel_event_let_through(Web::CompositorContextId context_id, Compositing::WheelStepLetThrough const& step, Compositing::AsyncScrollOperationTracking operation_tracking)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::WheelEventLetThrough>(context_id, step, operation_tracking);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    return response->take_answer();
}

void CompositorConnection::wheel_event_canceled(Web::CompositorContextId context_id, u64 wheel_gesture_id)
{
    if (!can_send_message_to_compositor())
        return;
    async_wheel_event_canceled(context_id, wheel_gesture_id);
}

String CompositorConnection::scroll_state_for_testing(Web::CompositorContextId context_id, Web::AsyncScrollNodeStableID stretched_scroller)
{
    if (!can_send_message_to_compositor())
        return {};

    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::ScrollStateForTesting>(context_id, stretched_scroller);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    return response->take_scroll_state();
}

void CompositorConnection::synchronize_for_testing()
{
    if (!can_send_message_to_compositor())
        return;
    if (!send_sync_but_allow_failure<Messages::CompositorWebContentServer::SynchronizeForTesting>())
        did_lose_compositor();
}

Optional<Compositing::FirstWheelStepRouteForTesting> CompositorConnection::route_first_wheel_step_for_testing(Web::CompositorContextId context_id, Gfx::FloatPoint position, Gfx::FloatPoint delta_in_css_pixels)
{
    if (!can_send_message_to_compositor())
        return {};
    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::RouteFirstWheelStepForTesting>(context_id, position, delta_in_css_pixels);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    if (!response->decision().has_value())
        return {};
    return Compositing::FirstWheelStepRouteForTesting { .route = { *response->decision(), response->scroller() }, .document_id = response->document_id() };
}

void CompositorConnection::begin_input_trace_for_testing(Web::CompositorContextId context_id)
{
    if (!can_send_message_to_compositor())
        return;
    if (!send_sync_but_allow_failure<Messages::CompositorWebContentServer::BeginInputTraceForTesting>(context_id))
        did_lose_compositor();
}

void CompositorConnection::clear_per_test_input_state_for_testing(Web::CompositorContextId context_id)
{
    if (!can_send_message_to_compositor())
        return;
    if (!send_sync_but_allow_failure<Messages::CompositorWebContentServer::ClearPerTestInputStateForTesting>(context_id))
        did_lose_compositor();
}

Vector<Web::InputTraceRecord> CompositorConnection::take_input_trace_for_testing(Web::CompositorContextId context_id)
{
    if (!can_send_message_to_compositor())
        return {};
    auto response = send_sync_but_allow_failure<Messages::CompositorWebContentServer::TakeInputTraceForTesting>(context_id);
    if (!response) {
        did_lose_compositor();
        return {};
    }
    return response->take_records();
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
    updates.applied_scroll_requests = move(pending->applied_scroll_requests);
    updates.document_id = pending->document_id;
    updates.boxes_in_user_scroll_gesture = pending->boxes_in_user_scroll_gesture;
    updates.boxes_whose_user_scroll_gesture_ended = move(pending->boxes_whose_user_scroll_gesture_ended);
    updates.wheel_latched_scroller = pending->wheel_latched_scroller;
    // Whether a gesture is in progress is a state the compositor process keeps current; the rest
    // was consumed here.
    pending->scroll_offsets.clear();
    pending->completed_operation_ids.clear();
    pending->operation_ids_taken_over_by_user_input.clear();
    pending->started_user_scrolls.clear();
    pending->applied_scroll_requests.clear();
    pending->boxes_whose_user_scroll_gesture_ended.clear();
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
            existing->merge_later_scroll(scroll_offset);
        } else {
            pending.scroll_offsets.append(scroll_offset);
        }
    }
    pending.completed_operation_ids.extend(move(updates.completed_operation_ids));
    pending.operation_ids_taken_over_by_user_input.extend(move(updates.operation_ids_taken_over_by_user_input));
    pending.started_user_scrolls.extend(move(updates.started_user_scrolls));
    pending.applied_scroll_requests.extend(move(updates.applied_scroll_requests));
    if (is_newest) {
        pending.document_id = updates.document_id;
        pending.boxes_in_user_scroll_gesture = move(updates.boxes_in_user_scroll_gesture);
        pending.wheel_latched_scroller = updates.wheel_latched_scroller;
    }
    for (auto box : updates.boxes_whose_user_scroll_gesture_ended) {
        if (!pending.boxes_whose_user_scroll_gesture_ended.contains_slow(box))
            pending.boxes_whose_user_scroll_gesture_ended.append(box);
    }
}

void CompositorConnection::viewport_size_updated(Web::CompositorContextId context_id, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::WindowResizingInProgress window_resize_in_progress)
{
    if (!can_send_message_to_compositor())
        return;
    async_viewport_size_updated(context_id, viewport_size, device_pixel_ratio, window_resize_in_progress);
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

void CompositorConnection::request_screenshot(Web::CompositorContextId context_id, NonnullRefPtr<Gfx::Bitmap> target_bitmap, Function<void(ScreenshotResult)>&& callback)
{
    if (!can_send_message_to_compositor()) {
        if (callback)
            callback(ScreenshotResult::Failed);
        return;
    }

    // The compositor paints straight into the target, so it has to be memory both processes can map.
    VERIFY(target_bitmap->anonymous_buffer().is_valid());
    auto shareable_bitmap = Gfx::ShareableBitmap { target_bitmap, Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap };
    auto request_id = Compositing::ScreenshotRequestId { m_next_screenshot_request_id++ };
    m_screenshots.set(request_id, PendingScreenshot { move(target_bitmap), move(callback) });
    // The screenshot is of what the compositor composes, so it goes after the frames handed to the Paint thread before.
    paint_stage_request_screenshot(new ScreenshotRequestForPaintThread { context_id, request_id, move(shareable_bitmap) });
}

void CompositorConnection::key_event(u64 page_id, Web::KeyEvent event)
{
    if (on_key_event)
        on_key_event(Web::PageId { page_id }, move(event));
}

void CompositorConnection::pinch_event(u64 page_id, Web::PinchEvent event)
{
    if (on_pinch_event)
        on_pinch_event(Web::PageId { page_id }, event);
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

void CompositorConnection::animated_content_moved_under_pointer(Web::CompositorContextId context_id, i64 frame_time_nanoseconds)
{
    for (auto& navigable : Web::HTML::all_local_navigables()) {
        if (!navigable->has_compositor_context() || navigable->compositor_context().id() != context_id)
            continue;
        navigable->event_handler().note_animated_content_moved_under_pointer(frame_time_nanoseconds);
        return;
    }
}

void CompositorConnection::overlay_scrollbar_visibilities_changed(Vector<Web::OverlayScrollbarVisibility> visibilities)
{
    // Each scroller belongs to the active document of one of our navigables, which takes its scrollbar's visibility.
    for (auto const& visibility : visibilities) {
        for (auto& navigable : Web::HTML::all_local_navigables()) {
            if (navigable->set_overlay_scrollbar_hidden(visibility.scroller_stable_node_id, visibility.vertical, visibility.hidden))
                break;
        }
    }
}

void CompositorConnection::placeholder_canvas_committed(Compositing::CanvasId canvas_id, Gfx::IntSize size, bool origin_clean)
{
    Web::HTML::HTMLCanvasElement::placeholder_frame_committed(canvas_id, size, origin_clean);
}

void CompositorConnection::prepare_remote_descendants_for_screenshot(Web::CompositorContextId context_id, Function<void()>&& callback)
{
    if (!can_send_message_to_compositor()) {
        callback();
        return;
    }
    auto request_id = m_next_remote_descendant_preparation_id++;
    m_remote_descendant_preparations.set(request_id, move(callback));
    async_prepare_remote_descendants_for_screenshot(context_id, request_id);
}

void CompositorConnection::remote_descendants_prepared_for_screenshot(u64 request_id)
{
    if (auto callback = m_remote_descendant_preparations.take(request_id); callback.has_value())
        (*callback)();
}

void CompositorConnection::paint_contexts_for_screenshot(u64 preparation_id, Vector<Web::CompositorContextId> context_ids)
{
    // The compositor names a context before those below it, and each is painted after them, as a rendering update would.
    GC::RootVector<GC::Ref<Web::HTML::LocalNavigable>> navigables;
    for (auto context_id : context_ids.in_reverse()) {
        for (auto& navigable : Web::HTML::all_local_navigables()) {
            if (navigable->has_been_destroyed() || !navigable->has_compositor_context() || navigable->compositor_context().id() != context_id)
                continue;
            navigables.append(navigable);
            break;
        }
    }
    GC::RootVector<GC::Ref<Web::Page>> prepared_pages;
    for (auto& navigable : navigables) {
        if (any_of(prepared_pages, [&](auto const& page) { return page.ptr() == &navigable->page(); }))
            continue;
        prepared_pages.append(navigable->page());
        navigable->page().prepare_canvas_contexts_for_compositing();
    }
    // A document that has just loaded may not have painted since, and the screenshot would show the frame it painted
    // before, so paint each one.
    for (auto& navigable : navigables)
        navigable->paint_next_frame_if_needed(Web::DOM::UpdateLayoutReason::ProcessScreenshot);
    // The frames reach the compositor on this connection once the Paint thread has handed them over, ahead of the
    // answer.
    Web::HTML::main_thread_event_loop().take_committed_frames_in();
    async_did_paint_contexts_for_screenshot(preparation_id);
}

void CompositorConnection::did_complete_screenshot(Compositing::ScreenshotRequestId request_id)
{
    auto pending_screenshot = take_screenshot(request_id);
    if (!pending_screenshot.has_value())
        return;

    if (pending_screenshot->callback)
        pending_screenshot->callback(ScreenshotResult::Taken);
}

void CompositorConnection::did_fail_screenshot(Compositing::ScreenshotRequestId request_id)
{
    auto pending_screenshot = take_screenshot(request_id);
    if (!pending_screenshot.has_value())
        return;

    if (pending_screenshot->callback)
        pending_screenshot->callback(ScreenshotResult::Failed);
}

void CompositorConnection::did_lose_compositor()
{
    if (m_has_lost_compositor)
        return;
    m_has_lost_compositor = true;
    stop_registering_resources_with_compositor(*m_poster);
    m_poster->detach_from_transport();

    for (auto& entry : m_screenshots) {
        if (entry.value.callback)
            entry.value.callback(ScreenshotResult::Failed);
    }
    m_screenshots.clear();

    // Nothing will answer the preparations now, and a screenshot that waits for one goes on without a compositor.
    for (auto& entry : exchange(m_remote_descendant_preparations, {}))
        entry.value();

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

extern "C" WEB_API bool web_compositor_post_fonts_and_image_frames(void* poster, u64 context_id, void const* const* fonts, size_t font_count, void const* const* image_frames, size_t image_frame_count);
extern "C" WEB_API bool web_compositor_create_shared_buffer(size_t size, Web::Compositor::FfiSharedBuffer* out);
extern "C" WEB_API void web_compositor_shared_buffer_release(void* owner);
extern "C" WEB_API int web_compositor_shared_buffer_clone_descriptor(void* owner);
extern "C" WEB_API Gfx::DecodedImageFrame* web_compositor_shared_buffer_image_frame(void* owner, i32 width, i32 height, u8 alpha_type, u8 color_space);
extern "C" WEB_API u64 web_compositor_connection_serial(Web::Compositor::CompositorConnection const*);
extern "C" WEB_API bool web_compositor_connection_for_sync_requests(Web::Compositor::CompositorConnection*, IPC::FfiConnection* out_connection);
extern "C" WEB_API bool web_compositor_connection_register_image_frames(Web::Compositor::CompositorConnection*, void const* const* image_frames, size_t image_frame_count);
extern "C" WEB_API bool web_compositor_post_screenshot_request(void* poster, void* request);
extern "C" WEB_API void web_screenshot_request_destroy(void* request);
extern "C" WEB_API bool web_compositor_stage_rasterization(void* poster, u64 token, void const* const* fonts, size_t font_count, void const* const* image_frames, size_t image_frame_count, void const* target_bitmap);

extern "C" WEB_API bool web_compositor_post_fonts_and_image_frames(void* poster, u64 context_id, void const* const* fonts, size_t font_count, void const* const* image_frames, size_t image_frame_count)
{
    return Web::Compositor::post_fonts_and_register_image_frames(
        *static_cast<IPC::ThreadSafeMessagePoster*>(poster),
        Web::CompositorContextId { context_id },
        { reinterpret_cast<Gfx::Font const* const*>(fonts), font_count },
        { reinterpret_cast<Gfx::DecodedImageFrame const* const*>(image_frames), image_frame_count });
}

extern "C" WEB_API bool web_compositor_create_shared_buffer(size_t size, Web::Compositor::FfiSharedBuffer* out)
{
    // Sealed where the platform supports it, so the receiver cannot be faulted by the file shrinking.
    auto buffer = Core::AnonymousBuffer::create_with_size(size);
    if (buffer.is_error())
        return false;
    auto descriptor = IPC::File::clone_fd(buffer.value().fd());
    if (descriptor.is_error())
        return false;
    auto owner = make<Web::Compositor::SharedBufferForRust>(buffer.release_value());
    *out = {
        .data = owner->buffer.data<u8>(),
        .size = owner->buffer.size(),
        .descriptor = descriptor.value().take_fd(),
        .owner = owner.leak_ptr(),
    };
    return true;
}

extern "C" WEB_API void web_compositor_shared_buffer_release(void* owner)
{
    delete static_cast<Web::Compositor::SharedBufferForRust*>(owner);
}

extern "C" WEB_API int web_compositor_shared_buffer_clone_descriptor(void* owner)
{
    auto descriptor = IPC::File::clone_fd(static_cast<Web::Compositor::SharedBufferForRust*>(owner)->buffer.fd());
    if (descriptor.is_error())
        return -1;
    return descriptor.value().take_fd();
}

// NB: A color space is the underlying value of a Bindings::PredefinedColorSpace.
extern "C" WEB_API Gfx::DecodedImageFrame* web_compositor_shared_buffer_image_frame(void* owner, i32 width, i32 height, u8 alpha_type, u8 color_space)
{
    VERIFY(Gfx::is_valid_alpha_type(alpha_type));
    auto& buffer = static_cast<Web::Compositor::SharedBufferForRust*>(owner)->buffer;
    auto format = static_cast<Gfx::AlphaType>(alpha_type) == Gfx::AlphaType::Unpremultiplied ? Gfx::BitmapFormat::RGBA8888 : Gfx::BitmapFormat::BGRA8888;
    auto gfx_color_space = Compositing::gfx_color_space_for(Web::HTML::predefined_color_space_from_underlying(color_space));
    auto frame = Gfx::DecodedImageFrame::create_over_shared_memory(buffer, 0, { width, height }, format, static_cast<Gfx::AlphaType>(alpha_type), move(gfx_color_space));
    if (frame.is_error())
        return nullptr;
    return new Gfx::DecodedImageFrame { frame.release_value() };
}

extern "C" WEB_API u64 web_compositor_connection_serial(Web::Compositor::CompositorConnection const* connection)
{
    return connection->serial();
}

extern "C" WEB_API bool web_compositor_connection_for_sync_requests(Web::Compositor::CompositorConnection* connection, IPC::FfiConnection* out_connection)
{
    if (!connection->can_send_message_to_compositor())
        return false;
    *out_connection = connection->to_ffi_with_one_reference();
    return true;
}

extern "C" WEB_API bool web_compositor_connection_register_image_frames(Web::Compositor::CompositorConnection* connection, void const* const* image_frames, size_t image_frame_count)
{
    return connection->register_image_frames({ reinterpret_cast<Gfx::DecodedImageFrame const* const*>(image_frames), image_frame_count });
}

extern "C" WEB_API bool web_compositor_post_screenshot_request(void* poster, void* request_pointer)
{
    auto request = adopt_own(*static_cast<Web::Compositor::ScreenshotRequestForPaintThread*>(request_pointer));
    auto encoded_message = MUST(Messages::CompositorWebContentServer::RequestScreenshot::static_encode(request->context_id, request->request_id, request->target));
    return static_cast<IPC::ThreadSafeMessagePoster*>(poster)->try_post(encoded_message);
}

extern "C" WEB_API void web_screenshot_request_destroy(void* request)
{
    delete static_cast<Web::Compositor::ScreenshotRequestForPaintThread*>(request);
}

extern "C" WEB_API bool web_compositor_stage_rasterization(void* poster, u64 token, void const* const* fonts, size_t font_count, void const* const* image_frames, size_t image_frame_count, void const* target_bitmap)
{
    auto& message_poster = *static_cast<IPC::ThreadSafeMessagePoster*>(poster);
    ReadonlySpan<Gfx::Font const*> staged_fonts { reinterpret_cast<Gfx::Font const* const*>(fonts), font_count };
    if (!Web::Compositor::register_typefaces_of_fonts_with_compositor(message_poster, staged_fonts))
        return false;
    ReadonlySpan<Gfx::DecodedImageFrame const*> staged_image_frames { reinterpret_cast<Gfx::DecodedImageFrame const* const*>(image_frames), image_frame_count };
    if (!Web::Compositor::register_image_frames_with_compositor(message_poster, staged_image_frames))
        return false;
    Vector<Compositing::ImageFrameResourceId> staged_image_frame_ids;
    staged_image_frame_ids.ensure_capacity(staged_image_frames.size());
    for (auto const* image_frame : staged_image_frames)
        staged_image_frame_ids.unchecked_append(Compositing::ImageFrameResourceId { image_frame->id() });
    auto& target = const_cast<Gfx::Bitmap&>(*static_cast<Gfx::Bitmap const*>(target_bitmap));
    auto encoded_message = MUST(Messages::CompositorWebContentServer::StageRasterization::static_encode(token, Web::Compositor::descriptions_of_fonts(staged_fonts), staged_image_frame_ids, Gfx::ShareableBitmap { target, Gfx::ShareableBitmap::ConstructWithKnownGoodBitmap }));
    return message_poster.try_post(encoded_message);
}
