/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Types.h>
#include <AK/Vector.h>
#include <AK/kmalloc.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

namespace Compositor {

// A multi-frame image that advances by the clock, whatever the page's main thread is doing. The frames come straight
// from the image decoder of the page's process, a few at a time.
class AnimatedImage {
public:
    AK_ALLOC_WITH_KMALLOC;

    struct Description {
        i64 session_id { 0 };
        Gfx::IntSize size;
        Gfx::ColorSpace color_space;
        Vector<u32> frame_durations_in_milliseconds;
        u32 loop_count { 0 };
    };

    struct FrameRequest {
        i64 session_id { 0 };
        u32 start_frame_index { 0 };
        u32 count { 0 };
    };

    explicit AnimatedImage(Description);

    i64 session_id() const { return m_session_id; }

    void start(MonotonicTime now);
    void stop();

    // Whether the image still has frames to move through.
    bool is_animating(MonotonicTime now) const;
    u32 frame_index_at(MonotonicTime now) const;
    u32 loops_completed_at(MonotonicTime now) const;
    MonotonicTime next_frame_time_after(MonotonicTime now) const;

    // Moves to the frame for the time, or as near it as the frames that have arrived allow. Returns whether the image
    // now shows another frame.
    bool advance_to(MonotonicTime now);
    Optional<Gfx::DecodedImageFrame> const& shown_frame() const { return m_shown_frame; }
    u32 shown_frame_index() const { return m_shown_frame_index; }

    // The frames to ask the decoder for next, if any. The caller must ask for them.
    Optional<FrameRequest> take_frame_request(MonotonicTime now);
    // Returns whether any of the frames is one the image did not have.
    bool receive_frames(Vector<NonnullRefPtr<Gfx::Bitmap>>);
    void decoding_failed();
    // The decoder this image's session was on is gone. The image keeps showing the frame it has.
    void lose_decoder();

private:
    static constexpr u32 BUFFER_POOL_SIZE = 8;
    static constexpr u32 REQUEST_BATCH_SIZE = 4;
    // NB: The decoders hand out what the file says, which can be 0 for each frame of a loop.
    static constexpr u32 MINIMUM_FRAME_DURATION_IN_MILLISECONDS = 10;

    struct BufferSlot {
        Optional<u32> frame_index;
        Optional<Gfx::DecodedImageFrame> frame;
        u64 generation { 0 };
    };

    struct TimelinePosition {
        u64 loops_completed { 0 };
        u32 frame_index { 0 };
        // When the frame ends, in milliseconds since the start of its loop.
        u64 frame_end_in_milliseconds { 0 };
        bool has_finished { false };
    };

    u32 frame_count() const { return m_frame_durations_in_milliseconds.size(); }
    TimelinePosition position_at(MonotonicTime now) const;
    BufferSlot const* find_slot(u32 frame_index) const;
    BufferSlot& evict_oldest_slot();
    u32 frames_behind(u32 frame_index, u32 target_frame_index) const;

    i64 m_session_id { 0 };
    Gfx::IntSize m_size;
    Gfx::ColorSpace m_color_space;
    Vector<u32> m_frame_durations_in_milliseconds;
    u64 m_loop_duration_in_milliseconds { 0 };
    u32 m_loop_count { 0 };

    Optional<MonotonicTime> m_start_time;

    Array<BufferSlot, BUFFER_POOL_SIZE> m_buffer_slots;
    u64 m_write_generation { 0 };
    bool m_request_in_flight { false };
    u32 m_requested_start_frame_index { 0 };

    // Until a frame arrives from the decoder, the display list's own frame 0 shows.
    Optional<Gfx::DecodedImageFrame> m_shown_frame;
    u32 m_shown_frame_index { 0 };
};

}
