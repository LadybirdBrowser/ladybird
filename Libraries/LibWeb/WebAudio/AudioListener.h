/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/AudioListener.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/WebAudio/AudioParam.h>

namespace Web::WebAudio {

class AudioListener final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(AudioListener, audio_listener)

public:
    static GC::Ref<AudioListener> create(GC::Ref<BaseAudioContext>);
    GC::Ref<AudioParam> forward_x() const;
    GC::Ref<AudioParam> forward_y() const;
    GC::Ref<AudioParam> forward_z() const;
    GC::Ref<AudioParam> position_x() const;
    GC::Ref<AudioParam> position_y() const;
    GC::Ref<AudioParam> position_z() const;
    GC::Ref<AudioParam> up_x() const;
    GC::Ref<AudioParam> up_y() const;
    GC::Ref<AudioParam> up_z() const;
};

}
