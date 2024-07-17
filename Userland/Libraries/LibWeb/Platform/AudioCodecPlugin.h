/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FixedArray.h>
#include <AK/Function.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <LibAudio/Forward.h>

namespace Web::Platform {

class AudioCodecPlugin {
public:
    using AudioCodecPluginCreator = Function<ErrorOr<NonnullOwnPtr<AudioCodecPlugin>>(NonnullRefPtr<Audio::Loader>)>;

    static void install_creation_hook(AudioCodecPluginCreator);
    static ErrorOr<NonnullOwnPtr<AudioCodecPlugin>> create(NonnullRefPtr<Audio::Loader>);

    virtual ~AudioCodecPlugin();

    static ErrorOr<FixedArray<Audio::Sample>> read_samples_from_loader(Audio::Loader&, size_t samples_to_load);
    static AK::Duration set_loader_position(Audio::Loader&, double position, AK::Duration duration);
    static AK::Duration current_loader_position(Audio::Loader const&);

    virtual void resume_playback() = 0;
    virtual void pause_playback() = 0;
    virtual void set_volume(double) = 0;
    virtual void seek(double) = 0;

    virtual AK::Duration duration() = 0;

    Function<void(AK::Duration)> on_playback_position_updated;
    Function<void(String)> on_decoder_error;

protected:
    AudioCodecPlugin();
};

}
