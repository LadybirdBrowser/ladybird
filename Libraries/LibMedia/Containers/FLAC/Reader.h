/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/FixedArray.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/Export.h>
#include <LibMedia/MediaStream.h>
#include <LibMediaCodecs/DecoderError.h>

namespace Media::FLAC {

struct StreamInfo {
    u16 minimum_block_size { 0 };
    u16 maximum_block_size { 0 };
    u32 minimum_frame_size { 0 };
    u32 maximum_frame_size { 0 };
    u32 sample_rate { 0 };
    u8 channel_count { 0 };
    u8 bits_per_sample { 0 };
    // Zero when the stream does not say.
    u64 total_sample_count { 0 };
};

struct SeekPoint {
    u64 sample_number { 0 };
    // From the first frame.
    u64 byte_offset { 0 };
};

// What telling the frames of a stream apart takes.
struct FrameSync {
    u16 sync_code { 0 };
    // Zero for a stream whose frames vary in size and number their first sample instead.
    u16 fixed_block_size { 0 };
};

struct FrameHeader {
    size_t position { 0 };
    size_t byte_size { 0 };
    u64 sample_number { 0 };
    u16 block_size { 0 };
};

// Frames carry no size, so a frame ends where the next one begins, which the CRC every frame ends with confirms.
class MEDIA_API FrameIterator {
public:
    struct Frame {
        FrameHeader header;
        FixedArray<u8> data;

        size_t end_position() const { return header.position + data.size(); }
    };

    // Whether to look for the next frame upon finding none at the position.
    enum class Resynchronize : bool {
        No,
        Yes,
    };

    FrameIterator(NonnullRefPtr<MediaStreamCursor> cursor, FrameSync sync, size_t position, Resynchronize resynchronize)
        : m_cursor(move(cursor))
        , m_sync(sync)
        , m_position(position)
        , m_resynchronize(resynchronize)
    {
    }

    // Moves to the first header at or after the position, which an estimated position has to be brought to before
    // frames are read from it. False when the stream ends, or the bound is reached, first.
    DecoderErrorOr<bool> resync_forward(size_t bound = NumericLimits<size_t>::max());
    // Reads the frame at the position and moves past it. Nothing moves until the whole frame has been read.
    DecoderErrorOr<Frame> read_next_frame();

    // The last frame heading before the bound, without moving.
    Optional<FrameHeader> find_last_header_before(size_t bound, size_t lower_bound);

    size_t position() const { return m_position; }
    void set_position(size_t position) { m_position = position; }
    MediaStreamCursor& cursor() { return *m_cursor; }

private:
    DecoderErrorOr<Optional<FrameHeader>> parse_header_at(size_t position);
    DecoderErrorOr<Optional<FrameHeader>> find_header_at_or_after(size_t position, size_t bound);
    DecoderErrorOr<Optional<FrameHeader>> find_header_before(size_t bound, size_t lower_bound);
    DecoderErrorOr<bool> frame_follows(FrameHeader const& header, FrameHeader const& candidate);
    DecoderErrorOr<Optional<Frame>> read_frame_at(FrameHeader const&);

    NonnullRefPtr<MediaStreamCursor> m_cursor;
    FrameSync m_sync;
    size_t m_position { 0 };
    Resynchronize m_resynchronize { Resynchronize::No };
};

// Reads the metadata blocks ahead of a stream's frames.
class MEDIA_API Reader {
public:
    static DecoderErrorOr<Reader> from_stream(NonnullRefPtr<MediaStreamCursor> const&);

    StreamInfo const& stream_info() const { return m_stream_info; }
    FrameSync const& frame_sync() const { return m_frame_sync; }
    size_t first_frame_position() const { return m_first_frame_position; }
    Vector<SeekPoint> const& seek_points() const { return m_seek_points; }
    // The stream's marker and STREAMINFO block, which is what a decoder is configured with.
    ReadonlyBytes codec_initialization_data() const { return m_codec_initialization_data; }
    // Zero when the stream does not say.
    AK::Duration duration() const;

private:
    Reader(StreamInfo, FrameSync, size_t first_frame_position, Vector<SeekPoint>, ByteBuffer codec_initialization_data);

    StreamInfo m_stream_info;
    FrameSync m_frame_sync;
    size_t m_first_frame_position { 0 };
    Vector<SeekPoint> m_seek_points;
    ByteBuffer m_codec_initialization_data;
};

}
