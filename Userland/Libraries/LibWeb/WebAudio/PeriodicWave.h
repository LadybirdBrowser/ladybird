/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#PeriodicWaveConstraints
struct PeriodicWaveConstraints {
    bool disable_normalization { false };
};

// https://webaudio.github.io/web-audio-api/#PeriodicWaveOptions
struct PeriodicWaveOptions : PeriodicWaveConstraints {
    Optional<Vector<float>> real;
    Optional<Vector<float>> imag;
};

// https://webaudio.github.io/web-audio-api/#PeriodicWave
class PeriodicWave : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(PeriodicWave, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(PeriodicWave);

public:
    static WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, PeriodicWaveOptions const&);

    virtual ~PeriodicWave() override;

protected:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
};

}
