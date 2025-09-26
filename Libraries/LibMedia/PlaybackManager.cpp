/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/MutexedDemuxer.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/Providers/GenericTimeProvider.h>
#include <LibMedia/Providers/VideoDataProvider.h>
#include <LibMedia/Providers/WrapperTimeProvider.h>
#include <LibMedia/Sinks/AudioMixingSink.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/Track.h>

#include "PlaybackManager.h"

namespace Media {

DecoderErrorOr<NonnullRefPtr<PlaybackManager>> PlaybackManager::try_create(ReadonlyBytes data)
{
    auto inner_demuxer = TRY([&] -> DecoderErrorOr<NonnullRefPtr<Demuxer>> {
        auto matroska_result = Matroska::MatroskaDemuxer::from_data(data);
        if (!matroska_result.is_error())
            return matroska_result.release_value();
        return DECODER_TRY_ALLOC(FFmpeg::FFmpegDemuxer::create(make<FixedMemoryStream>(data)));
    }());
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

    // Create all the audio tracks, their data providers, and the audio output.
    auto all_audio_tracks = TRY(demuxer->get_tracks_for_type(TrackType::Audio));

    auto supported_audio_tracks = AudioTracks();
    auto supported_audio_track_datas = AudioTrackDatas();
    supported_audio_tracks.ensure_capacity(all_audio_tracks.size());
    supported_audio_track_datas.ensure_capacity(all_audio_tracks.size());
    for (auto const& track : all_audio_tracks) {
        auto audio_data_provider_result = AudioDataProvider::try_create(demuxer, track);
        if (audio_data_provider_result.is_error())
            continue;
        auto audio_data_provider = audio_data_provider_result.release_value();
        supported_audio_tracks.append(track);
        supported_audio_track_datas.empend(AudioTrackData(track, move(audio_data_provider)));
    }
    supported_audio_tracks.shrink_to_fit();
    supported_audio_track_datas.shrink_to_fit();

    if (supported_video_tracks.is_empty() && supported_audio_tracks.is_empty())
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "No supported video or audio tracks found"sv);

    RefPtr<AudioMixingSink> audio_sink = nullptr;
    if (!supported_audio_tracks.is_empty())
        audio_sink = DECODER_TRY_ALLOC(AudioMixingSink::try_create());

    // Create the time provider.
    auto time_provider = DECODER_TRY_ALLOC([&] -> ErrorOr<NonnullRefPtr<MediaTimeProvider>> {
        if (audio_sink)
            return TRY(try_make_ref_counted<WrapperTimeProvider<AudioMixingSink>>(*audio_sink));
        return TRY(try_make_ref_counted<GenericTimeProvider>());
    }());

    auto playback_manager = DECODER_TRY_ALLOC(adopt_nonnull_ref_or_enomem(new (nothrow) PlaybackManager(demuxer, weak_playback_manager, time_provider, move(supported_video_tracks), move(supported_video_track_datas), audio_sink, move(supported_audio_tracks), move(supported_audio_track_datas))));
    weak_playback_manager->m_manager = playback_manager;
    playback_manager->set_up_error_handlers();
    return playback_manager;
}

PlaybackManager::PlaybackManager(NonnullRefPtr<MutexedDemuxer> const& demuxer, NonnullRefPtr<WeakPlaybackManager> const& weak_wrapper, NonnullRefPtr<MediaTimeProvider> const& time_provider, VideoTracks&& video_tracks, VideoTrackDatas&& video_track_datas, RefPtr<AudioMixingSink> const& audio_sink, AudioTracks&& audio_tracks, AudioTrackDatas&& audio_track_datas)
    : m_demuxer(demuxer)
    , m_weak_wrapper(weak_wrapper)
    , m_time_provider(time_provider)
    , m_video_tracks(video_tracks)
    , m_video_track_datas(video_track_datas)
    , m_audio_sink(audio_sink)
    , m_audio_tracks(audio_tracks)
    , m_audio_track_datas(audio_track_datas)
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

    for (auto const& audio_track_data : m_audio_track_datas) {
        audio_track_data.provider->set_error_handler([weak_self = m_weak_wrapper](DecoderError&& error) {
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

Optional<Track> PlaybackManager::preferred_audio_track()
{
    auto result = m_demuxer->get_preferred_track_for_type(TrackType::Audio).value_or({});
    if (result.has_value() && !m_audio_tracks.contains_slow(result.value()))
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
        track_data.display = MUST(Media::DisplayingVideoSink::try_create(m_time_provider));
        track_data.display->set_provider(track, track_data.provider);
        track_data.provider->seek(m_time_provider->current_time());
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

PlaybackManager::AudioTrackData& PlaybackManager::get_audio_data_for_track(Track const& track)
{
    for (auto& track_data : m_audio_track_datas) {
        if (track_data.track == track)
            return track_data;
    }

    VERIFY_NOT_REACHED();
}

void PlaybackManager::enable_an_audio_track(Track const& track)
{
    auto& track_data = get_audio_data_for_track(track);
    auto had_provider = m_audio_sink->provider(track) != nullptr;
    m_audio_sink->set_provider(track, track_data.provider);
    if (!had_provider)
        track_data.provider->seek(current_time());
}

void PlaybackManager::disable_an_audio_track(Track const& track)
{
    auto& track_data = get_audio_data_for_track(track);
    VERIFY(track_data.provider == m_audio_sink->provider(track));
    m_audio_sink->set_provider(track, nullptr);
}

void PlaybackManager::play()
{
    m_time_provider->resume();
}

void PlaybackManager::pause()
{
    m_time_provider->pause();
}

}
