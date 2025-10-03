/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/MutexedDemuxer.h>
#include <LibMedia/Providers/VideoDataProvider.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/Track.h>

#include "PlaybackManager.h"

namespace Media {

DecoderErrorOr<NonnullRefPtr<PlaybackManager>> PlaybackManager::try_create(NonnullOwnPtr<SeekableStream>&& stream)
{
    auto inner_demuxer = DECODER_TRY_ALLOC(FFmpeg::FFmpegDemuxer::create(move(stream)));
    auto demuxer = DECODER_TRY_ALLOC(try_make_ref_counted<MutexedDemuxer>(inner_demuxer));

    // Create the weak wrapper.
    auto weak_playback_manager = DECODER_TRY_ALLOC(try_make_ref_counted<WeakPlaybackManager>());

    // Create the video tracks and their data providers.
    auto all_video_tracks = TRY(demuxer->get_tracks_for_type(TrackType::Video));

    auto supported_video_tracks = VideoTracks();
    auto supported_video_track_datas = VideoTrackDatas();
    supported_video_tracks.ensure_capacity(all_video_tracks.size());
    supported_video_track_datas.ensure_capacity(all_video_tracks.size());
    for (auto const& track : all_video_tracks) {
        auto video_data_provider_result = VideoDataProvider::try_create(demuxer, track);
        if (video_data_provider_result.is_error())
            continue;
        supported_video_tracks.append(track);
        supported_video_track_datas.empend(VideoTrackData(track, video_data_provider_result.release_value(), nullptr));
    }
    supported_video_tracks.shrink_to_fit();
    supported_video_track_datas.shrink_to_fit();

    if (supported_video_tracks.is_empty())
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "No supported video tracks found"sv);

    auto playback_manager = DECODER_TRY_ALLOC(adopt_nonnull_ref_or_enomem(new (nothrow) PlaybackManager(demuxer, weak_playback_manager, move(supported_video_tracks), move(supported_video_track_datas))));
    weak_playback_manager->m_manager = playback_manager;
    playback_manager->set_up_error_handlers();
    return playback_manager;
}

PlaybackManager::PlaybackManager(NonnullRefPtr<MutexedDemuxer> const& demuxer, NonnullRefPtr<WeakPlaybackManager> const& weak_wrapper, VideoTracks&& video_tracks, VideoTrackDatas&& video_track_datas)
    : m_demuxer(demuxer)
    , m_weak_wrapper(weak_wrapper)
    , m_video_tracks(video_tracks)
    , m_video_track_datas(video_track_datas)
    , m_real_time_base(MonotonicTime::now())
{
}

PlaybackManager::~PlaybackManager()
{
    m_weak_wrapper->revoke();
}

void PlaybackManager::set_up_error_handlers()
{
    for (auto const& video_track_data : m_video_track_datas) {
        video_track_data.provider->set_error_handler([weak_self = m_weak_wrapper](DecoderError&& error) {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            self->dispatch_error(move(error));
        });
    }
}

void PlaybackManager::dispatch_error(DecoderError&& error)
{
    if (m_is_in_error_state)
        return;
    m_is_in_error_state = true;
    if (on_error)
        on_error(move(error));
}

AK::Duration PlaybackManager::current_time() const
{
    return MonotonicTime::now() - m_real_time_base;
}

AK::Duration PlaybackManager::duration() const
{
    return m_demuxer->total_duration().value_or(AK::Duration::zero());
}

Optional<Track> PlaybackManager::preferred_video_track()
{
    auto result = m_demuxer->get_preferred_track_for_type(TrackType::Video).value_or({});
    if (result.has_value() && !m_video_tracks.contains_slow(result.value()))
        return {};
    return result;
}

PlaybackManager::VideoTrackData& PlaybackManager::get_video_data_for_track(Track const& track)
{
    for (auto& track_data : m_video_track_datas) {
        if (track_data.track == track)
            return track_data;
    }

    VERIFY_NOT_REACHED();
}

NonnullRefPtr<DisplayingVideoSink> PlaybackManager::get_or_create_the_displaying_video_sink_for_track(Track const& track)
{
    auto& track_data = get_video_data_for_track(track);
    if (track_data.display == nullptr) {
        track_data.display = MUST(Media::DisplayingVideoSink::try_create(m_weak_wrapper));
        track_data.display->set_provider(track, track_data.provider);
        track_data.provider->seek(current_time());
    }

    VERIFY(track_data.display->provider(track) == track_data.provider);
    return *track_data.display;
}

void PlaybackManager::remove_the_displaying_video_sink_for_track(Track const& track)
{
    auto& track_data = get_video_data_for_track(track);
    track_data.display->set_provider(track, nullptr);
    track_data.display = nullptr;
}

}
