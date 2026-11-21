/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/DemuxerRegistry.h>
#include <LibMedia/MonotonicMediaClock.h>
#include <LibMedia/PlaybackStates/EndedStateHandler.h>
#include <LibMedia/PlaybackStates/StartingStateHandler.h>
#include <LibMedia/Processors/AudioMixer.h>
#include <LibMedia/Processors/AudioTimeStretchProcessor.h>
#include <LibMedia/Producers/DecodedAudioProducer.h>
#include <LibMedia/Producers/DecodedVideoProducer.h>
#include <LibMedia/Sinks/AudioPlaybackSink.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/Track.h>
#include <LibMediaCodecs/VideoFrame.h>
#include <LibThreading/Thread.h>

#include "PlaybackManager.h"

namespace Media {

namespace {

HashMap<VideoSinkHandle, PlaybackManager*>& video_sink_registrations()
{
    static NeverDestroyed<HashMap<VideoSinkHandle, PlaybackManager*>> registrations;
    return *registrations;
}

}

DecoderErrorOr<void> PlaybackManager::prepare_playback_from_demuxer(WeakPlaybackManager const& self, NonnullRefPtr<Demuxer> const& demuxer, Core::EventLoop& main_thread_event_loop, NonnullRefPtr<AddMediaSourcePromise>& promise)
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
        supported_video_track_datas.empend(VideoTrackData(track, demuxer, video_producer_result.release_value()));
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
        supported_audio_track_datas.empend(AudioTrackData(track, demuxer, move(audio_producer)));
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
    auto earliest_possible_position = demuxer->earliest_possible_position();

    main_thread_event_loop.deferred_invoke([self, demuxer, video_tracks = move(supported_video_tracks), video_track_datas = move(supported_video_track_datas), preferred_video_track, audio_tracks = move(supported_audio_tracks), audio_track_datas = move(supported_audio_track_datas), preferred_audio_track, duration, start_time_realtime, earliest_possible_position, promise = move(promise)] mutable {
        if (!self)
            return;

        for (auto const& existing_track : self->m_video_tracks) {
            if (video_tracks.contains_slow(existing_track)) {
                promise->reject(DecoderError::with_description(DecoderErrorCategory::Invalid, "Duplicate video track found"sv));
                return;
            }
        }
        for (auto const& existing_track : self->m_audio_tracks) {
            if (audio_tracks.contains_slow(existing_track)) {
                promise->reject(DecoderError::with_description(DecoderErrorCategory::Invalid, "Duplicate audio track found"sv));
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

        if (self->m_demuxers.is_empty())
            self->m_timeline.earliest_possible_position = earliest_possible_position;
        else
            self->m_timeline.earliest_possible_position = min(self->m_timeline.earliest_possible_position, earliest_possible_position);
        self->check_for_demuxed_duration_change(duration);

        // A seek before playback starts has already chosen where playback begins.
        if (!self->m_started && self->m_handler->state() == PlaybackState::Starting)
            self->seek_clock_and_video_sinks(self->m_timeline.earliest_possible_position);

        self->m_demuxers.append(demuxer);
        demuxer->set_scan_state_change_handler([self, &demuxer = *demuxer](TimeRanges const& invalidated_ranges) {
            if (!self)
                return;
            self->update_duration_from_scan_states();
            self->seek_tracks_with_invalidated_data(demuxer, invalidated_ranges);
            self->update_pipeline_state();
            self->dispatch_buffered_ranges_change();
        });
        self->update_duration_from_scan_states();

        self->set_up_producers();

        if (!self->m_audio_sink && !self->m_audio_tracks.is_empty()) {
            self->m_audio_mixer = MUST(AudioMixer::try_create());
            self->m_audio_time_stretch_processor = MUST(AudioTimeStretchProcessor::try_create());
            self->m_audio_sink = MUST(AudioPlaybackSink::try_create(
                [self](PipelineStatus status, PipelineCheckpoint checkpoint) {
                    if (!self)
                        return;
                    self->on_audio_sink_state_changed(status, checkpoint);
                },
                self->m_audio_output_factory));
            MUST(self->m_audio_time_stretch_processor->connect_input(*self->m_audio_mixer));
            MUST(self->m_audio_sink->connect_input(*self->m_audio_time_stretch_processor));
            self->set_clock(*self->m_audio_sink);
            self->m_audio_sink->on_audio_output_error = [self](Error&& error) {
                if (!self)
                    return;
                dbgln("Audio output initialization failed with error: {}", error);
                self->disable_audio();
            };
            if (self->m_started)
                self->m_audio_sink->start();
        }

        AddedTracks added_tracks { .demuxer = demuxer, .audio_tracks = {}, .video_tracks = {} };
        for (size_t i = first_new_audio_index; i < self->m_audio_tracks.size(); i++)
            added_tracks.audio_tracks.append(self->m_audio_tracks[i]);
        for (size_t i = first_new_video_index; i < self->m_video_tracks.size(); i++)
            added_tracks.video_tracks.append(self->m_video_tracks[i]);
        promise->resolve(move(added_tracks));
    });

    return {};
}

NonnullOwnPtr<PlaybackManager> PlaybackManager::create(NonnullRefPtr<AudioOutputFactory> audio_output_factory)
{
    auto playback_manager = adopt_own(*new (nothrow) PlaybackManager(move(audio_output_factory)));
    playback_manager->m_handler = make<StartingStateHandler>(*playback_manager);
    playback_manager->m_handler->on_enter();
    return playback_manager;
}

PlaybackManager::PlaybackManager(NonnullRefPtr<AudioOutputFactory> audio_output_factory)
    : m_weak_link(make_ref_counted<WeakPlaybackManagerLink>(*this))
    , m_audio_output_factory(move(audio_output_factory))
    , m_clock(MUST(MonotonicMediaClock::try_create()))
    , m_time_reader(m_clock->time_reader())
{
}

PlaybackManager::~PlaybackManager()
{
    for (auto const& track_data : m_video_track_datas) {
        track_data.producer->set_error_handler(nullptr);
        track_data.producer->set_read_unblocked_handler(nullptr);
    }
    for (auto const& track_data : m_audio_track_datas) {
        track_data.producer->set_error_handler(nullptr);
        track_data.producer->set_read_unblocked_handler(nullptr);
    }
    m_clock->pause();
    for (auto& track_data : m_video_track_datas) {
        if (track_data.handle.has_value())
            video_sink_registrations().remove(*track_data.handle);
    }
    m_weak_link->revoke({});
}

void PlaybackManager::reject_media_source(WeakPlaybackManager self, NonnullRefPtr<AddMediaSourcePromise>&& promise, Core::EventLoop& main_thread_event_loop, DecoderError error)
{
    if (error.category() == DecoderErrorCategory::EndOfStream)
        error = DecoderError::with_description(DecoderErrorCategory::Corrupted, "The media ended before its metadata could be read"sv);
    main_thread_event_loop.deferred_invoke([self = move(self), promise = move(promise), error = move(error)] mutable {
        if (!self)
            return;
        promise->reject(move(error));
    });
}

NonnullRefPtr<PlaybackManager::AddMediaSourcePromise> PlaybackManager::add_media_source(NonnullRefPtr<MediaStream> const& stream)
{
    auto self = weak();
    auto& main_thread_event_loop = Core::EventLoop::current();
    auto promise = AddMediaSourcePromise::construct();

    // The promise's reference count is not atomic, so it must only be moved to and from the thread.
    auto thread = Threading::Thread::construct("InitMediaSource"sv, [self = move(self), stream, &main_thread_event_loop, promise = promise] mutable -> intptr_t {
        auto demuxer_or_error = create_demuxer(stream);
        if (demuxer_or_error.is_error()) {
            reject_media_source(move(self), move(promise), main_thread_event_loop, demuxer_or_error.release_error());
            return 0;
        }

        auto maybe_error = prepare_playback_from_demuxer(self, demuxer_or_error.release_value(), main_thread_event_loop, promise);
        if (maybe_error.is_error())
            reject_media_source(move(self), move(promise), main_thread_event_loop, maybe_error.release_error());
        return 0;
    });
    thread->start();
    thread->detach();
    return promise;
}

NonnullRefPtr<PlaybackManager::AddMediaSourcePromise> PlaybackManager::add_media_source(NonnullRefPtr<Demuxer> const& demuxer)
{
    auto self = weak();
    auto& main_thread_event_loop = Core::EventLoop::current();
    auto promise = AddMediaSourcePromise::construct();

    // The promise's reference count is not atomic, so it must only be moved to and from the thread.
    auto thread = Threading::Thread::construct("InitMediaSource"sv, [self = move(self), demuxer, &main_thread_event_loop, promise = promise] mutable -> intptr_t {
        auto maybe_error = prepare_playback_from_demuxer(self, demuxer, main_thread_event_loop, promise);
        if (maybe_error.is_error())
            reject_media_source(move(self), move(promise), main_thread_event_loop, maybe_error.release_error());
        return 0;
    });
    thread->start();
    thread->detach();
    return promise;
}

WeakPlaybackManager PlaybackManager::weak()
{
    return WeakPlaybackManager(m_weak_link);
}

void PlaybackManager::set_up_producers()
{
    // The checkpoint is taken at once, so that a settled state reported after the data arrived accounts for it, but
    // it is handed to the producers on this thread, where their tracks may be enabled and disabled.
    auto& main_thread_event_loop = Core::EventLoop::current();
    auto create_read_unblocked_handler = [&] {
        return [this, self = weak(), &main_thread_event_loop] {
            m_latest_checkpoint.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
            main_thread_event_loop.deferred_invoke([self] {
                if (self)
                    self->hand_out_latest_checkpoint();
            });
        };
    };

    for (auto const& video_track_data : m_video_track_datas) {
        video_track_data.producer->set_error_handler([this](DecoderError&& error) {
            dispatch_error(move(error));
        });
        video_track_data.producer->set_read_unblocked_handler(create_read_unblocked_handler());
    }

    for (auto const& audio_track_data : m_audio_track_datas) {
        audio_track_data.producer->set_error_handler([this](DecoderError&& error) {
            dispatch_error(move(error));
        });
        audio_track_data.producer->set_read_unblocked_handler(create_read_unblocked_handler());
    }
}

void PlaybackManager::hand_out_latest_checkpoint()
{
    auto checkpoint = m_latest_checkpoint.load(AK::MemoryOrder::memory_order_relaxed);
    for (auto const& track_data : m_video_track_datas) {
        if (track_data.video_sink != nullptr)
            track_data.producer->set_checkpoint(checkpoint);
    }
    for (auto const& track_data : m_audio_track_datas) {
        if (track_data.enabled)
            track_data.producer->set_checkpoint(checkpoint);
    }
}

AK::Duration PlaybackManager::current_time() const
{
    auto time = m_handler->current_time();
    return min(time, duration());
}

void PlaybackManager::on_audio_sink_state_changed(PipelineStatus status, PipelineCheckpoint checkpoint)
{
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "PlaybackManager({:p}): Audio sink reports {} at checkpoint {}", this, status, checkpoint);
    m_audio_sink_status = status;
    m_audio_sink_checkpoint = checkpoint;
    update_pipeline_state();
}

void PlaybackManager::on_video_sink_state_changed(Track const& track, PipelineStatus status, PipelineCheckpoint checkpoint)
{
    auto& track_data = get_video_data_for_track(track);
    dbgln_if(PLAYBACK_MANAGER_DEBUG, "PlaybackManager({:p}): Video sink for track {} reports {} at checkpoint {}", this, track.identifier(), status, checkpoint);
    track_data.sink_status = status;
    track_data.sink_checkpoint = checkpoint;
    update_pipeline_state();
}

Optional<AK::Duration> PlaybackManager::verified_end_time_for_source(Demuxer const& demuxer) const
{
    auto scan_state = scan_state_for_enabled_tracks(demuxer);
    if (!scan_state.reached_end_of_stream)
        return {};
    auto const& ranges = scan_state.time_ranges;
    if (ranges.is_empty())
        return {};
    return ranges.highest_end_time();
}

PipelineStatus PlaybackManager::combined_pipeline_status() const
{
    auto status = PipelineStatus::EndOfStream;

    if (m_audio_sink != nullptr) {
        status = select_combined_pipeline_status(status, m_audio_sink_status);
    } else {
        for (auto const& track_data : m_audio_track_datas) {
            if (!track_data.enabled)
                continue;
            auto track_status = PipelineStatus::HaveData;
            auto verified_end_time = verified_end_time_for_source(track_data.demuxer);
            if (verified_end_time.has_value() && current_time() >= *verified_end_time)
                track_status = PipelineStatus::EndOfStream;
            status = select_combined_pipeline_status(status, track_status);
        }
    }

    for (auto const& track_data : m_video_track_datas) {
        if (!track_data.handle.has_value())
            continue;
        auto track_status = track_data.sink_status;
        if (track_data.ticking == VideoSinkTicking::No && track_status != PipelineStatus::Error) {
            auto verified_end_time = verified_end_time_for_source(track_data.demuxer);
            if (verified_end_time.has_value() && current_time() >= *verified_end_time)
                track_status = PipelineStatus::EndOfStream;
        }
        status = select_combined_pipeline_status(status, track_status);
    }

    return status;
}

void PlaybackManager::update_pipeline_state()
{
    auto status = combined_pipeline_status();
    if constexpr (PLAYBACK_MANAGER_DEBUG) {
        if (status != m_last_traced_combined_status) {
            m_last_traced_combined_status = status;
            StringBuilder breakdown;
            breakdown.appendff("audio sink {}{}", m_audio_sink_status, m_audio_sink ? "" : " (none)");
            for (auto const& track_data : m_audio_track_datas)
                breakdown.appendff(", audio track {} {}", track_data.track.identifier(), track_data.enabled ? "enabled" : "disabled");
            for (auto const& track_data : m_video_track_datas)
                breakdown.appendff(", video track {} sink {}{}", track_data.track.identifier(), track_data.sink_status, track_data.ticking == VideoSinkTicking::No ? " unticked" : "");
            dbgln("PlaybackManager({:p}): Combined status {} in {} ({})", this, status, m_handler->state(), breakdown.string_view());
        }
    }
    // The state changes that the handler reports must carry whether the new state is settled.
    auto was_settled = m_playback_state_is_settled;
    set_whether_playback_state_is_settled(status);
    m_handler->on_pipeline_status_changed(status);
    dispatch_playback_state_settled_change_if_changed(was_settled);
}

bool PlaybackManager::every_waiting_sink_has_reported_the_latest_checkpoint() const
{
    auto latest_checkpoint = m_latest_checkpoint.load(AK::MemoryOrder::memory_order_relaxed);
    // A sink with data to present cannot be holding a status from before the latest change.
    auto sink_is_behind = [&](PipelineStatus status, PipelineCheckpoint reported_checkpoint) {
        if (status != PipelineStatus::Pending && status != PipelineStatus::Blocked)
            return false;
        return reported_checkpoint < latest_checkpoint;
    };

    if (m_audio_sink != nullptr && sink_is_behind(m_audio_sink_status, m_audio_sink_checkpoint))
        return false;

    for (auto const& track_data : m_video_track_datas) {
        if (track_data.video_sink == nullptr)
            continue;
        if (sink_is_behind(track_data.sink_status, track_data.sink_checkpoint))
            return false;
    }
    return true;
}

void PlaybackManager::set_whether_playback_state_is_settled(PipelineStatus combined_status)
{
    m_playback_state_is_settled = combined_status != PipelineStatus::Pending && every_waiting_sink_has_reported_the_latest_checkpoint();
}

void PlaybackManager::dispatch_playback_state_settled_change_if_changed(bool was_settled)
{
    if (m_playback_state_is_settled != was_settled && on_playback_state_settled_change)
        on_playback_state_settled_change();
}

void PlaybackManager::update_whether_playback_state_is_settled()
{
    auto was_settled = m_playback_state_is_settled;
    set_whether_playback_state_is_settled(combined_pipeline_status());
    dispatch_playback_state_settled_change_if_changed(was_settled);
}

void PlaybackManager::reset_pipeline_state()
{
    for (auto& track_data : m_video_track_datas) {
        if (track_data.video_sink == nullptr)
            continue;
        track_data.sink_status = PipelineStatus::Pending;
    }
    m_audio_sink_status = m_audio_sink != nullptr ? PipelineStatus::Pending : PipelineStatus::HaveData;
}

void PlaybackManager::update_duration_from_scan_states()
{
    auto duration = AK::Duration::zero();
    for (auto const& demuxer : m_demuxers)
        duration = max(duration, demuxer->scan_state().duration);
    check_for_demuxed_duration_change(duration);
}

void PlaybackManager::check_for_demuxed_duration_change(AK::Duration duration)
{
    if (m_duration_is_set_by_owner)
        return;
    duration = max(duration, m_timeline.earliest_possible_position);
    if (m_timeline.duration == duration)
        return;
    m_timeline.duration = duration;
    if (on_duration_change)
        on_duration_change(m_timeline.duration);
}

void PlaybackManager::set_duration(AK::Duration duration)
{
    m_duration_is_set_by_owner = true;
    if (m_timeline.duration == duration)
        return;
    m_timeline.duration = duration;
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
    if (m_host_hooks.on_clock_changed)
        m_host_hooks.on_clock_changed(m_time_reader);
    for (auto& track_data : m_video_track_datas) {
        if (!track_data.video_sink)
            continue;
        track_data.video_sink->set_time_reader(m_time_reader);
    }
    clock->set_playback_rate(m_playback_rate);
    if (is_playing())
        clock->resume();
}

void PlaybackManager::disable_audio()
{
    m_audio_mixer = nullptr;
    m_audio_time_stretch_processor = nullptr;
    m_audio_sink = nullptr;
    set_clock(MUST(MonotonicMediaClock::try_create()));
    on_audio_sink_state_changed(PipelineStatus::EndOfStream, 0);
}

void PlaybackManager::attach_video_sink(VideoTrackData& track_data, NonnullRefPtr<VideoSink> video_sink)
{
    VERIFY(track_data.video_sink == nullptr);
    auto track = track_data.track;
    video_sink->set_state_change_handler([self = weak(), track](PipelineStatus status, PipelineCheckpoint checkpoint) {
        if (!self)
            return;
        self->on_video_sink_state_changed(track, status, checkpoint);
    });
    video_sink->set_resize_handler([self = weak(), track](Gfx::Size<u32> size) {
        if (!self)
            return;
        auto& track_data = self->get_video_data_for_track(track);
        if (track_data.on_resize)
            track_data.on_resize(size);
    });
    track_data.producer->set_checkpoint(m_latest_checkpoint.load(AK::MemoryOrder::memory_order_relaxed));
    MUST(video_sink->connect_input(track_data.producer));
    if (track_data.ticking != VideoSinkTicking::Undecided)
        video_sink->set_ticking(track_data.ticking == VideoSinkTicking::Yes);
    track_data.video_sink = move(video_sink);
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

VideoSinkHandle PlaybackManager::reserve_video_sink_handle(Track const& track)
{
    auto handle = allocate_video_sink_handle();
    reserve_video_sink_handle(track, handle, ResumeEndedPlayback::No);
    return handle;
}

void PlaybackManager::reserve_video_sink_handle(Track const& track, VideoSinkHandle handle, ResumeEndedPlayback resume_ended_playback)
{
    auto& track_data = get_video_data_for_track(track);
    if (track_data.handle.has_value())
        disable_video_sink_by_handle(*track_data.handle);
    track_data.handle = handle;
    track_data.ticking = VideoSinkTicking::Undecided;
    video_sink_registrations().set(handle, this);
    apply_track_change_to_ended_state(resume_ended_playback);
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

void PlaybackManager::set_host_hooks(HostHooks hooks)
{
    m_host_hooks = move(hooks);
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
        disconnect_video_sink(*track_data);
        track_data->sink_status = PipelineStatus::HaveData;
    }
    track_data->handle = {};
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

void PlaybackManager::disconnect_video_sink(VideoTrackData& track_data)
{
    track_data.video_sink->set_state_change_handler(nullptr);
    track_data.video_sink->set_resize_handler(nullptr);
    track_data.video_sink->disconnect_input(track_data.producer);
    track_data.video_sink = nullptr;
    track_data.video_edge_sink = nullptr;
}

void PlaybackManager::set_video_sink_ticking(VideoSinkHandle handle, bool ticking)
{
    auto* manager = video_sink_registrations().get(handle).value_or(nullptr);
    if (!manager)
        return;
    auto& track_data = manager->get_video_data_for_handle(handle);
    auto new_ticking = ticking ? VideoSinkTicking::Yes : VideoSinkTicking::No;
    if (track_data.ticking == new_ticking)
        return;
    track_data.ticking = new_ticking;
    if (track_data.video_sink)
        track_data.video_sink->set_ticking(ticking);
    manager->update_pipeline_state();
}

void PlaybackManager::detach_video_sink(VideoSinkHandle handle)
{
    auto* track_data = find_video_data_for_handle(handle);
    if (!track_data)
        return;
    if (track_data->video_sink)
        disconnect_video_sink(*track_data);
    track_data->sink_status = PipelineStatus::Pending;
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

bool PlaybackManager::has_video_sink_handle(Badge<VideoPresentationServerConnection>, VideoSinkHandle handle)
{
    return video_sink_registrations().contains(handle);
}

ErrorOr<PlaybackManager::RemoteVideoEdge> PlaybackManager::create_video_edge(Badge<VideoPresentationServerConnection>, VideoSinkHandle handle, RemoteVideoSink::Delegates delegates)
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

void PlaybackManager::attach_video_edge(Badge<VideoPresentationServerConnection>, VideoSinkHandle handle, NonnullRefPtr<RemoteVideoSink> const& pump)
{
    auto* manager = video_sink_registrations().get(handle).value_or(nullptr);
    if (!manager)
        return;
    auto& track_data = manager->get_video_data_for_handle(handle);
    // The replaced sink's last status stands until the new one reports, so replacing does not interrupt buffering.
    if (track_data.video_sink != nullptr)
        disconnect_video_sink(track_data);
    manager->attach_video_sink(track_data, pump);
    track_data.video_edge_sink = pump;
}

void PlaybackManager::release_video_edge(Badge<VideoPresentationServerConnection>, VideoSinkHandle handle, VideoSink const& released_sink)
{
    auto* manager = video_sink_registrations().get(handle).value_or(nullptr);
    if (!manager)
        return;
    auto* track_data = manager->find_video_data_for_handle(handle);
    if (!track_data || track_data->video_sink != &released_sink)
        return;
    manager->detach_video_sink(handle);
}

void PlaybackManager::enable_an_audio_track(Track const& track, ResumeEndedPlayback resume_ended_playback)
{
    auto& track_data = get_audio_data_for_track(track);
    VERIFY(!track_data.enabled);
    m_audio_sink_status = m_audio_sink != nullptr ? PipelineStatus::Pending : PipelineStatus::HaveData;
    track_data.producer->set_checkpoint(m_latest_checkpoint.load(AK::MemoryOrder::memory_order_relaxed));
    if (m_audio_mixer) {
        MUST(m_audio_mixer->connect_input(track_data.producer));
        m_audio_sink->reseek_input_keeping_clock();
    }
    track_data.enabled = true;
    apply_track_change_to_ended_state(resume_ended_playback);
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

void PlaybackManager::disable_an_audio_track(Track const& track)
{
    auto& track_data = get_audio_data_for_track(track);
    VERIFY(track_data.enabled);
    m_audio_sink_status = m_audio_sink != nullptr ? PipelineStatus::Pending : PipelineStatus::HaveData;
    if (m_audio_mixer) {
        m_audio_mixer->disconnect_input(track_data.producer);
        m_audio_sink->reseek_input_keeping_clock();
    }
    track_data.enabled = false;
    update_pipeline_state();
    dispatch_buffered_ranges_change();
}

void PlaybackManager::apply_track_change_to_ended_state(ResumeEndedPlayback resume_ended_playback)
{
    if (m_handler->state() != PlaybackState::Ended)
        return;
    auto& ended_state_handler = static_cast<EndedStateHandler&>(*m_handler);
    if (resume_ended_playback == ResumeEndedPlayback::Yes)
        ended_state_handler.resume_from_position_before_end();
    else
        ended_state_handler.move_pipeline_to_end();
}

void PlaybackManager::seek_tracks_with_invalidated_data(Demuxer const& demuxer, TimeRanges const& invalidated_ranges)
{
    auto current_timestamp = current_time();
    auto pipeline_may_hold_invalidated_data = invalidated_ranges.highest_end_time() > current_timestamp;
    if (!pipeline_may_hold_invalidated_data)
        return;

    for (auto& track_data : m_video_track_datas) {
        if (track_data.demuxer.ptr() != &demuxer || track_data.video_sink == nullptr)
            continue;
        track_data.video_sink->seek(current_timestamp, InvalidateHeldData::Yes);
    }

    auto has_enabled_audio_track = any_of(m_audio_track_datas, [&](auto const& track_data) {
        return track_data.demuxer.ptr() == &demuxer && track_data.enabled;
    });
    if (has_enabled_audio_track && m_audio_sink != nullptr)
        m_audio_sink->reseek_input_keeping_clock();
}

void PlaybackManager::seek_clock_and_video_sinks(AK::Duration timestamp)
{
    m_clock->seek(timestamp);

    for (auto& track_data : m_video_track_datas) {
        if (track_data.video_sink == nullptr)
            continue;
        track_data.video_sink->seek(timestamp);
    }
}

void PlaybackManager::resume_clock_and_notify_video_sinks()
{
    m_clock->resume();
    notify_video_sinks_of_clock_resumption();
}

void PlaybackManager::notify_video_sinks_of_clock_resumption()
{
    for (auto& track_data : m_video_track_datas) {
        if (track_data.video_sink == nullptr)
            continue;
        track_data.video_sink->notify_clock_resumed();
    }
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
    m_started = true;
    if (m_audio_sink)
        m_audio_sink->start();
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

Optional<PlaybackManager::SeekTarget> PlaybackManager::decide_seek_target(AK::Duration timestamp, SeekMode mode) const
{
    // https://html.spec.whatwg.org/multipage/media.html#dom-media-seek
    // NB: The timestamp variable stands in for the new playback position.
    //     These are the seek steps that run in parallel.

    // 6. If the new playback position is later than the end of the media resource, then let it be the end of the media
    //    resource instead.
    timestamp = min(timestamp, duration());

    // 7. If the new playback position is less than the earliest possible position, let it be that position instead.
    timestamp = max(timestamp, earliest_possible_position());

    // 8. If the (possibly now changed) new playback position is not in one of the ranges given in the seekable
    //    attribute, then let it be the position in one of the ranges given in the seekable attribute that is the
    //    nearest to the new playback position. If two positions both satisfy that constraint (i.e. the new playback
    //    position is exactly in the middle between two ranges in the seekable attribute), then use the position that
    //    is closest to the current playback position. If there are no ranges given in the seekable attribute, then set
    //    the seeking IDL attribute to false and return.
    auto seekable = seekable_time_ranges();
    if (!seekable.has_value())
        return OptionalNone();

    // NB: We only have one range, so no tie-breaker is needed, and we can just clamp to it.
    timestamp = max(timestamp, seekable.value().start);
    timestamp = min(timestamp, seekable.value().end);

    // 9. If the approximate-for-speed flag is set, adjust the new playback position to a value that will allow for
    //    playback to resume promptly. If new playback position before this step is before current playback position,
    //    then the adjusted new playback position must also be before the current playback position. Similarly, if the
    //    new playback position before this step is after current playback position, then the adjusted new playback
    //    position must also be after the current playback position.
    if (mode == SeekMode::ApproximateForSpeed) {
        auto current_playback_position = current_time();
        if (timestamp < current_playback_position)
            mode = Media::SeekMode::FastBefore;
        else if (timestamp > current_playback_position)
            mode = Media::SeekMode::FastAfter;
        else
            mode = Media::SeekMode::Accurate;
    }

    return SeekTarget { timestamp, mode };
}

void PlaybackManager::seek(AK::Duration timestamp, SeekMode mode)
{
    auto target = decide_seek_target(timestamp, mode);
    if (!target.has_value()) {
        dbgln_if(PLAYBACK_MANAGER_DEBUG, "PlaybackManager({:p}): Seek was skipped", this);
        if (on_seek_decided)
            on_seek_decided(SeekDecision::Skip);
        return;
    }
    if (on_seek_decided)
        on_seek_decided(SeekDecision::Seek);
    timestamp = target.value().timestamp;
    mode = target.value().mode;

    dbgln_if(PLAYBACK_MANAGER_DEBUG, "PlaybackManager({:p}): Seek to {} ({}) from {}", this, timestamp, mode, m_handler->state());
    m_handler->seek(timestamp, mode);
    m_is_in_error_state = false;

    // Defer the pipeline update so that we don't synchronously resolve seeks.
    Core::deferred_invoke([self = weak()] {
        if (!self)
            return;
        self->update_pipeline_state();
    });
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

// https://w3c.github.io/media-source/#htmlmediaelement-extensions-seekable
// NB: The spec defines these ranges for a media element with a MediaSource attached; they apply to every source alike.
Optional<TimeRanges::Range> PlaybackManager::seekable_time_range(TimeRanges const& buffered, MediaTimeline const& timeline)
{
    // AD-HOC: The ranges start at the earliest possible position instead of 0. A MediaSource's is always 0, its
    //         presentation start time.
    auto start = timeline.earliest_possible_position;

    // -> If recent duration equals positive Infinity:
    if (timeline.duration == AK::Duration::max()) {
        // 1. If recent live seekable range is not empty:
        if (timeline.live_seekable_range.has_value()) {
            // 1. Let union ranges be the union of recent live seekable range and the HTMLMediaElement's buffered
            //    attribute.
            // 2. Return a single range with a start time equal to the earliest start time in union ranges and an end
            //    time equal to the highest end time in union ranges and abort these steps.
            auto union_start = timeline.live_seekable_range->start;
            auto union_end = timeline.live_seekable_range->end;
            if (!buffered.is_empty()) {
                union_start = min(union_start, buffered[0].start);
                union_end = max(union_end, buffered.highest_end_time());
            }
            return TimeRanges::Range { union_start, union_end };
        }

        // 2. If the HTMLMediaElement's buffered attribute returns an empty TimeRanges object, then return an empty
        //    TimeRanges object and abort these steps.
        if (buffered.is_empty())
            return {};

        // 3. Return a single range with a start time of 0 and an end time equal to the highest end time reported by the
        //    HTMLMediaElement's buffered attribute.
        return TimeRanges::Range { start, buffered.highest_end_time() };
    }

    // -> Otherwise:
    //    Return a single range with a start time of 0 and an end time equal to recent duration.
    return TimeRanges::Range { start, timeline.duration };
}

BufferedRanges PlaybackManager::buffered_time_ranges_of_sources(ReadonlySpan<CombinedScanState> enabled_sources, MediaTimeline const& timeline)
{
    auto buffered = DemuxerScanState::buffered_ranges_of_sources(enabled_sources);
    // Demuxers may report data from the start of the resource as buffered from zero, before the earliest possible
    // position.
    auto seekable = seekable_time_range(buffered.time_ranges, timeline);
    if (seekable.has_value())
        buffered.time_ranges = buffered.time_ranges.intersection(TimeRanges { seekable.value() });
    return buffered;
}

Vector<CombinedScanState, 2> PlaybackManager::enabled_source_scan_states() const
{
    Vector<CombinedScanState, 2> enabled_sources;
    for (auto const& demuxer : m_demuxers) {
        if (source_has_enabled_track(demuxer))
            enabled_sources.append(scan_state_for_enabled_tracks(demuxer));
    }
    return enabled_sources;
}

BufferedRanges PlaybackManager::buffered_time_ranges() const
{
    return buffered_time_ranges_of_sources(enabled_source_scan_states(), m_timeline);
}

Optional<TimeRanges::Range> PlaybackManager::seekable_time_ranges() const
{
    auto buffered = DemuxerScanState::buffered_ranges_of_sources(enabled_source_scan_states());
    return seekable_time_range(buffered.time_ranges, m_timeline);
}

void PlaybackManager::set_live_seekable_range(Optional<TimeRanges::Range> range)
{
    m_timeline.live_seekable_range = range;
    dispatch_buffered_ranges_change();
}

CombinedScanState PlaybackManager::scan_state_for_enabled_tracks(Demuxer const& demuxer) const
{
    return demuxer.scan_state().combined_scan_state([&](u64 identifier) {
        for (auto const& data : m_audio_track_datas) {
            if (data.demuxer.ptr() == &demuxer && data.track.identifier() == identifier)
                return data.enabled;
        }
        for (auto const& data : m_video_track_datas) {
            if (data.demuxer.ptr() == &demuxer && data.track.identifier() == identifier)
                return data.handle.has_value();
        }
        return false;
    });
}

bool PlaybackManager::source_has_enabled_track(Demuxer const& demuxer) const
{
    for (auto const& track_data : m_video_track_datas) {
        if (track_data.demuxer.ptr() == &demuxer && track_data.handle.has_value())
            return true;
    }
    for (auto const& track_data : m_audio_track_datas) {
        if (track_data.demuxer.ptr() == &demuxer && track_data.enabled)
            return true;
    }
    return false;
}

void PlaybackManager::set_volume(double volume)
{
    if (m_audio_sink)
        m_audio_sink->set_volume(volume);
}

void PlaybackManager::set_playback_rate(float rate)
{
    auto clock_was_stopped_by_the_rate = m_playback_rate == 0.0f;
    m_playback_rate = rate;
    m_clock->set_playback_rate(rate);
    if (clock_was_stopped_by_the_rate && rate != 0.0f && m_handler->is_playing())
        notify_video_sinks_of_clock_resumption();
    update_pipeline_state();
}

}
