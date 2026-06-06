/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PeriodicWave.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#PeriodicWave
class PeriodicWave : public Bindings::Wrappable {
    WEB_WRAPPABLE(PeriodicWave, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(PeriodicWave);

public:
    static WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> construct_impl(GC::Ref<BaseAudioContext>, Bindings::PeriodicWaveOptions const&);

    PeriodicWave();
    virtual ~PeriodicWave() override;

protected:
    virtual size_t external_memory_size() const override;

private:
    Vector<float> m_real;
    Vector<float> m_imag;
    bool m_normalize { true };
};

}
