/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibAudio/PlaybackStream.h>
#include <LibWeb/Platform/AudioCodecPlugin.h>

namespace Web::Platform {

class AudioCodecPluginAgnostic final : public AudioCodecPlugin {
public:
    static ErrorOr<NonnullOwnPtr<AudioCodecPluginAgnostic>> create(NonnullRefPtr<Audio::Loader> const&);

    virtual void resume_playback() override;
    virtual void pause_playback() override;
    virtual void set_volume(double) override;
    virtual void seek(double) override;

    virtual AK::Duration duration() override;

private:
    explicit AudioCodecPluginAgnostic(NonnullRefPtr<Audio::Loader> loader, AK::Duration, NonnullRefPtr<Core::Timer> update_timer);

    void update_timestamp();

    NonnullRefPtr<Audio::Loader> m_loader;
    RefPtr<Audio::PlaybackStream> m_output { nullptr };
    AK::Duration m_duration { AK::Duration::zero() };
    AK::Duration m_last_resume_in_media_time { AK::Duration::zero() };
    AK::Duration m_last_resume_in_device_time { AK::Duration::zero() };
    AK::Duration m_last_good_device_time { AK::Duration::zero() };
    Core::EventLoop& m_main_thread_event_loop;
    NonnullRefPtr<Core::Timer> m_update_timer;
    bool m_paused { true };
};

}
