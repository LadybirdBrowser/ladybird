/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/MediaCapture/EncodedVideoSource.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>

namespace Web::MediaCapture {

NonnullRefPtr<EncodedVideoSource> EncodedVideoSource::create(Function<void()> request_key_frame)
{
    return adopt_ref(*new EncodedVideoSource(move(request_key_frame)));
}

EncodedVideoSource::EncodedVideoSource(Function<void()> request_key_frame)
    : m_request_key_frame(move(request_key_frame))
{
}

void EncodedVideoSource::add_track(MediaStreamTrack& track)
{
    m_tracks.append(track);
}

void EncodedVideoSource::remove_track(MediaStreamTrack& track)
{
    m_tracks.remove_all_matching([&](auto const& existing) { return !existing || existing.ptr().ptr() == &track; });
}

bool EncodedVideoSource::has_tracks() const
{
    return any_of(m_tracks, [](auto const& track) { return static_cast<bool>(track); });
}

void EncodedVideoSource::deliver(EncodedVideoFrame const& frame)
{
    // A track may stop, and so leave, while it is given the frame.
    auto tracks = m_tracks;
    for (auto const& track : tracks) {
        if (track)
            track->deliver_video_frame(frame);
    }
}

void EncodedVideoSource::request_key_frame()
{
    if (m_request_key_frame)
        m_request_key_frame();
}

void EncodedVideoSource::set_key_frame_request_handler(Function<void()> request_key_frame)
{
    m_request_key_frame = move(request_key_frame);
}

}
