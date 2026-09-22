/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibMedia/Containers/ADTS/FrameHeader.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaStream.h>

namespace Media::ADTS {

class MEDIA_API FrameIterator {
public:
    struct Frame {
        size_t position { 0 };
        FrameHeader header;
    };

    // Whether to scan forward for a sync code upon finding an invalid frame.
    enum class Resynchronize : bool {
        No,
        Yes,
    };

    FrameIterator(NonnullRefPtr<MediaStreamCursor> cursor, size_t position, Resynchronize resynchronize)
        : m_cursor(move(cursor))
        , m_position(position)
        , m_resynchronize(resynchronize)
    {
    }

    Optional<Frame> next();

    // The audio of the frame, without the framing that ADTS wraps it in. A decoder is configured
    // from the configuration record the header implies, so it is handed the same access units that
    // a stream carrying that record in its container would hand it.
    DecoderErrorOr<FixedArray<u8>> read_frame_data(Frame const&);

    size_t position() const { return m_position; }
    MediaStreamCursor& cursor() { return *m_cursor; }

private:
    Optional<Frame> read_frame();

    NonnullRefPtr<MediaStreamCursor> m_cursor;
    size_t m_position { 0 };
    Resynchronize m_resynchronize { Resynchronize::No };
};

class MEDIA_API Reader {
public:
    // Junk ahead of the audio is left over from a stream that was cut, so a bound only has to be far
    // enough out to cover that, and it keeps a stream that only claims to hold audio from being read
    // in full. FFmpeg and Firefox both settled on the same distance.
    static constexpr size_t MAXIMUM_SYNC_SEARCH_BYTE_COUNT = 64 * KiB;

    // Finds the position of a frame that begins a run of valid frames, so that a false sync code in
    // the middle of a stream is not mistaken for a frame boundary.
    static Optional<size_t> find_frame_boundary_at_or_after(NonnullRefPtr<MediaStreamCursor> const&, size_t start_byte, size_t upper_bound);
    static Optional<size_t> find_frame_boundary_at_or_before(NonnullRefPtr<MediaStreamCursor> const&, size_t target_byte, size_t lower_bound, size_t upper_bound);

    // Returns nothing until enough frames are available to tell audio apart from a false sync code.
    static Optional<Reader> from_stream(NonnullRefPtr<MediaStreamCursor> const&);

    size_t first_frame_position() const { return m_first_frame_position; }
    FrameHeader const& frame_header() const { return m_frame_header; }

    // ADTS states nothing about the length of the stream, so this is always extrapolated from the
    // bitrate of the frames at the front of it.
    AK::Duration duration() const { return m_duration; }

private:
    size_t m_first_frame_position { 0 };
    FrameHeader m_frame_header;
    AK::Duration m_duration { AK::Duration::zero() };
};

}
