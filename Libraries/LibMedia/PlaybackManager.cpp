/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/PlaybackStates/PausedStateHandler.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/Providers/GenericTimeProvider.h>
#include <LibMedia/Providers/VideoDataProvider.h>
#include <LibMedia/Providers/WrapperTimeProvider.h>
#include <LibMedia/Sinks/AudioMixingSink.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/Track.h>

#include "PlaybackManager.h"

namespace Media {

DecoderErrorOr<void> PlaybackManager::ThreadData::init()
{
    auto demuxer = TRY([&] -> DecoderErrorOr<NonnullRefPtr<Demuxer>> {
        if (Matroska::MatroskaDemuxer::sniff_webm(m_stream))
            return Matroska::MatroskaDemuxer::from_stream(m_stream);
        if (FFmpeg::FFmpegDemuxer::sniff_mp4(m_stream))
            return FFmpeg::FFmpegDemuxer::from_stream(m_stream);
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "No suitable demuxer found"sv);
    }());

    // Create the video tracks and their data providers.
    auto all_video_tracks = TRY(demuxer->get_tracks_for_type(TrackType::Video));

    auto supported_video_tracks = VideoTracks();
    auto supported_video_track_datas = VideoTrackDatas();
    supported_video_tracks.ensure_capacity(all_video_tracks.size());
    supported_video_track_datas.ensure_capacity(all_video_tracks.size());
    for (auto const& track : all_video_tracks) {
        auto video_data_provider_result = VideoDataProvider::try_create(m_main_thread_event_loop, demuxer, track);
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
        auto audio_data_provider_result = AudioDataProvider::try_create(m_main_thread_event_loop, demuxer, track);
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

    auto preferred_video_track = demuxer->get_preferred_track_for_type(TrackType::Video).value_or({});
    if (preferred_video_track.has_value() && !supported_video_tracks.contains_slow(*preferred_video_track))
        preferred_video_track = {};
    auto preferred_audio_track = demuxer->get_preferred_track_for_type(TrackType::Audio).value_or({});
    if (preferred_audio_track.has_value() && !supported_audio_tracks.contains_slow(*preferred_audio_track))
        preferred_audio_track = {};

    auto duration = demuxer->total_duration().value_or(AK::Duration::zero());

    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr { *this }, supported_video_tracks = move(supported_video_tracks), supported_video_track_datas = move(supported_video_track_datas), preferred_video_track, supported_audio_tracks = move(supported_audio_tracks), supported_audio_track_datas = move(supported_audio_track_datas), preferred_audio_track, duration] mutable {
        self->m_on_demuxer_init_done(move(supported_video_tracks), move(supported_video_track_datas), preferred_video_track, move(supported_audio_tracks), move(supported_audio_track_datas), preferred_audio_track, duration);
    });

    return {};
}

void PlaybackManager::ThreadData::on_error(DecoderError&& error)
{
    m_main_thread_event_loop.deferred_invoke([self = NonnullRefPtr { *this }, error] mutable {
        self->m_on_demuxer_error(move(error));
    });
}

DecoderErrorOr<NonnullRefPtr<PlaybackManager>> PlaybackManager::try_create(NonnullRefPtr<IncrementallyPopulatedStream> const& stream)
{
    // Create the weak wrapper.
    auto weak_playback_manager = DECODER_TRY_ALLOC(try_make_ref_counted<WeakPlaybackManager>());

    auto on_demuxer_init_error = [weak_playback_manager](DecoderError&& error) {
        auto playback_manager = weak_playback_manager->take_strong();
        if (!playback_manager)
            return;
        if (playback_manager->on_demuxer_init_error)
            playback_manager->on_demuxer_init_error(move(error));
    };

    auto thread_data = DECODER_TRY_ALLOC(try_make_ref_counted<PlaybackManager::ThreadData>(stream, [weak_playback_manager](VideoTracks&& supported_video_tracks, VideoTrackDatas&& supported_video_track_datas, Optional<Track> preferred_video_track, AudioTracks&& supported_audio_tracks, AudioTrackDatas&& supported_audio_track_datas, Optional<Track> preferred_audio_track, AK::Duration duration) {
        auto playback_manager = weak_playback_manager->take_strong();
        if (!playback_manager)
            return;

        RefPtr<AudioMixingSink> audio_sink = nullptr;
        if (!supported_audio_tracks.is_empty())
            audio_sink = MUST(AudioMixingSink::try_create());

        // Create the time provider.
        auto time_provider = MUST([&] -> ErrorOr<NonnullRefPtr<MediaTimeProvider>> {
            if (audio_sink)
                return TRY(try_make_ref_counted<WrapperTimeProvider<AudioMixingSink>>(*audio_sink));
            return TRY(try_make_ref_counted<GenericTimeProvider>());
        }());

        playback_manager->m_time_provider = time_provider;
        playback_manager->m_audio_sink = audio_sink;

        playback_manager->m_video_tracks = supported_video_tracks;
        playback_manager->m_video_track_datas = supported_video_track_datas;
        playback_manager->m_preferred_video_track = preferred_video_track;
        playback_manager->m_audio_tracks = supported_audio_tracks;
        playback_manager->m_audio_track_datas = supported_audio_track_datas;
        playback_manager->m_preferred_audio_track = preferred_audio_track;
        playback_manager->m_duration = duration;
        playback_manager->m_ready_to_use = true;

        playback_manager->set_up_error_handlers();

        playback_manager->on_ready(); }, move(on_demuxer_init_error)));

    auto thread = DECODER_TRY_ALLOC(Threading::Thread::try_create([thread_data] -> int {
        auto maybe_error = thread_data->init();
        if (maybe_error.is_error())
            thread_data->on_error(maybe_error.release_error());
        return 0;
    }));

    auto playback_manager = DECODER_TRY_ALLOC(adopt_nonnull_ref_or_enomem(new (nothrow) PlaybackManager(stream, weak_playback_manager, thread_data)));
    weak_playback_manager->m_manager = playback_manager;
    playback_manager->m_handler = DECODER_TRY_ALLOC(try_make<PausedStateHandler>(*playback_manager));

    thread->start();
    thread->detach();

    return playback_manager;
}

PlaybackManager::PlaybackManager(NonnullRefPtr<IncrementallyPopulatedStream> const& stream, NonnullRefPtr<WeakPlaybackManager> const& weak_wrapper, NonnullRefPtr<ThreadData> const& thread_data)
    : m_stream(stream)
    , m_weak_wrapper(weak_wrapper)
    , m_thread_data(thread_data)
{
}

PlaybackManager::ThreadData::ThreadData(NonnullRefPtr<IncrementallyPopulatedStream> stream, OnDemuxerInitDone&& on_demuxer_init_done, OnDemuxerError&& on_demuxer_error)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_stream(move(stream))
    , m_on_demuxer_init_done(move(on_demuxer_init_done))
    , m_on_demuxer_error(move(on_demuxer_error))
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
        // FIXME: This is not error handler
        video_track_data.provider->set_frames_queue_is_full_handler([weak_self = m_weak_wrapper] {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            self->m_handler->exit_buffering();
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
    VERIFY(m_ready_to_use);
    return m_duration;
}

Optional<Track> PlaybackManager::preferred_video_track()
{
    VERIFY(m_ready_to_use);
    return m_preferred_video_track;
}

Optional<Track> PlaybackManager::preferred_audio_track()
{
    VERIFY(m_ready_to_use);
    return m_preferred_audio_track;
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
    VERIFY(m_time_provider);

    auto& track_data = get_video_data_for_track(track);
    if (track_data.display == nullptr) {
        track_data.display = MUST(Media::DisplayingVideoSink::try_create(*m_time_provider, m_stream));
        track_data.display->set_provider(track, track_data.provider);
        track_data.display->m_on_start_buffering = [this] {
            m_handler->enter_buffering();
        };
        track_data.provider->start();
        track_data.provider->seek(m_time_provider->current_time(), SeekMode::Accurate);
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
    if (!had_provider) {
        track_data.provider->start();
        track_data.provider->seek(current_time());
    }
}

void PlaybackManager::disable_an_audio_track(Track const& track)
{
    auto& track_data = get_audio_data_for_track(track);
    VERIFY(track_data.provider == m_audio_sink->provider(track));
    m_audio_sink->set_provider(track, nullptr);
}

void PlaybackManager::play()
{
    m_handler->play();
}

void PlaybackManager::pause()
{
    m_handler->pause();
}

void PlaybackManager::seek(AK::Duration timestamp, SeekMode mode)
{
    m_handler->seek(timestamp, mode);
    m_is_in_error_state = false;
}

bool PlaybackManager::is_playing()
{
    return m_handler->is_playing();
}

PlaybackState PlaybackManager::state()
{
    return m_handler->state();
}

void PlaybackManager::set_volume(double volume)
{
    if (m_audio_sink)
        m_audio_sink->set_volume(volume);
}

}
