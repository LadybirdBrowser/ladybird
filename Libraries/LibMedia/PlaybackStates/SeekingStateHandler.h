/*
 * Copyright (c) 2025-2026, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashTable.h>
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
        VERIFY(m_video_seeks_pending.is_empty());
        VERIFY(m_audio_seeks_pending.is_empty());
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
    virtual AvailableData available_data() override
    {
        return AvailableData::Current;
    }

    virtual void enter_buffering() override { }
    virtual void exit_buffering() override { }

    virtual void on_track_enabled(Track const& track) override
    {
        if (track.type() == TrackType::Video) {
            start_video_seek(track);
            return;
        }

        VERIFY(track.type() == TrackType::Audio);
        if (m_mode == SeekMode::Accurate || m_audio_seeks_started)
            start_audio_seek(track);
    }

    virtual void on_track_disabled(Track const& track) override
    {
        if (track.type() == TrackType::Video) {
            auto& track_data = manager().get_video_data_for_track(track);
            track_data.provider->seek(m_target_timestamp, SeekMode::Accurate, nullptr);
            end_video_seek(track);
            return;
        }

        VERIFY(track.type() == TrackType::Audio);
        auto& track_data = manager().get_audio_data_for_track(track);
        track_data.provider->seek(m_target_timestamp, nullptr);
        end_audio_seek(track);
    }

private:
    void possibly_complete_seek()
    {
        if (!m_video_seeks_pending.is_empty())
            return;

        if (!m_audio_seeks_started) {
            begin_audio_seeks();
            return;
        }

        if (!m_audio_seeks_pending.is_empty())
            return;

        manager().m_time_provider->set_time(m_chosen_timestamp);

        for (auto& video_track_data : manager().m_video_track_datas) {
            if (video_track_data.display == nullptr)
                continue;
            video_track_data.display->resume_updates();
        }

        resume();
    }

    void start_video_seek(Track const& track)
    {
        auto& track_data = manager().get_video_data_for_track(track);
        if (track_data.display == nullptr)
            return;

        m_video_seeks_pending.set(track);
        track_data.display->pause_updates();
        track_data.provider->seek(m_target_timestamp, m_mode, [this, weak_manager = manager().weak(), track](AK::Duration provider_timestamp) {
            if (!weak_manager)
                return;
            m_chosen_timestamp = max(m_chosen_timestamp, provider_timestamp);
            end_video_seek(track);
        });
    }

    void end_video_seek(Track const& track)
    {
        m_video_seeks_pending.remove(track);
        possibly_complete_seek();
    }

    void start_audio_seek(Track const& track)
    {
        if (!manager().m_audio_sink)
            return;

        auto& track_data = manager().get_audio_data_for_track(track);
        if (!track_data.enabled)
            return;

        m_audio_seeks_pending.set(track);

        track_data.provider->seek(m_chosen_timestamp, [this, weak_manager = manager().weak(), track]() {
            if (!weak_manager)
                return;
            end_audio_seek(track);
        });
    }

    void end_audio_seek(Track const& track)
    {
        m_audio_seeks_pending.remove(track);
        possibly_complete_seek();
    }

    void begin_audio_seeks()
    {
        VERIFY(!m_audio_seeks_started);
        m_audio_seeks_started = true;

        for (auto const& track : manager().audio_tracks())
            start_audio_seek(track);

        if (m_audio_seeks_pending.is_empty()) {
            possibly_complete_seek();
            return;
        }
    }

    void begin_seek()
    {
        m_chosen_timestamp = AK::Duration::zero();
        m_audio_seeks_started = false;
        m_video_seeks_pending.clear();
        m_audio_seeks_pending.clear();

        for (auto const& track : manager().video_tracks())
            start_video_seek(track);

        if (m_mode == SeekMode::Accurate) {
            m_chosen_timestamp = m_target_timestamp;
            begin_audio_seeks();
        } else {
            possibly_complete_seek();
        }
    }

    AK::Duration m_target_timestamp;
    SeekMode m_mode { SeekMode::Accurate };
    AK::Duration m_chosen_timestamp { AK::Duration::zero() };
    bool m_audio_seeks_started { false };
    HashTable<Track> m_video_seeks_pending;
    HashTable<Track> m_audio_seeks_pending;
};

}
