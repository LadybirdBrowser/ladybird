/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Format.h>
#include <Compositor/AnimatedImage.h>
#include <LibGfx/Bitmap.h>

namespace Compositor {

AnimatedImage::AnimatedImage(Description description)
    : m_session_id(description.session_id)
    , m_size(description.size)
    , m_color_space(move(description.color_space))
    , m_frame_durations_in_milliseconds(move(description.frame_durations_in_milliseconds))
    , m_loop_count(description.loop_count)
{
    VERIFY(!m_frame_durations_in_milliseconds.is_empty());
    for (auto& duration : m_frame_durations_in_milliseconds) {
        duration = max(duration, MINIMUM_FRAME_DURATION_IN_MILLISECONDS);
        m_loop_duration_in_milliseconds += duration;
    }
}

void AnimatedImage::start(MonotonicTime now)
{
    // NB: With no decoder to ask for frames, the image stays on the frame it has.
    if (m_session_id == 0)
        return;
    m_start_time = now;
}

void AnimatedImage::stop()
{
    m_start_time.clear();
}

AnimatedImage::TimelinePosition AnimatedImage::position_at(MonotonicTime now) const
{
    VERIFY(m_start_time.has_value());
    auto elapsed_in_milliseconds = static_cast<u64>(max<i64>((now - *m_start_time).to_milliseconds(), 0));

    auto loops_completed = elapsed_in_milliseconds / m_loop_duration_in_milliseconds;
    if (m_loop_count > 0 && loops_completed >= m_loop_count)
        return { .loops_completed = m_loop_count, .frame_index = frame_count() - 1, .frame_end_in_milliseconds = m_loop_duration_in_milliseconds, .has_finished = true };

    auto time_in_loop = elapsed_in_milliseconds % m_loop_duration_in_milliseconds;
    u64 frame_end = 0;
    for (u32 frame_index = 0; frame_index < frame_count(); ++frame_index) {
        frame_end += m_frame_durations_in_milliseconds[frame_index];
        if (time_in_loop < frame_end)
            return { .loops_completed = loops_completed, .frame_index = frame_index, .frame_end_in_milliseconds = frame_end };
    }
    VERIFY_NOT_REACHED();
}

bool AnimatedImage::is_animating(MonotonicTime now) const
{
    return m_start_time.has_value() && !position_at(now).has_finished;
}

u32 AnimatedImage::frame_index_at(MonotonicTime now) const
{
    if (!m_start_time.has_value())
        return m_shown_frame_index;
    return position_at(now).frame_index;
}

u32 AnimatedImage::loops_completed_at(MonotonicTime now) const
{
    if (!m_start_time.has_value())
        return 0;
    return position_at(now).loops_completed;
}

MonotonicTime AnimatedImage::next_frame_time_after(MonotonicTime now) const
{
    auto position = position_at(now);
    auto elapsed_in_milliseconds = position.loops_completed * m_loop_duration_in_milliseconds + position.frame_end_in_milliseconds;
    return *m_start_time + AK::Duration::from_milliseconds(static_cast<i64>(elapsed_in_milliseconds));
}

// How far before the target frame a frame is, going round the loop.
u32 AnimatedImage::frames_behind(u32 frame_index, u32 target_frame_index) const
{
    return (target_frame_index + frame_count() - frame_index) % frame_count();
}

bool AnimatedImage::advance_to(MonotonicTime now)
{
    auto target_frame_index = frame_index_at(now);

    // A decoder that falls behind must not stop the image: take the newest frame that has arrived.
    BufferSlot const* best_slot = nullptr;
    auto fewest_frames_behind = frames_behind(m_shown_frame_index, target_frame_index);
    for (auto const& slot : m_buffer_slots) {
        if (!slot.frame_index.has_value())
            continue;
        auto slot_frames_behind = frames_behind(*slot.frame_index, target_frame_index);
        if (slot_frames_behind < fewest_frames_behind) {
            fewest_frames_behind = slot_frames_behind;
            best_slot = &slot;
        }
    }
    if (!best_slot)
        return false;

    m_shown_frame_index = *best_slot->frame_index;
    m_shown_frame = best_slot->frame;
    return true;
}

Optional<AnimatedImage::FrameRequest> AnimatedImage::take_frame_request(MonotonicTime now)
{
    if (m_request_in_flight || m_session_id == 0)
        return {};

    auto target_frame_index = frame_index_at(now);

    // Count how many frames ahead of the target are in the pool.
    u32 frames_ahead = 0;
    for (u32 offset = 1; offset <= BUFFER_POOL_SIZE; ++offset) {
        if (!find_slot((target_frame_index + offset) % frame_count()))
            break;
        ++frames_ahead;
    }

    // Ask for more when the pool is less than half full, which gives the decoder time to answer while frames remain.
    bool target_frame_is_missing = target_frame_index != m_shown_frame_index && !find_slot(target_frame_index);
    if (frames_ahead >= REQUEST_BATCH_SIZE && !target_frame_is_missing)
        return {};

    m_requested_start_frame_index = target_frame_is_missing ? target_frame_index : (target_frame_index + frames_ahead + 1) % frame_count();
    m_request_in_flight = true;
    return FrameRequest { .session_id = m_session_id, .start_frame_index = m_requested_start_frame_index, .count = REQUEST_BATCH_SIZE };
}

AnimatedImage::BufferSlot const* AnimatedImage::find_slot(u32 frame_index) const
{
    for (auto const& slot : m_buffer_slots) {
        if (slot.frame_index == frame_index)
            return &slot;
    }
    return nullptr;
}

AnimatedImage::BufferSlot& AnimatedImage::evict_oldest_slot()
{
    BufferSlot* oldest = &m_buffer_slots[0];
    for (auto& slot : m_buffer_slots) {
        if (slot.generation < oldest->generation)
            oldest = &slot;
    }
    return *oldest;
}

bool AnimatedImage::receive_frames(Vector<NonnullRefPtr<Gfx::Bitmap>> bitmaps)
{
    m_request_in_flight = false;

    bool received_new_frame = false;
    for (u32 i = 0; i < bitmaps.size(); ++i) {
        u32 frame_index = m_requested_start_frame_index + i;
        if (frame_index >= frame_count())
            break;

        // NB: The decoder parses untrusted data, so what it sends is checked rather than trusted.
        if (bitmaps[i]->size() != m_size) {
            dbgln("Compositor: Dropping an animated image frame of the wrong size");
            continue;
        }

        if (find_slot(frame_index))
            continue;

        auto frame = Gfx::DecodedImageFrame::create(*bitmaps[i], m_color_space);
        if (frame.is_error()) {
            dbgln("Compositor: Dropping an animated image frame: {}", frame.error());
            continue;
        }

        auto& slot = evict_oldest_slot();
        slot.frame_index = frame_index;
        slot.frame = frame.release_value();
        slot.generation = ++m_write_generation;
        received_new_frame = true;
    }
    return received_new_frame;
}

void AnimatedImage::decoding_failed()
{
    m_request_in_flight = false;
}

void AnimatedImage::lose_decoder()
{
    m_session_id = 0;
    m_request_in_flight = false;
    stop();
    for (auto& slot : m_buffer_slots)
        slot = {};
}

}
