/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/MediaSourceExtensions/SourceBufferDemuxer.h>
#include <LibMedia/MediaSourceExtensions/TrackBuffer.h>

namespace Media::MediaSourceExtensions {

TrackBuffer::TrackBuffer(NonnullRefPtr<SourceBufferDemuxer> demuxer, Media::Track const& track)
    : m_demuxer(move(demuxer))
    , m_track(track)
{
}

TrackBuffer::~TrackBuffer() = default;

// https://w3c.github.io/media-source/#track-buffer-ranges
void TrackBuffer::track_buffer_ranges() const
{
    // FIXME: Return the presentation time ranges occupied by the coded frames currently stored in the track buffer.
}

}
