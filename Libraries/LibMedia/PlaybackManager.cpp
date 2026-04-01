/*
 * Copyright (c) 2022-2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Containers/Matroska/MatroskaDemuxer.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/PlaybackStates/BufferingStateHandler.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/Providers/GenericTimeProvider.h>
#include <LibMedia/Providers/VideoDataProvider.h>
#include <LibMedia/Providers/WrapperTimeProvider.h>
#include <LibMedia/Sinks/AudioMixingSink.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/Track.h>
#include <LibThreading/Thread.h>
#include <LibThreading/ThreadPool.h>

#include "PlaybackManager.h"

namespace Media {

DecoderErrorOr<NonnullRefPtr<Demuxer>> PlaybackManager::create_demuxer_for_stream(NonnullRefPtr<MediaStream> const& stream)
{
    if (Matroska::Reader::is_matroska_or_webm(stream->create_cursor()))
        return Matroska::MatroskaDemuxer::from_stream(stream);
    return FFmpeg::FFmpegDemuxer::from_stream(stream);
}

DecoderErrorOr<void> PlaybackManager::prepare_playback_from_demuxer(WeakPlaybackManager const& self, NonnullRefPtr<Demuxer> const& demuxer, NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop_reference)
{
    // Create the video tracks and their data providers.
    auto all_video_tracks = TRY(demuxer->get_tracks_for_type(TrackType::Video));

    auto supported_video_tracks = VideoTracks();
    auto supported_video_track_datas = VideoTrackDatas();
    supported_video_tracks.ensure_capacity(all_video_tracks.size());
    supported_video_track_datas.ensure_capacity(all_video_tracks.size());
    for (auto const& track : all_video_tracks) {
        auto video_data_provider_result = VideoDataProvider::try_create(main_thread_event_loop_reference, demuxer, track);
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
        auto audio_data_provider_result = AudioDataProvider::try_create(main_thread_event_loop_reference, demuxer, track);
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

    auto main_thread_event_loop = main_thread_event_loop_reference->take();
    main_thread_event_loop->deferred_invoke([self, video_tracks = move(supported_video_tracks), video_track_datas = move(supported_video_track_datas), preferred_video_track, audio_tracks = move(supported_audio_tracks), audio_track_datas = move(supported_audio_track_datas), preferred_audio_track, duration] mutable {
        if (!self)
            return;

        for (auto const& existing_track : self->m_video_tracks) {
            if (video_tracks.contains_slow(existing_track)) {
                self->on_unsupported_format_error(DecoderError::with_description(DecoderErrorCategory::Invalid, "Duplicate video track found"sv));
                return;
            }
        }
        for (auto const& existing_track : self->m_audio_tracks) {
            if (audio_tracks.contains_slow(existing_track)) {
                self->on_unsupported_format_error(DecoderError::with_description(DecoderErrorCategory::Invalid, "Duplicate audio track found"sv));
                return;
            }
        }

        auto first_new_video_index = self->m_video_tracks.size();
        auto first_new_audio_index = self->m_audio_tracks.size();

        self->m_video_tracks.extend(move(video_tracks));
        self->m_video_track_datas.extend(move(video_track_datas));
        self->m_audio_tracks.extend(move(audio_tracks));
        self->m_audio_track_datas.extend(move(audio_track_datas));

        if (!self->m_preferred_video_track.has_value())
            self->m_preferred_video_track = preferred_video_track;
        if (!self->m_preferred_audio_track.has_value())
            self->m_preferred_audio_track = preferred_audio_track;

        self->check_for_duration_change(duration);

        self->set_up_data_providers();

        if (!self->m_audio_output_disabled && !self->m_audio_sink && !self->m_audio_tracks.is_empty()) {
            self->m_audio_sink = MUST(AudioMixingSink::try_create());
            self->set_time_provider(make_ref_counted<WrapperTimeProvider<AudioMixingSink>>(*self->m_audio_sink));
            self->m_audio_sink->on_audio_output_error = [self](Error&& error) {
                if (!self)
                    return;
                dbgln("Audio output initialization failed with error: {}", error);
                self->disable_audio();
            };
            self->m_audio_sink->on_start_buffering = [self](Track const& track) {
                if (!self)
                    return;
                self->track_started_buffering(track);
            };
        }

        if (self->on_track_added) {
            for (size_t i = first_new_audio_index; i < self->m_audio_tracks.size(); i++)
                self->on_track_added(self->m_audio_tracks[i]);
            for (size_t i = first_new_video_index; i < self->m_video_tracks.size(); i++)
                self->on_track_added(self->m_video_tracks[i]);
        }

        if (self->on_metadata_parsed)
            self->on_metadata_parsed();
    });

    return {};
}

NonnullOwnPtr<PlaybackManager> PlaybackManager::create()
{
    auto playback_manager = adopt_own(*new (nothrow) PlaybackManager());
    playback_manager->m_handler = make<BufferingStateHandler>(*playback_manager, false);
    playback_manager->m_handler->on_enter();
    return playback_manager;
}

PlaybackManager::PlaybackManager()
    : m_weak_link(make_ref_counted<WeakPlaybackManagerLink>(*this))
    , m_time_provider(make_ref_counted<GenericTimeProvider>())
{
}

PlaybackManager::~PlaybackManager()
{
    m_weak_link->revoke({});
}

static void handle_media_init_error(WeakPlaybackManager self, NonnullRefPtr<Core::WeakEventLoopReference> const& main_thread_event_loop_reference, DecoderError error)
{
    auto main_thread_event_loop = main_thread_event_loop_reference->take();
    if (!main_thread_event_loop)
        return;
    main_thread_event_loop->deferred_invoke([self = move(self), error = move(error)] mutable {
        if (!self)
            return;
        if (self->on_unsupported_format_error)
            self->on_unsupported_format_error(move(error));
    });
}

void PlaybackManager::add_media_source(NonnullRefPtr<MediaStream> const& stream)
{
    auto self = weak();
    auto main_thread_event_loop_reference = Core::EventLoop::current_weak();

    Threading::ThreadPool::the().submit([self = move(self), stream, main_thread_event_loop_reference = move(main_thread_event_loop_reference)] mutable {
        auto demuxer_or_error = create_demuxer_for_stream(stream);
        if (demuxer_or_error.is_error()) {
            handle_media_init_error(move(self), move(main_thread_event_loop_reference), demuxer_or_error.release_error());
            return;
        }

        auto maybe_error = prepare_playback_from_demuxer(self, demuxer_or_error.release_value(), main_thread_event_loop_reference);
        if (maybe_error.is_error())
            handle_media_init_error(move(self), move(main_thread_event_loop_reference), maybe_error.release_error());
    });
}

void PlaybackManager::add_media_source(NonnullRefPtr<Demuxer> const& demuxer)
{
    auto self = weak();
    auto main_thread_event_loop_reference = Core::EventLoop::current_weak();

    Threading::ThreadPool::the().submit([self = move(self), demuxer, main_thread_event_loop_reference = move(main_thread_event_loop_reference)] mutable {
        auto maybe_error = prepare_playback_from_demuxer(self, demuxer, main_thread_event_loop_reference);
        if (maybe_error.is_error())
            handle_media_init_error(move(self), move(main_thread_event_loop_reference), maybe_error.release_error());
    });
}

WeakPlaybackManager PlaybackManager::weak()
{
    return WeakPlaybackManager(m_weak_link);
}

void PlaybackManager::set_up_data_providers()
{
    for (auto const& video_track_data : m_video_track_datas) {
        auto track = video_track_data.track;
        video_track_data.provider->set_error_handler([self = weak(), track](DecoderError&& error) {
            if (!self)
                return;
            if (error.category() == DecoderErrorCategory::EndOfStream) {
                self->track_stopped_buffering(track);
                return;
            }
            self->dispatch_error(move(error));
        });
        video_track_data.provider->set_duration_change_handler([self = weak()](AK::Duration time) {
            if (!self)
                return;
            self->check_for_duration_change(time);
        });
        video_track_data.provider->set_frames_queue_is_full_handler([self = weak(), track] {
            if (!self)
                return;
            self->track_stopped_buffering(track);
        });
    }

    for (auto const& audio_track_data : m_audio_track_datas) {
        auto track = audio_track_data.track;
        audio_track_data.provider->set_error_handler([self = weak(), track](DecoderError&& error) {
            if (!self)
                return;
            if (error.category() == DecoderErrorCategory::EndOfStream) {
                self->track_stopped_buffering(track);
                return;
            }
            self->dispatch_error(move(error));
        });
        audio_track_data.provider->set_duration_change_handler([self = weak()](AK::Duration time) {
            if (!self)
                return;
            self->check_for_duration_change(time);
        });
        audio_track_data.provider->set_queue_is_full_handler([self = weak(), track] {
            if (!self)
                return;
            self->track_stopped_buffering(track);
        });
    }
}

void PlaybackManager::track_started_buffering(Track const& track)
{
    bool was_buffering = !m_tracks_still_buffering.is_empty();
    m_tracks_still_buffering.set(track);
    if (!was_buffering)
        m_handler->enter_buffering();
}

void PlaybackManager::track_stopped_buffering(Track const& track)
{
    bool was_buffering = !m_tracks_still_buffering.is_empty();
    m_tracks_still_buffering.remove(track);
    if (was_buffering && m_tracks_still_buffering.is_empty())
        m_handler->exit_buffering();
}

void PlaybackManager::check_for_duration_change(AK::Duration duration)
{
    if (m_duration >= duration)
        return;
    m_duration = duration;
    if (on_duration_change)
        on_duration_change(m_duration);
}

void PlaybackManager::dispatch_error(DecoderError&& error)
{
    VERIFY(error.category() != DecoderErrorCategory::EndOfStream);

    if (m_is_in_error_state)
        return;
    m_is_in_error_state = true;
    if (on_error)
        on_error(move(error));
}

void PlaybackManager::set_time_provider(NonnullRefPtr<MediaTimeProvider> const& provider)
{
    auto time = current_time();
    provider->set_time(time);
    m_time_provider = provider;
    for (auto& track_data : m_video_track_datas) {
        if (!track_data.display)
            continue;
        track_data.display->set_time_provider(provider);
    }
    if (is_playing())
        provider->resume();
}

void PlaybackManager::disable_audio()
{
    m_audio_sink = nullptr;
    set_time_provider(make_ref_counted<GenericTimeProvider>());

    for (auto const& track : m_audio_tracks)
        track_stopped_buffering(track);
}

NonnullRefPtr<DisplayingVideoSink> PlaybackManager::get_or_create_the_displaying_video_sink_for_track(Track const& track)
{
    auto& track_data = get_video_data_for_track(track);
    if (track_data.display == nullptr) {
        track_data.display = MUST(Media::DisplayingVideoSink::try_create(m_time_provider));
        track_data.display->set_provider(track, track_data.provider);
        track_data.display->m_on_start_buffering = [this, track] {
            track_started_buffering(track);
        };
        m_tracks_still_buffering.set(track);
        m_handler->on_track_enabled(track);
    }

    VERIFY(track_data.display->provider(track) == track_data.provider);
    return *track_data.display;
}

void PlaybackManager::remove_the_displaying_video_sink_for_track(Track const& track)
{
    auto& track_data = get_video_data_for_track(track);
    track_data.display->set_provider(track, nullptr);
    track_data.display = nullptr;
    track_stopped_buffering(track);
}

void PlaybackManager::enable_an_audio_track(Track const& track)
{
    if (!m_audio_sink)
        return;
    auto& track_data = get_audio_data_for_track(track);
    auto had_provider = m_audio_sink->provider(track) != nullptr;
    m_audio_sink->set_provider(track, track_data.provider);
    if (!had_provider) {
        m_tracks_still_buffering.set(track);
        m_handler->on_track_enabled(track);
    }
}

void PlaybackManager::disable_an_audio_track(Track const& track)
{
    if (!m_audio_sink)
        return;
    auto& track_data = get_audio_data_for_track(track);
    VERIFY(track_data.provider == m_audio_sink->provider(track));
    m_audio_sink->set_provider(track, nullptr);
    track_stopped_buffering(track);
}

bool PlaybackManager::track_is_enabled(Track const& track) const
{
    if (track.type() == TrackType::Video) {
        auto const& track_data = get_video_data_for_track(track);
        return track_data.display != nullptr;
    }

    VERIFY(track.type() == TrackType::Audio);
    if (!m_audio_sink)
        return false;

    auto const& track_data = get_audio_data_for_track(track);
    auto const& assigned_provider = m_audio_sink->provider(track);
    if (assigned_provider == nullptr)
        return false;
    VERIFY(track_data.provider == assigned_provider);
    return true;
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

bool PlaybackManager::has_future_data()
{
    return m_handler->has_future_data();
}

TimeRanges PlaybackManager::buffered_time_ranges() const
{
    TimeRanges intersection;

    auto intersect_ranges = [&](auto const& track_datas) {
        for (auto const& track_data : track_datas) {
            if (!track_is_enabled(track_data.track))
                continue;

            auto range = track_data.provider->buffered_time_ranges();
            if (intersection.is_empty()) {
                intersection = range;
                continue;
            }
            intersection = intersection.intersection(range);
        }
    };
    intersect_ranges(m_video_track_datas);
    intersect_ranges(m_audio_track_datas);

    return intersection;
}

void PlaybackManager::set_volume(double volume)
{
    if (m_audio_sink)
        m_audio_sink->set_volume(volume);
}

}
