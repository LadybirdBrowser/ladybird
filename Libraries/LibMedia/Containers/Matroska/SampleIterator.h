/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <LibMedia/DecoderError.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>

#include "Document.h"

namespace Media::Matroska {

class Reader;

struct TrackCuePoint {
    AK::Duration timestamp;
    CueTrackPosition position;
};

enum class CuePointTarget : u8 {
    Cluster,
    Block,
};

class MEDIA_API SampleIterator {
    AK_MAKE_DEFAULT_MOVABLE(SampleIterator);
    AK_MAKE_DEFAULT_COPYABLE(SampleIterator);

public:
    ~SampleIterator();

    DecoderErrorOr<Block> next_block();
    DecoderErrorOr<Vector<ByteBuffer>> get_frames(Block);
    Cluster const& current_cluster() const { return *m_current_cluster; }
    Optional<AK::Duration> const& last_timestamp() const { return m_last_timestamp; }
    MediaStreamCursor& cursor() { return m_stream_cursor; }
    size_t position() const { return m_position; }

private:
    friend class Reader;

    SampleIterator(NonnullRefPtr<MediaStreamCursor> const& stream_cursor, Optional<u64> track_number, TrackBlockContexts&&, u64 timestamp_scale, size_t segment_contents_position, size_t position);

    DecoderErrorOr<void> seek_to_cue_point(TrackCuePoint const& cue_point, CuePointTarget);

    NonnullRefPtr<MediaStreamCursor> m_stream_cursor;
    Optional<u64> m_track_number;
    TrackBlockContexts m_track_block_contexts;
    u64 m_segment_timestamp_scale { 0 };
    size_t m_segment_contents_position { 0 };

    // Must always point to an element ID or the end of the stream.
    size_t m_position { 0 };

    Optional<AK::Duration> m_last_timestamp;

    Optional<Cluster> m_current_cluster;
};

}
