/*
 * Copyright (c) 2025, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/PlaybackManager.h>
#include <LibMedia/PlaybackStates/Forward.h>
#include <LibMedia/PlaybackStates/ResumingStateHandler.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/Providers/VideoDataProvider.h>
#include <LibMedia/SeekMode.h>
#include <LibMedia/Sinks/AudioMixingSink.h>
#include <LibMedia/Sinks/DisplayingVideoSink.h>

namespace Media {

class SeekingStateHandler final : public ResumingStateHandler {
public:
    SeekingStateHandler(PlaybackManager& manager, bool playing, AK::Duration timestamp, SeekMode mode)
        : ResumingStateHandler(manager, playing)
        , m_target_timestamp(timestamp)
        , m_mode(mode)
    {
    }
    virtual ~SeekingStateHandler() override = default;

    virtual void on_enter() override
    {
        ResumingStateHandler::on_enter();
        begin_seek();
    }

    virtual void on_exit() override
    {
        for (auto const& track : m_tracks_enabled_while_seeking)
            PlaybackStateHandler::on_track_enabled(track);
    }

    virtual void seek(AK::Duration timestamp, SeekMode mode) override
    {
        m_target_timestamp = timestamp;
        m_mode = mode;
        begin_seek();
    }

    virtual PlaybackState state() override
    {
        return PlaybackState::Seeking;
    }

    virtual void enter_buffering() override { }
    virtual void exit_buffering() override { }

    virtual void on_track_enabled(Track const& track) override
    {
        m_tracks_enabled_while_seeking.append(track);
    }

private:
    struct SeekData : public RefCounted<SeekData> {
        SeekData(PlaybackManager& manager)
            : manager(manager)
        {
        }

        NonnullRefPtr<PlaybackManager> manager;

        size_t id { 0 };

        AK::Duration chosen_timestamp { AK::Duration::zero() };

        size_t video_seeks_in_flight { 0 };
        size_t video_seeks_completed { 0 };

        size_t audio_seeks_in_flight { 0 };
        size_t audio_seeks_completed { 0 };
    };

    static void possibly_complete_seek(SeekData& seek_data)
    {
        if (seek_data.video_seeks_completed != seek_data.video_seeks_in_flight)
            return;
        if (seek_data.audio_seeks_completed != seek_data.audio_seeks_in_flight)
            return;

        auto& seek_handler = as<SeekingStateHandler>(*seek_data.manager->m_handler);

        // Providers guarantee that their callbacks don't get called if a new seek is started, but we
        // can end up with video seeks in flight while an audio seek is completing. Ensure that the
        // old audio seek doesn't cause us to exit the seeking state before the current seek completes.
        if (seek_handler.m_current_seek_id != seek_data.id)
            return;

        seek_data.manager->m_time_provider->set_time(seek_data.chosen_timestamp);

        for (auto& video_track_data : seek_data.manager->m_video_track_datas) {
            if (video_track_data.display == nullptr)
                continue;
            video_track_data.display->resume_updates();
        }

        seek_handler.resume();
    }

    static size_t count_audio_tracks(PlaybackManager& manager)
    {
        size_t count = 0;
        for (auto const& audio_track_data : manager.m_audio_track_datas) {
            if (manager.m_audio_sink->provider(audio_track_data.track) == nullptr)
                continue;
            count++;
        }
        return count;
    }

    static void begin_audio_seeks(SeekData& seek_data)
    {
        seek_data.audio_seeks_in_flight = count_audio_tracks(seek_data.manager);

        if (seek_data.audio_seeks_in_flight == 0) {
            possibly_complete_seek(seek_data);
            return;
        }

        for (auto const& audio_track_data : seek_data.manager->m_audio_track_datas) {
            if (seek_data.manager->m_audio_sink->provider(audio_track_data.track) == nullptr)
                continue;
            audio_track_data.provider->seek(seek_data.chosen_timestamp, [seek_data = NonnullRefPtr(seek_data)]() {
                seek_data->audio_seeks_completed++;
                possibly_complete_seek(seek_data);
            });
        }
    }

    void begin_seek()
    {
        auto seek_data = make_ref_counted<SeekData>(manager());
        seek_data->id = ++m_current_seek_id;

        for (auto const& video_track_data : manager().m_video_track_datas) {
            if (video_track_data.display == nullptr)
                continue;
            seek_data->video_seeks_in_flight++;
            video_track_data.display->pause_updates();
        }

        seek_data->audio_seeks_in_flight = count_audio_tracks(manager());

        if (m_mode == SeekMode::Accurate || seek_data->video_seeks_in_flight == 0) {
            seek_data->chosen_timestamp = m_target_timestamp;
            begin_audio_seeks(seek_data);
            if (m_mode != SeekMode::Accurate)
                return;
        }

        for (auto const& video_track_data : manager().m_video_track_datas) {
            if (video_track_data.display == nullptr)
                continue;
            video_track_data.provider->seek(m_target_timestamp, m_mode, [seek_data, seek_mode = m_mode](AK::Duration provider_timestamp) {
                seek_data->chosen_timestamp = max(seek_data->chosen_timestamp, provider_timestamp);
                seek_data->video_seeks_completed++;

                if (seek_mode == SeekMode::Accurate)
                    possibly_complete_seek(seek_data);
                else if (seek_data->video_seeks_completed == seek_data->video_seeks_in_flight)
                    begin_audio_seeks(seek_data);
            });
        }
    }

    AK::Duration m_target_timestamp;
    SeekMode m_mode { SeekMode::Accurate };
    size_t m_current_seek_id { 0 };
    Vector<Track> m_tracks_enabled_while_seeking;
};

}
