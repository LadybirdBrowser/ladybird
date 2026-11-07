/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PeriodicWave.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/WebAudio/Rendering/AudioData.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

class BaseAudioContext;

using PeriodicWaveConstraints = Bindings::PeriodicWaveConstraints;
struct PeriodicWaveOptions : PeriodicWaveConstraints {
    Optional<Vector<float>> real;
    Optional<Vector<float>> imag;
};

// https://webaudio.github.io/web-audio-api/#PeriodicWave
class PeriodicWave : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(PeriodicWave, periodic_wave)

public:
    static WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> create_for_constructor(GC::Ref<BaseAudioContext>, PeriodicWaveOptions const&);
    NonnullRefPtr<Rendering::PeriodicWaveData> render_data() const;
};

}
