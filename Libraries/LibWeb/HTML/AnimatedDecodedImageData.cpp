/*
 * Copyright (c) 2026, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Debug.h>
#include <AK/StdLibExtras.h>
#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/AnimatedDecodedImageData.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>
#include <LibWeb/Platform/ImageCodecPlugin.h>

namespace Web::HTML {

struct DecodedFrame {
    NonnullRefPtr<Gfx::DecodedImageFrame> frame;
};

static u32 allocate_animated_image_owner_id()
{
    static Atomic<u32> s_next_owner_id { 1 };
    u32 owner_id = s_next_owner_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
    VERIFY(owner_id != 0);
    return owner_id;
}

static DecodedFrame make_decoded_frame(NonnullRefPtr<Gfx::Bitmap> bitmap, Gfx::ColorSpace const& color_space, Optional<u64> stable_image_id)
{
    (void)stable_image_id;
    auto frame = Gfx::DecodedImageFrame::create(NonnullRefPtr<Gfx::Bitmap const> { *bitmap }, color_space);

    return {
        .frame = move(frame),
    };
}

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
    Vector<NonnullRefPtr<Gfx::Bitmap>> initial_bitmaps,
    bool use_gpu_backed_bitmap_resources)
{
    auto data = realm.create<AnimatedDecodedImageData>(
        session_id, frame_count, loop_count, size, move(color_space), move(durations), use_gpu_backed_bitmap_resources);

    // Place initial bitmaps into the buffer pool.
    for (u32 i = 0; i < initial_bitmaps.size(); ++i) {
        auto decoded_frame = make_decoded_frame(*initial_bitmaps[i], data->m_color_space, data->stable_image_id_for_frame(i));

        auto& slot = data->m_buffer_slots[i % BUFFER_POOL_SIZE];
        slot.frame_index = i;
        slot.frame = move(decoded_frame.frame);
        slot.generation = ++data->m_write_generation;
    }

    data->m_highest_requested_frame = initial_bitmaps.size();

    if (!initial_bitmaps.is_empty())
        data->m_last_displayed_frame = data->m_buffer_slots[0].frame;

    install_frame_delivery_callback();
    session_registry().set(session_id, data.ptr());
    data->maybe_request_more_frames(data->m_current_frame_index);

    return data;
}

AnimatedDecodedImageData::AnimatedDecodedImageData(
    i64 session_id,
    u32 frame_count,
    u32 loop_count,
    Gfx::IntSize size,
    Gfx::ColorSpace color_space,
    Vector<u32> durations,
    bool use_gpu_backed_bitmap_resources)
    : m_session_id(session_id)
    , m_image_owner_id(use_gpu_backed_bitmap_resources ? allocate_animated_image_owner_id() : 0)
    , m_frame_count(frame_count)
    , m_loop_count(loop_count)
    , m_size(size)
    , m_color_space(move(color_space))
    , m_durations(move(durations))
    , m_use_gpu_backed_bitmap_resources(use_gpu_backed_bitmap_resources)
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
        if (slot.frame_index == frame_index && slot.frame)
            return &slot;
    }
    return nullptr;
}

Optional<u64> AnimatedDecodedImageData::stable_image_id_for_frame(u32 frame_index) const
{
    if (!m_use_gpu_backed_bitmap_resources || m_image_owner_id == 0)
        return {};
    return (static_cast<u64>(m_image_owner_id) << 32) | frame_index;
}

AnimatedDecodedImageData::BufferSlot& AnimatedDecodedImageData::evict_oldest_slot()
{
    BufferSlot* oldest = m_buffer_slots.data();
    for (auto& slot : m_buffer_slots) {
        if (slot.generation < oldest->generation)
            oldest = &slot;
    }
    return *oldest;
}

u32 AnimatedDecodedImageData::count_contiguous_frames_ahead(size_t current_frame_index) const
{
    if (m_frame_count <= 1)
        return 0;

    u32 frame_count = min(static_cast<u32>(m_frame_count - 1), BUFFER_POOL_SIZE - 1);
    u32 frames_ahead = 0;
    for (u32 offset = 1; offset <= frame_count; ++offset) {
        u32 future_index = (static_cast<u32>(current_frame_index) + offset) % m_frame_count;
        if (!find_slot(future_index))
            break;
        ++frames_ahead;
    }
    return frames_ahead;
}

RefPtr<Gfx::DecodedImageFrame> AnimatedDecodedImageData::frame(size_t frame_index, Gfx::IntSize) const
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

int AnimatedDecodedImageData::frame_duration(size_t frame_index) const
{
    if (frame_index >= m_durations.size())
        return 0;
    return static_cast<int>(m_durations[frame_index]);
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
    auto decoded_frame = frame(frame_index);
    if (!decoded_frame)
        return;
    context.display_list_recorder().draw_scaled_decoded_image_frame(dst_rect, clip_rect, *decoded_frame, scaling_mode);
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

        auto decoded_frame = make_decoded_frame(*bitmaps[i], m_color_space, stable_image_id_for_frame(frame_index));

        auto& slot = evict_oldest_slot();
        slot.frame_index = frame_index;
        slot.frame = move(decoded_frame.frame);
        slot.generation = ++m_write_generation;
    }

    maybe_request_more_frames(m_current_frame_index);
}

size_t AnimatedDecodedImageData::notify_frame_advanced(size_t caller_frame_index)
{
    // We own the frame progression. Only advance when a caller reports
    // the expected next frame (this deduplicates multiple callers per tick).
    size_t expected_next = (m_current_frame_index + 1) % m_frame_count;
    if (caller_frame_index != expected_next)
        return m_current_frame_index;

    maybe_request_more_frames(m_current_frame_index);

    if (!find_slot(expected_next))
        return m_current_frame_index;

    m_current_frame_index = expected_next;
    maybe_request_more_frames(m_current_frame_index);
    return m_current_frame_index;
}

void AnimatedDecodedImageData::maybe_request_more_frames(size_t current_frame_index)
{
    if (m_request_in_flight || m_frame_count <= 1)
        return;

    u32 target_frames_ahead = min(static_cast<u32>(m_frame_count - 1), BUFFER_POOL_SIZE - 1);
    if (target_frames_ahead == 0)
        return;

    u32 frames_ahead = count_contiguous_frames_ahead(current_frame_index);
    if (frames_ahead >= target_frames_ahead)
        return;

    u32 request_start = (static_cast<u32>(current_frame_index) + frames_ahead + 1) % m_frame_count;
    u32 request_count = min(REQUEST_BATCH_SIZE, target_frames_ahead - frames_ahead);

    m_request_in_flight = true;
    m_last_requested_start_frame = request_start;
    m_highest_requested_frame = max(m_highest_requested_frame, request_start + request_count);
    Platform::ImageCodecPlugin::the().request_animation_frames(m_session_id, request_start, request_count);
}

}
