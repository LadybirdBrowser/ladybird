/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/MonotonicMediaClock.h>
#include <LibMedia/PlaybackStates/StartingStateHandler.h>
#include <LibMedia/Processors/AudioMixer.h>
#include <LibMedia/Processors/AudioTimeStretchProcessor.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMedia/Sinks/AudioPlaybackSink.h>
#include <LibMedia/Sinks/AudioPullSink.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/Track.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/Thread.h>
#include <LibThreading/ThreadPool.h>

#include "PlaybackManager.h"

namespace Media {

namespace {

HashMap<VideoSinkHandle, PlaybackManager*>& video_sink_registrations()
{
    static NeverDestroyed<HashMap<VideoSinkHandle, PlaybackManager*>> registrations;
    return *registrations;
}

}

DecoderErrorOr<void> PlaybackManager::prepare_playback_from_demuxer(WeakPlaybackManager const& self, NonnullRefPtr<Demuxer> const& demuxer, Core::EventLoop& main_thread_event_loop)
{
    // Create the video tracks and their producers.
    auto all_video_tracks = TRY(demuxer->get_tracks_for_type(TrackType::Video));

    auto supported_video_tracks = VideoTracks();
    auto supported_video_track_datas = VideoTrackDatas();
    supported_video_tracks.ensure_capacity(all_video_tracks.size());
    supported_video_track_datas.ensure_capacity(all_video_tracks.size());
    for (auto const& track : all_video_tracks) {
        auto video_producer_result = DecodedVideoProducer::try_create(main_thread_event_loop, demuxer, track);
        if (video_producer_result.is_error())
            continue;
        supported_video_tracks.append(track);
        supported_video_track_datas.empend(VideoTrackData(track, video_producer_result.release_value()));
    }
    supported_video_tracks.shrink_to_fit();
    supported_video_track_datas.shrink_to_fit();

    // Create all the audio tracks, their producers, and the audio output.
    auto all_audio_tracks = TRY(demuxer->get_tracks_for_type(TrackType::Audio));

    auto supported_audio_tracks = AudioTracks();
    auto supported_audio_track_datas = AudioTrackDatas();
    supported_audio_tracks.ensure_capacity(all_audio_tracks.size());
    supported_audio_track_datas.ensure_capacity(all_audio_tracks.size());
    for (auto const& track : all_audio_tracks) {
        auto audio_producer_result = DecodedAudioProducer::try_create(main_thread_event_loop, demuxer, track);
        if (audio_producer_result.is_error())
            continue;
        auto audio_producer = audio_producer_result.release_value();
        supported_audio_tracks.append(track);
        supported_audio_track_datas.empend(AudioTrackData(track, move(audio_producer)));
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
    auto start_time_realtime = demuxer->start_time_realtime();

    main_thread_event_loop.deferred_invoke([self, demuxer, video_tracks = move(supported_video_tracks), video_track_datas = move(supported_video_track_datas), preferred_video_track, audio_tracks = move(supported_audio_tracks), audio_track_datas = move(supported_audio_track_datas), preferred_audio_track, duration, start_time_realtime] mutable {
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

        self->m_start_time_realtime = start_time_realtime;
        self->check_for_duration_change(duration);

        self->m_demuxers.append(demuxer);
        demuxer->set_scan_state_change_handler([self] {
            if (!self)
                return;
            self->update_duration_from_scan_states();
            self->update_pipeline_state();
            self->dispatch_buffered_ranges_change();
        });
        self->update_duration_from_scan_states();

        self->set_up_producers();

        self->ensure_audio_output_pipeline();

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
    playback_manager->m_handler = make<StartingStateHandler>(*playback_manager);
    playback_manager->m_handler->on_enter();
    return playback_manager;
}

PlaybackManager::PlaybackManager()
    : m_weak_link(make_ref_counted<WeakPlaybackManagerLink>(*this))
    , m_clock(MUST(MonotonicMediaClock::try_create()))
    , m_time_reader(m_clock->time_reader())
{
}

PlaybackManager::~PlaybackManager()
{
    if (auto* playback_sink = m_audio_output.get_pointer<NonnullRefPtr<AudioPlaybackSink>>())
        (*playback_sink)->set_audio_output_state_change_handler(nullptr);
    set_audio_output_is_non_silent(false);
    m_clock->pause();
    if (auto* pull_output = m_audio_output.get_pointer<PullAudioOutput>()) {
        if (pull_output->is_connected && m_audio_time_stretch_processor) {
            pull_output->sink->disconnect_input(*m_audio_time_stretch_processor);
            pull_output->is_connected = false;
        }
        pull_output->sink->set_state_change_handler(nullptr);
    }
    for (auto& track_data : m_video_track_datas) {
        if (track_data.handle.has_value())
            video_sink_registrations().remove(*track_data.handle);
    }
    m_weak_link->revoke({});
}

static void handle_media_init_error(WeakPlaybackManager self, Core::EventLoop& main_thread_event_loop, DecoderError error)
{
    main_thread_event_loop.deferred_invoke([self = move(self), error = move(error)] mutable {
        if (!self)
            return;
        if (self->on_unsupported_format_error)
            self->on_unsupported_format_error(move(error));
    });
}

void PlaybackManager::add_media_source(NonnullRefPtr<MediaStream> const& stream)
{
    auto self = weak();
    auto& main_thread_event_loop = Core::EventLoop::current();

    Threading::ThreadPool::the().submit([self = move(self), stream, &main_thread_event_loop] mutable {
        auto demuxer_or_error = create_demuxer(stream);
        if (demuxer_or_error.is_error()) {
            handle_media_init_error(move(self), main_thread_event_loop, demuxer_or_error.release_error());
            return;
        }

        auto maybe_error = prepare_playback_from_demuxer(self, demuxer_or_error.release_value(), main_thread_event_loop);
        if (maybe_error.is_error())
            handle_media_init_error(move(self), main_thread_event_loop, maybe_error.release_error());
    });
}

void PlaybackManager::add_media_source(NonnullRefPtr<Demuxer> const& demuxer)
{
    auto self = weak();
    auto& main_thread_event_loop = Core::EventLoop::current();

    Threading::ThreadPool::the().submit([self = move(self), demuxer, &main_thread_event_loop] mutable {
        auto maybe_error = prepare_playback_from_demuxer(self, demuxer, main_thread_event_loop);
        if (maybe_error.is_error())
            handle_media_init_error(move(self), main_thread_event_loop, maybe_error.release_error());
    });
}

WeakPlaybackManager PlaybackManager::weak()
{
    return WeakPlaybackManager(m_weak_link);
}

void PlaybackManager::set_up_producers()
{
    for (auto const& video_track_data : m_video_track_datas) {
        video_track_data.producer->set_error_handler([self = weak()](DecoderError&& error) {
            if (!self)
                return;
            self->dispatch_error(move(error));
        });
        video_track_data.producer->set_read_blocked_change_handler([self = weak(), track = video_track_data.track](ReadBlocked read_blocked) {
            if (!self)
                return;
            self->get_video_data_for_track(track).read_blocked = read_blocked == ReadBlocked::Yes;
            self->update_pipeline_state();
        });
    }

    for (auto const& audio_track_data : m_audio_track_datas) {
        audio_track_data.producer->set_error_handler([self = weak()](DecoderError&& error) {
            if (!self)
                return;
            self->dispatch_error(move(error));
        });
        audio_track_data.producer->set_read_blocked_change_handler([self = weak(), track = audio_track_data.track](ReadBlocked read_blocked) {
            if (!self)
                return;
            self->get_audio_data_for_track(track).read_blocked = read_blocked == ReadBlocked::Yes;
            self->update_pipeline_state();
        });
    }
}

AK::Duration PlaybackManager::current_time() const
{
    auto time = m_handler->current_time();
    return min(time, duration());
}

void PlaybackManager::on_audio_sink_state_changed(PipelineStatus status)
{
    m_audio_sink_status = status;
    update_pipeline_state();
}

void PlaybackManager::on_video_sink_state_changed(Track const& track, PipelineStatus status)
{
    auto& track_data = get_video_data_for_track(track);
    track_data.sink_status = status;
    update_pipeline_state();
}

Optional<AK::Duration> PlaybackManager::verified_end_time_for_track(Track const& track) const
{
    for (auto const& demuxer : m_demuxers) {
        auto const* track_state = demuxer->scan_state().state_for_track(track);
        if (track_state == nullptr)
            continue;
        if (!track_state->reached_end_of_stream || track_state->buffered_ranges.is_empty())
            return {};
        return track_state->buffered_ranges.highest_end_time();
    }
    return {};
}

bool PlaybackManager::has_ticking_audio_sink() const
{
    if (m_audio_output.has<NonnullRefPtr<AudioPlaybackSink>>())
        return true;
    if (auto const* pull_output = m_audio_output.get_pointer<PullAudioOutput>())
        return pull_output->ticking;
    return false;
}

PipelineStatus PlaybackManager::combined_pipeline_status() const
{
    auto status = PipelineStatus::EndOfStream;

    if (has_ticking_audio_sink()) {
        auto audio_status = m_audio_sink_status;
        if (audio_status == PipelineStatus::Suspended)
            audio_status = PipelineStatus::Pending;
        if (audio_status == PipelineStatus::Pending) {
            for (auto const& track_data : m_audio_track_datas) {
                if (!track_data.enabled)
                    continue;
                if (!track_data.read_blocked)
                    continue;
                audio_status = PipelineStatus::Blocked;
                break;
            }
        }
        status = select_combined_pipeline_status(status, audio_status);
    } else {
        for (auto const& track_data : m_audio_track_datas) {
            if (!track_data.enabled)
                continue;
            auto track_status = PipelineStatus::HaveData;
            auto verified_end_time = verified_end_time_for_track(track_data.track);
            if (verified_end_time.has_value() && current_time() >= *verified_end_time)
                track_status = PipelineStatus::EndOfStream;
            status = select_combined_pipeline_status(status, track_status);
        }
    }

    for (auto const& track_data : m_video_track_datas) {
        if (!track_data.handle.has_value())
            continue;
        auto track_status = track_data.sink_status;
        if (track_status == PipelineStatus::Suspended)
            track_status = PipelineStatus::Pending;
        if (!track_data.ticking && track_status != PipelineStatus::Error) {
            auto verified_end_time = verified_end_time_for_track(track_data.track);
            if (verified_end_time.has_value() && current_time() >= *verified_end_time)
                track_status = PipelineStatus::EndOfStream;
        }
        if (track_status == PipelineStatus::Pending && track_data.read_blocked)
            track_status = PipelineStatus::Blocked;
        status = select_combined_pipeline_status(status, track_status);
    }

    return status;
}

void PlaybackManager::update_pipeline_state()
{
    m_handler->on_pipeline_status_changed(combined_pipeline_status());
}

void PlaybackManager::reset_pipeline_state()
{
    for (auto& track_data : m_video_track_datas) {
        if (track_data.video_sink == nullptr)
            continue;
        track_data.sink_status = PipelineStatus::Pending;
    }
    m_audio_sink_status = has_ticking_audio_sink() ? PipelineStatus::Pending : PipelineStatus::HaveData;
}

void PlaybackManager::update_duration_from_scan_states()
{
    auto duration = AK::Duration::zero();
    for (auto const& demuxer : m_demuxers)
        duration = max(duration, demuxer->scan_state().duration);
    check_for_duration_change(duration);
}

void PlaybackManager::check_for_duration_change(AK::Duration duration)
{
    if (m_duration >= duration)
        return;
    m_duration = duration;
    if (on_duration_change)
        on_duration_change(m_duration);
}

void PlaybackManager::set_duration(AK::Duration duration)
{
    if (m_duration == duration)
        return;
    m_duration = duration;
    dispatch_buffered_ranges_change();
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

void PlaybackManager::dispatch_buffered_ranges_change()
{
    if (on_buffered_ranges_change)
        on_buffered_ranges_change();
}

void PlaybackManager::set_clock(NonnullRefPtr<MediaClock> const& clock)
{
    auto time = current_time();
    clock->seek(time);
    m_clock = clock;
    m_time_reader = clock->time_reader();
    for (auto& track_data : m_video_track_datas) {
        if (!track_data.video_sink)
            continue;
        track_data.video_sink->set_time_reader(m_time_reader);
    }
    clock->set_playback_rate(m_playback_rate);
    if (is_playing())
        clock->resume();
}

Optional<Audio::ChannelMap> PlaybackManager::audio_output_channel_map() const
{
    for (auto const& track_data : m_audio_track_datas) {
        if (track_data.enabled && track_data.track.audio_data().sample_specification.channel_map().is_valid())
            return track_data.track.audio_data().sample_specification.channel_map();
    }

    if (m_preferred_audio_track.has_value()) {
        for (auto const& track_data : m_audio_track_datas) {
            if (track_data.track == *m_preferred_audio_track && track_data.track.audio_data().sample_specification.channel_map().is_valid())
                return track_data.track.audio_data().sample_specification.channel_map();
        }
    }

    for (auto const& track_data : m_audio_track_datas) {
        if (track_data.track.audio_data().sample_specification.channel_map().is_valid())
            return track_data.track.audio_data().sample_specification.channel_map();
    }

    return {};
}

ErrorOr<void> PlaybackManager::update_audio_pull_sink_channel_map()
{
    auto* pull_output = m_audio_output.get_pointer<PullAudioOutput>();
    if (!pull_output)
        return {};
    auto channel_map = audio_output_channel_map();
    if (!channel_map.has_value())
        return {};
    return pull_output->sink->set_channel_map(*channel_map);
}

void PlaybackManager::ensure_audio_output_pipeline()
{
    if (m_audio_tracks.is_empty())
        return;
    auto* pull_output = m_audio_output.get_pointer<PullAudioOutput>();
    if (!pull_output && m_audio_output_disabled)
        return;

    if (!m_audio_mixer) {
        m_audio_mixer = MUST(AudioMixer::try_create());
        m_audio_time_stretch_processor = MUST(AudioTimeStretchProcessor::try_create());
        MUST(m_audio_time_stretch_processor->connect_input(*m_audio_mixer));

        // Tracks enabled while there was no audio output are connected now that there is one. The mixer has no output
        // specification yet, so connecting cannot fail.
        for (auto const& track_data : m_audio_track_datas) {
            if (track_data.enabled)
                MUST(m_audio_mixer->connect_input(track_data.producer));
        }
    }

    if (pull_output) {
        if (auto result = update_audio_pull_sink_channel_map(); result.is_error()) {
            disable_audio_after_error(result.error());
            return;
        }
        if (!pull_output->is_connected) {
            MUST(pull_output->sink->connect_input(*m_audio_time_stretch_processor));
            pull_output->is_connected = true;
        }
        return;
    }

    if (m_audio_output.has<NonnullRefPtr<AudioPlaybackSink>>())
        return;

    auto self = weak();
    auto audio_sink = MUST(AudioPlaybackSink::try_create(
        [self](PipelineStatus status) {
            if (!self)
                return;
            self->on_audio_sink_state_changed(status);
        }));
    audio_sink->set_audio_output_state_change_handler([self](bool is_non_silent) {
        if (self)
            self->set_audio_output_is_non_silent(is_non_silent);
    });
    MUST(audio_sink->connect_input(*m_audio_time_stretch_processor));
    audio_sink->set_volume(m_volume);
    audio_sink->set_muted(m_audio_output_muted);
    m_audio_output = audio_sink;
    set_clock(audio_sink);
    audio_sink->on_audio_output_error = [self, sink = audio_sink.ptr()](Error&& error) {
        if (!self)
            return;
        auto* current_sink = self->m_audio_output.get_pointer<NonnullRefPtr<AudioPlaybackSink>>();
        if (!current_sink || current_sink->ptr() != sink)
            return;
        dbgln("Audio output initialization failed with error: {}", error);
        self->disable_audio();
    };
}

void PlaybackManager::set_audio_pull_sink(NonnullRefPtr<AudioPullSink> sink, bool ticking)
{
    if (auto* pull_output = m_audio_output.get_pointer<PullAudioOutput>()) {
        if (pull_output->sink == sink) {
            set_audio_pull_sink_ticking(ticking);
            return;
        }
        if (pull_output->is_connected && m_audio_time_stretch_processor)
            pull_output->sink->disconnect_input(*m_audio_time_stretch_processor);
        pull_output->sink->set_state_change_handler(nullptr);
    }

    if (auto* playback_sink = m_audio_output.get_pointer<NonnullRefPtr<AudioPlaybackSink>>()) {
        (*playback_sink)->set_audio_output_state_change_handler(nullptr);
        if (m_audio_time_stretch_processor)
            (*playback_sink)->disconnect_input(*m_audio_time_stretch_processor);
    }
    set_audio_output_is_non_silent(false);

    sink->set_volume(m_volume);
    sink->set_state_change_handler([self = weak()](PipelineStatus status) {
        if (!self)
            return;
        self->on_audio_sink_state_changed(status);
    });
    sink->set_ticking(ticking);
    m_audio_output = PullAudioOutput { .sink = move(sink), .ticking = ticking };
    auto& pull_output = m_audio_output.get<PullAudioOutput>();
    m_audio_sink_status = PipelineStatus::Pending;
    set_clock(pull_output.sink);
    ensure_audio_output_pipeline();
    update_pipeline_state();
}

void PlaybackManager::set_audio_pull_sink_ticking(bool ticking)
{
    auto* pull_output = m_audio_output.get_pointer<PullAudioOutput>();
    if (!pull_output || pull_output->ticking == ticking)
        return;
    pull_output->ticking = ticking;
    pull_output->sink->set_ticking(ticking);
    if (ticking)
        m_audio_sink_status = PipelineStatus::Pending;
    update_pipeline_state();
}

void PlaybackManager::disable_audio()
{
    if (auto* playback_sink = m_audio_output.get_pointer<NonnullRefPtr<AudioPlaybackSink>>())
        (*playback_sink)->set_audio_output_state_change_handler(nullptr);
    set_audio_output_is_non_silent(false);
    if (m_audio_time_stretch_processor) {
        if (auto* playback_sink = m_audio_output.get_pointer<NonnullRefPtr<AudioPlaybackSink>>())
            (*playback_sink)->disconnect_input(*m_audio_time_stretch_processor);
        if (auto* pull_output = m_audio_output.get_pointer<PullAudioOutput>(); pull_output && pull_output->is_connected)
            pull_output->sink->disconnect_input(*m_audio_time_stretch_processor);
    }
    if (auto* pull_output = m_audio_output.get_pointer<PullAudioOutput>())
        pull_output->sink->set_state_change_handler(nullptr);

    m_audio_output = Empty {};
    m_audio_output_disabled = true;
    m_audio_mixer = nullptr;
    m_audio_time_stretch_processor = nullptr;
    set_clock(MUST(MonotonicMediaClock::try_create()));
    on_audio_sink_state_changed(PipelineStatus::EndOfStream);
}

void PlaybackManager::set_audio_output_is_non_silent(bool is_non_silent)
{
    if (m_audio_output_is_non_silent == is_non_silent)
        return;

    m_audio_output_is_non_silent = is_non_silent;
    if (on_audio_output_state_change)
        on_audio_output_state_change(is_non_silent);
}

void PlaybackManager::disable_audio_after_error(Error const& error)
{
    dbgln("PlaybackManager: Disabling audio output after an error: {}", error);
    disable_audio();
}

void PlaybackManager::attach_video_sink(VideoTrackData& track_data, NonnullRefPtr<VideoSink> video_sink)
{
    VERIFY(track_data.video_sink == nullptr);
    auto track = track_data.track;
    video_sink->set_state_change_handler([self = weak(), track](PipelineStatus status) {
        if (!self)
            return;
        self->on_video_sink_state_changed(track, status);
    });
    video_sink->set_resize_handler([self = weak(), track](Gfx::Size<u32> size) {
        if (!self)
            return;
        auto& track_data = self->get_video_data_for_track(track);
        if (track_data.on_resize)
            track_data.on_resize(size);
    });
    MUST(video_sink->connect_input(track_data.producer));
    track_data.video_sink = move(video_sink);
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

VideoSinkHandle PlaybackManager::reserve_video_sink_handle(Track const& track)
{
    auto& track_data = get_video_data_for_track(track);
    if (track_data.handle.has_value())
        disable_video_sink_by_handle(*track_data.handle);
    track_data.handle = allocate_video_sink_handle();
    track_data.ticking = true;
    video_sink_registrations().set(*track_data.handle, this);
    update_pipeline_state();
    return *track_data.handle;
}

void PlaybackManager::set_video_resize_handler(VideoSinkHandle handle, Function<void(Gfx::Size<u32>)> handler)
{
    if (auto* track_data = find_video_data_for_handle(handle))
        track_data->on_resize = move(handler);
}

void PlaybackManager::disable_video_sink_by_handle(VideoSinkHandle handle)
{
    video_sink_registrations().remove(handle);
    auto* track_data = find_video_data_for_handle(handle);
    if (!track_data)
        return;
    if (track_data->video_sink) {
        track_data->video_sink->disconnect_input(track_data->producer);
        track_data->video_sink = nullptr;
        track_data->sink_status = PipelineStatus::HaveData;
    }
    track_data->handle = {};
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

void PlaybackManager::set_video_sink_ticking(VideoSinkHandle handle, bool ticking)
{
    auto* manager = video_sink_registrations().get(handle).value_or(nullptr);
    if (!manager)
        return;
    auto& track_data = manager->get_video_data_for_handle(handle);
    if (track_data.ticking == ticking)
        return;
    track_data.ticking = ticking;
    manager->update_pipeline_state();
}

void PlaybackManager::detach_video_sink(VideoSinkHandle handle)
{
    auto* track_data = find_video_data_for_handle(handle);
    if (!track_data)
        return;
    if (track_data->video_sink) {
        track_data->video_sink->disconnect_input(track_data->producer);
        track_data->video_sink = nullptr;
    }
    track_data->sink_status = PipelineStatus::Pending;
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

ErrorOr<PlaybackManager::RemoteVideoEdge> PlaybackManager::create_video_edge(VideoSinkHandle handle, RemoteVideoSink::Delegates delegates)
{
    auto* manager = video_sink_registrations().get(handle).value_or(nullptr);
    if (!manager)
        return Error::from_string_literal("No playback manager registered for video sink handle");

    auto pump = TRY(RemoteVideoSink::create(move(delegates)));
    return RemoteVideoEdge {
        .sink = pump,
        .time_reader = manager->m_time_reader,
    };
}

void PlaybackManager::attach_video_edge(VideoSinkHandle handle, NonnullRefPtr<RemoteVideoSink> const& pump)
{
    auto* manager = video_sink_registrations().get(handle).value_or(nullptr);
    if (!manager)
        return;
    auto& track_data = manager->get_video_data_for_handle(handle);
    if (track_data.video_sink != nullptr) {
        dbgln("PlaybackManager: Refusing to attach a video edge to an already-attached video sink handle");
        return;
    }
    manager->attach_video_sink(track_data, pump);
}

RefPtr<VideoFrame> PlaybackManager::current_presented_frame(VideoSinkHandle handle)
{
    auto* manager = video_sink_registrations().get(handle).value_or(nullptr);
    if (!manager)
        return nullptr;
    auto& track_data = manager->get_video_data_for_handle(handle);
    if (!track_data.video_sink)
        return nullptr;
    return track_data.video_sink->current_frame();
}

void PlaybackManager::release_video_edge(VideoSinkHandle handle, VideoSink const& released_sink)
{
    auto* manager = video_sink_registrations().get(handle).value_or(nullptr);
    if (!manager)
        return;
    auto* track_data = manager->find_video_data_for_handle(handle);
    if (!track_data || track_data->video_sink != &released_sink)
        return;
    manager->detach_video_sink(handle);
}

void PlaybackManager::enable_an_audio_track(Track const& track)
{
    auto& track_data = get_audio_data_for_track(track);
    VERIFY(!track_data.enabled);
    m_audio_sink_status = PipelineStatus::HaveData;
    if (m_audio_mixer) {
        m_audio_mixer->seek(current_time());
        if (auto result = m_audio_mixer->connect_input(track_data.producer); result.is_error())
            disable_audio_after_error(result.error());
    }
    track_data.enabled = true;
    if (auto result = update_audio_pull_sink_channel_map(); result.is_error())
        disable_audio_after_error(result.error());
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

void PlaybackManager::disable_an_audio_track(Track const& track)
{
    auto& track_data = get_audio_data_for_track(track);
    VERIFY(track_data.enabled);
    m_audio_sink_status = PipelineStatus::HaveData;
    if (m_audio_mixer) {
        m_audio_mixer->seek(current_time());
        m_audio_mixer->disconnect_input(track_data.producer);
    }
    track_data.enabled = false;
    if (auto result = update_audio_pull_sink_channel_map(); result.is_error())
        disable_audio_after_error(result.error());
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

bool PlaybackManager::track_is_enabled(Track const& track) const
{
    if (track.type() == TrackType::Video) {
        auto const& track_data = get_video_data_for_track(track);
        return track_data.video_sink != nullptr;
    }

    VERIFY(track.type() == TrackType::Audio);
    auto const& track_data = get_audio_data_for_track(track);
    return track_data.enabled;
}

void PlaybackManager::start()
{
    m_handler->start();
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
    reset_pipeline_state();
    m_handler->seek(timestamp, mode);
    m_is_in_error_state = false;
    update_pipeline_state();
}

bool PlaybackManager::is_playing()
{
    return m_handler->is_playing();
}

PlaybackState PlaybackManager::state()
{
    return m_handler->state();
}

AvailableData PlaybackManager::available_data()
{
    return m_handler->available_data();
}

TimeRanges PlaybackManager::buffered_time_ranges() const
{
    TimeRanges intersection { { AK::Duration::zero(), m_duration } };
    bool any_track_contributed_ranges = false;

    for (auto const& demuxer : m_demuxers) {
        for (auto const& track_state : demuxer->scan_state().tracks) {
            if (!is_enabled_supported_track(track_state.track))
                continue;
            auto track_ranges = track_state.buffered_ranges;
            // The estimated duration may lie beyond a track's scanned ranges, but it should be
            // reported as buffered once no further data will arrive to extend that track.
            if (track_state.reached_end_of_stream && !track_ranges.is_empty())
                track_ranges.add_range(track_ranges[track_ranges.size() - 1].start, m_duration);
            intersection = intersection.intersection(track_ranges);
            any_track_contributed_ranges = true;
        }
    }

    if (!any_track_contributed_ranges)
        return {};

    return intersection;
}

bool PlaybackManager::is_enabled_supported_track(Track const& track) const
{
    if (track.type() == TrackType::Video) {
        for (auto const& track_data : m_video_track_datas) {
            if (track_data.track == track)
                return track_data.video_sink != nullptr;
        }
        return false;
    }

    if (track.type() == TrackType::Audio) {
        for (auto const& track_data : m_audio_track_datas) {
            if (track_data.track == track)
                return track_data.enabled;
        }
    }

    return false;
}

void PlaybackManager::set_volume(double volume)
{
    m_volume = volume;
    if (auto* playback_sink = m_audio_output.get_pointer<NonnullRefPtr<AudioPlaybackSink>>())
        (*playback_sink)->set_volume(volume);
    if (auto* pull_output = m_audio_output.get_pointer<PullAudioOutput>())
        pull_output->sink->set_volume(volume);
}

void PlaybackManager::set_audio_output_muted(bool muted)
{
    m_audio_output_muted = muted;
    if (auto* playback_sink = m_audio_output.get_pointer<NonnullRefPtr<AudioPlaybackSink>>())
        (*playback_sink)->set_muted(muted);
}

void PlaybackManager::set_playback_rate(float rate)
{
    m_playback_rate = rate;
    m_clock->set_playback_rate(rate);
    update_pipeline_state();
}

}
