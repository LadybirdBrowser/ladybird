/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NeverDestroyed.h>
#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/DOM/DocumentObserver.h>
#include <LibWeb/HTML/AnimatedBitmapDecodedImageData.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(AnimatedBitmapDecodedImageData);

HashMap<i64, GC::RawPtr<AnimatedBitmapDecodedImageData>>& AnimatedBitmapDecodedImageData::session_registry()
{
    static NeverDestroyed<HashMap<i64, GC::RawPtr<AnimatedBitmapDecodedImageData>>> registry;
    return *registry;
}

void AnimatedBitmapDecodedImageData::install_frame_delivery_callback()
{
    static bool s_installed = false;
    if (s_installed)
        return;
    s_installed = true;

    Platform::ImageCodecPlugin::the().on_animation_frames_decoded = [](i64 session_id, Vector<NonnullRefPtr<Gfx::Bitmap>> bitmaps) {
        deliver_frames_for_session(session_id, move(bitmaps));
    };
    Platform::ImageCodecPlugin::the().on_animation_decode_failed = [](i64 session_id) {
        auto it = session_registry().find(session_id);
        if (it != session_registry().end()) {
            if (auto data = it->value)
                data->m_request_in_flight = false;
        }
    };
}

void AnimatedBitmapDecodedImageData::deliver_frames_for_session(i64 session_id, Vector<NonnullRefPtr<Gfx::Bitmap>> bitmaps)
{
    auto it = session_registry().find(session_id);
    if (it == session_registry().end())
        return;
    if (auto data = it->value)
        data->receive_frames(move(bitmaps), data->m_last_requested_start_frame);
}

GC::Ref<AnimatedBitmapDecodedImageData> AnimatedBitmapDecodedImageData::create(
    JS::Realm& realm,
    DOM::Document& document,
    i64 session_id,
    u32 frame_count,
    u32 loop_count,
    Gfx::IntSize size,
    Gfx::ColorSpace color_space,
    Vector<u32> durations,
    Vector<NonnullRefPtr<Gfx::Bitmap>> initial_bitmaps)
{
    auto animation_timer = Platform::Timer::create(realm.heap());
    auto document_observer = realm.create<DOM::DocumentObserver>(realm, document);

    auto data = realm.create<AnimatedBitmapDecodedImageData>(
        session_id, frame_count, loop_count, size, move(color_space), move(durations), animation_timer, document_observer);

    // Place initial bitmaps into the buffer pool.
    for (u32 i = 0; i < initial_bitmaps.size(); ++i) {
        auto& slot = data->m_buffer_slots[i % BUFFER_POOL_SIZE];
        slot.frame_index = i;
        slot.frame = Gfx::DecodedImageFrame { *initial_bitmaps[i], data->m_color_space };
        slot.generation = ++data->m_write_generation;
    }

    if (!initial_bitmaps.is_empty())
        data->m_last_displayed_frame = data->m_buffer_slots[0].frame;

    install_frame_delivery_callback();
    session_registry().set(session_id, data.ptr());

    return data;
}

AnimatedBitmapDecodedImageData::AnimatedBitmapDecodedImageData(
    i64 session_id,
    u32 frame_count,
    u32 loop_count,
    Gfx::IntSize size,
    Gfx::ColorSpace color_space,
    Vector<u32> durations,
    GC::Ref<Platform::Timer> animation_timer,
    GC::Ref<DOM::DocumentObserver> document_observer)
    : AnimatedDecodedImageData(document_observer)
    , m_session_id(session_id)
    , m_frame_count(frame_count)
    , m_loop_count(loop_count)
    , m_size(size)
    , m_color_space(move(color_space))
    , m_durations(move(durations))
    , m_animation_timer(animation_timer)
{
    m_animation_timer->on_timeout = GC::create_function(vm().heap(), [weak_this = GC::Weak { *this }] {
        if (auto self = weak_this.ptr())
            self->advance_animation();
    });
}

AnimatedBitmapDecodedImageData::~AnimatedBitmapDecodedImageData() = default;

void AnimatedBitmapDecodedImageData::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_animation_timer);
}

size_t AnimatedBitmapDecodedImageData::external_memory_size() const
{
    size_t size = JS::vector_external_memory_size(m_durations);
    for (auto const& slot : m_buffer_slots) {
        if (slot.frame.has_value())
            size = JS::saturating_add_external_memory_size(size, slot.frame->bitmap().data_size());
    }
    return size;
}

void AnimatedBitmapDecodedImageData::finalize()
{
    Base::finalize();
    m_animation_timer->stop();
    session_registry().remove(m_session_id);
    Platform::ImageCodecPlugin::the().stop_animation_decode(m_session_id);
}

bool AnimatedBitmapDecodedImageData::animation_has_completed() const
{
    return m_loop_count > 0 && m_loops_completed == m_loop_count;
}

void AnimatedBitmapDecodedImageData::reset_animation()
{
    m_current_frame_index = 0;
    m_loops_completed = 0;
    maybe_request_more_frames(m_current_frame_index);
    notify_clients_did_update();
}

void AnimatedBitmapDecodedImageData::start_animation()
{
    // NB: We should only ever start the animation when the first client is registered, or when animation restarts, both
    //     of which should guarantee that we are at the beginning of the animation.
    VERIFY(m_current_frame_index == 0 && m_loops_completed == 0);

    m_animation_timer->start(frame_duration(0));
}

void AnimatedBitmapDecodedImageData::stop_animation()
{
    m_animation_timer->stop();
}

void AnimatedBitmapDecodedImageData::advance_animation()
{
    m_current_frame_index = (m_current_frame_index + 1) % m_frame_count;
    maybe_request_more_frames(m_current_frame_index);

    auto current_frame_duration = frame_duration(m_current_frame_index);
    if (current_frame_duration != m_animation_timer->interval())
        m_animation_timer->restart(current_frame_duration);

    if (m_current_frame_index == m_frame_count - 1) {
        ++m_loops_completed;
        if (animation_has_completed())
            stop_animation();
    }

    notify_clients_did_update();
}

AnimatedBitmapDecodedImageData::BufferSlot const* AnimatedBitmapDecodedImageData::find_slot(u32 frame_index) const
{
    for (auto const& slot : m_buffer_slots) {
        if (slot.frame_index == frame_index && slot.frame.has_value())
            return &slot;
    }
    return nullptr;
}

AnimatedBitmapDecodedImageData::BufferSlot& AnimatedBitmapDecodedImageData::evict_oldest_slot()
{
    BufferSlot* oldest = &m_buffer_slots[0];
    for (auto& slot : m_buffer_slots) {
        if (slot.generation < oldest->generation)
            oldest = &slot;
    }
    return *oldest;
}

Optional<Gfx::DecodedImageFrame> AnimatedBitmapDecodedImageData::frame(size_t frame_index, Gfx::IntSize) const
{
    if (frame_index >= m_frame_count)
        return m_last_displayed_frame;

    if (auto const* slot = find_slot(frame_index)) {
        m_last_displayed_frame = slot->frame;
        return slot->frame;
    }

    // Frame not in pool; return last displayed frame as fallback.
    return m_last_displayed_frame;
}

Optional<Gfx::DecodedImageFrame> AnimatedBitmapDecodedImageData::default_frame(Gfx::IntSize size) const
{
    // FIXME: This should account for if the file specifies a default frame other than the first frame e.g. the
    //        "static image" in APNG or the "primary image item" in AVIF.
    // FIXME: This should bypass the "m_last_displayed_frame" fallback logic and always return the correct default frame
    return frame(0, size);
}

Optional<Gfx::DecodedImageFrame> AnimatedBitmapDecodedImageData::current_frame(Gfx::IntSize size) const
{
    return frame(m_current_frame_index, size);
}

int AnimatedBitmapDecodedImageData::frame_duration(size_t frame_index) const
{
    if (frame_index >= m_durations.size())
        return 0;
    return m_durations[frame_index];
}

Optional<CSSPixels> AnimatedBitmapDecodedImageData::intrinsic_width() const
{
    return m_size.width();
}

Optional<CSSPixels> AnimatedBitmapDecodedImageData::intrinsic_height() const
{
    return m_size.height();
}

Optional<CSSPixelFraction> AnimatedBitmapDecodedImageData::intrinsic_aspect_ratio() const
{
    return CSSPixels(m_size.width()) / CSSPixels(m_size.height());
}

void AnimatedBitmapDecodedImageData::paint(DisplayListRecordingContext& context, Gfx::IntRect dst_rect, CSS::ImageRendering image_rendering) const
{
    auto decoded_frame = current_frame();
    if (!decoded_frame.has_value())
        return;

    auto scaling_mode = CSS::to_gfx_scaling_mode(image_rendering, m_size, dst_rect.size());

    context.display_list_recorder().draw_scaled_decoded_image_frame(dst_rect, *decoded_frame, scaling_mode);
}

void AnimatedBitmapDecodedImageData::receive_frames(Vector<NonnullRefPtr<Gfx::Bitmap>> bitmaps, u32 start_frame_index)
{
    m_request_in_flight = false;

    for (u32 i = 0; i < bitmaps.size(); ++i) {
        u32 frame_index = start_frame_index + i;
        if (frame_index >= m_frame_count)
            break;

        // Check if this frame is already in the pool.
        if (find_slot(frame_index))
            continue;

        auto& slot = evict_oldest_slot();
        slot.frame_index = frame_index;
        slot.frame = Gfx::DecodedImageFrame { *bitmaps[i], m_color_space };
        slot.generation = ++m_write_generation;
    }
}

void AnimatedBitmapDecodedImageData::maybe_request_more_frames(size_t current_frame_index)
{
    if (m_request_in_flight)
        return;

    // TODO: Once all `ImageProvider`s are `DecodedImageData::Client`s we can defer loading new frames if we have no
    //       clients

    // Count how many frames ahead of current are in the pool.
    u32 frames_ahead = 0;
    for (u32 offset = 1; offset <= BUFFER_POOL_SIZE; ++offset) {
        u32 future_index = (current_frame_index + offset) % m_frame_count;
        if (find_slot(future_index))
            ++frames_ahead;
        else
            break;
    }

    // Request more when buffer is less than half full, giving the decoder
    // time to respond while we still have frames to display.
    if (frames_ahead >= REQUEST_BATCH_SIZE)
        return;

    // Determine which frame to request from.
    u32 request_start = (current_frame_index + frames_ahead + 1) % m_frame_count;
    u32 request_count = REQUEST_BATCH_SIZE;

    m_request_in_flight = true;
    m_last_requested_start_frame = request_start;
    Platform::ImageCodecPlugin::the().request_animation_frames(m_session_id, request_start, request_count);
}

}
