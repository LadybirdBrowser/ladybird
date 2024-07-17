/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibAudio/Forward.h>
#include <LibWeb/Platform/AudioCodecPlugin.h>
#include <QObject>

namespace Ladybird {

class AudioThread;

class AudioCodecPluginQt final
    : public QObject
    , public Web::Platform::AudioCodecPlugin {
    Q_OBJECT

public:
    static ErrorOr<NonnullOwnPtr<AudioCodecPluginQt>> create(NonnullRefPtr<Audio::Loader>);
    virtual ~AudioCodecPluginQt() override;

    virtual void resume_playback() override;
    virtual void pause_playback() override;
    virtual void set_volume(double) override;
    virtual void seek(double) override;

    virtual AK::Duration duration() override;

private:
    explicit AudioCodecPluginQt(NonnullOwnPtr<AudioThread>);

    NonnullOwnPtr<AudioThread> m_audio_thread;
};

}
