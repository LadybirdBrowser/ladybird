/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/MediaCapture/VideoPictureSource.h>

namespace Web::MediaCapture {

VideoPictureSource::~VideoPictureSource() = default;

void VideoPictureSource::add_track(MediaStreamTrack& track)
{
    m_tracks.append(track);
    if (m_tracks.size() == 1)
        did_gain_first_track();
}

void VideoPictureSource::remove_track(MediaStreamTrack& track)
{
    auto had_tracks = !m_tracks.is_empty();
    m_tracks.remove_all_matching([&](auto const& existing) { return !existing || existing.ptr().ptr() == &track; });
    if (had_tracks && m_tracks.is_empty())
        did_lose_last_track();
}

void VideoPictureSource::deliver(NonnullRefPtr<Media::I420Picture> const& picture)
{
    // A track may stop, and so leave, while it is given the picture.
    auto tracks = m_tracks;
    for (auto const& track : tracks) {
        if (track)
            track->deliver_video_picture(picture);
    }
}

void VideoPictureSource::end_tracks_for_lost_source()
{
    // The tracks leave the source as they end.
    auto tracks = m_tracks;
    for (auto const& track : tracks) {
        if (track)
            track->video_source_ended();
    }
}

void VideoPictureSource::set_tracks_muted(bool muted)
{
    auto tracks = m_tracks;
    for (auto const& track : tracks) {
        if (track)
            track->set_muted(muted);
    }
}

void VideoPictureSource::end_tracks()
{
    auto tracks = move(m_tracks);
    for (auto const& track : tracks) {
        if (track)
            track->end();
    }
    if (!tracks.is_empty())
        did_lose_last_track();
}

}
