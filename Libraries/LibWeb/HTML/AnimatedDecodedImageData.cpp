/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/AnimatedDecodedImageData.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(AnimatedDecodedImageData);

HashMap<i64, GC::RawPtr<AnimatedDecodedImageData>>& AnimatedDecodedImageData::session_registry()
{
    static HashMap<i64, GC::RawPtr<AnimatedDecodedImageData>> s_registry;
    return s_registry;
}

void AnimatedDecodedImageData::install_frame_delivery_callback()
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

void AnimatedDecodedImageData::deliver_frames_for_session(i64 session_id, Vector<NonnullRefPtr<Gfx::Bitmap>> bitmaps)
{
    auto it = session_registry().find(session_id);
    if (it == session_registry().end())
        return;
    if (auto data = it->value)
        data->receive_frames(move(bitmaps), data->m_last_requested_start_frame);
}

GC::Ref<AnimatedDecodedImageData> AnimatedDecodedImageData::create(
    JS::Realm& realm,
    i64 session_id,
    u32 frame_count,
    u32 loop_count,
    Gfx::IntSize size,
    Gfx::ColorSpace color_space,
    Vector<u32> durations,
    Vector<NonnullRefPtr<Gfx::Bitmap>> initial_bitmaps)
{
    auto data = realm.create<AnimatedDecodedImageData>(
        session_id, frame_count, loop_count, size, move(color_space), move(durations));

    // Place initial bitmaps into the buffer pool.
    for (u32 i = 0; i < initial_bitmaps.size(); ++i) {
        auto& slot = data->m_buffer_slots[i % BUFFER_POOL_SIZE];
        slot.frame_index = i;
        slot.bitmap = Gfx::ImmutableBitmap::create(*initial_bitmaps[i], data->m_color_space);
        slot.generation = ++data->m_write_generation;
    }

    data->m_highest_requested_frame = initial_bitmaps.size();

    if (!initial_bitmaps.is_empty())
        data->m_last_displayed_bitmap = data->m_buffer_slots[0].bitmap;

    install_frame_delivery_callback();
    session_registry().set(session_id, data.ptr());

    return data;
}

AnimatedDecodedImageData::AnimatedDecodedImageData(
    i64 session_id,
    u32 frame_count,
    u32 loop_count,
    Gfx::IntSize size,
    Gfx::ColorSpace color_space,
    Vector<u32> durations)
    : m_session_id(session_id)
    , m_frame_count(frame_count)
    , m_loop_count(loop_count)
    , m_size(size)
    , m_color_space(move(color_space))
    , m_durations(move(durations))
{
}

AnimatedDecodedImageData::~AnimatedDecodedImageData() = default;

void AnimatedDecodedImageData::finalize()
{
    Base::finalize();
    session_registry().remove(m_session_id);
    Platform::ImageCodecPlugin::the().stop_animation_decode(m_session_id);
}

AnimatedDecodedImageData::BufferSlot const* AnimatedDecodedImageData::find_slot(u32 frame_index) const
{
    for (auto const& slot : m_buffer_slots) {
        if (slot.frame_index == frame_index && slot.bitmap)
            return &slot;
    }
    return nullptr;
}

AnimatedDecodedImageData::BufferSlot& AnimatedDecodedImageData::evict_oldest_slot()
{
    BufferSlot* oldest = &m_buffer_slots[0];
    for (auto& slot : m_buffer_slots) {
        if (slot.generation < oldest->generation)
            oldest = &slot;
    }
    return *oldest;
}

RefPtr<Gfx::ImmutableBitmap> AnimatedDecodedImageData::bitmap(size_t frame_index, Gfx::IntSize) const
{
    if (frame_index >= m_frame_count)
        return m_last_displayed_bitmap;

    if (auto const* slot = find_slot(frame_index)) {
        m_last_displayed_bitmap = slot->bitmap;
        return slot->bitmap;
    }

    // Frame not in pool; return last displayed frame as fallback.
    return m_last_displayed_bitmap;
}

int AnimatedDecodedImageData::frame_duration(size_t frame_index) const
{
    if (frame_index >= m_durations.size())
        return 0;
    return m_durations[frame_index];
}

Optional<CSSPixels> AnimatedDecodedImageData::intrinsic_width() const
{
    return m_size.width();
}

Optional<CSSPixels> AnimatedDecodedImageData::intrinsic_height() const
{
    return m_size.height();
}

Optional<CSSPixelFraction> AnimatedDecodedImageData::intrinsic_aspect_ratio() const
{
    return CSSPixels(m_size.width()) / CSSPixels(m_size.height());
}

Optional<Gfx::IntRect> AnimatedDecodedImageData::frame_rect(size_t) const
{
    return Gfx::IntRect { {}, m_size };
}

void AnimatedDecodedImageData::paint(DisplayListRecordingContext& context, size_t frame_index, Gfx::IntRect dst_rect, Gfx::IntRect clip_rect, Gfx::ScalingMode scaling_mode) const
{
    auto immutable_bitmap = bitmap(frame_index);
    if (!immutable_bitmap)
        return;
    context.display_list_recorder().draw_scaled_immutable_bitmap(dst_rect, clip_rect, *immutable_bitmap, scaling_mode);
}

void AnimatedDecodedImageData::receive_frames(Vector<NonnullRefPtr<Gfx::Bitmap>> bitmaps, u32 start_frame_index)
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
        slot.bitmap = Gfx::ImmutableBitmap::create(*bitmaps[i], m_color_space);
        slot.generation = ++m_write_generation;
    }
}

size_t AnimatedDecodedImageData::notify_frame_advanced(size_t caller_frame_index)
{
    // We own the frame progression. Only advance when a caller reports
    // the expected next frame (this deduplicates multiple callers per tick).
    size_t expected_next = (m_current_frame_index + 1) % m_frame_count;
    if (caller_frame_index == expected_next) {
        m_current_frame_index = expected_next;
        maybe_request_more_frames(m_current_frame_index);
    }
    return m_current_frame_index;
}

void AnimatedDecodedImageData::maybe_request_more_frames(size_t current_frame_index)
{
    if (m_request_in_flight)
        return;

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
    m_highest_requested_frame = max(m_highest_requested_frame, request_start + request_count);
    Platform::ImageCodecPlugin::the().request_animation_frames(m_session_id, request_start, request_count);
}

}
