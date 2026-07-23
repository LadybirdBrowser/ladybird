/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebAudio/Rendering/AudioData.h>

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#PeriodicWave
class PeriodicWave : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(PeriodicWave, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(PeriodicWave);

public:
    static WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> construct_impl(JS::Realm&, GC::Ref<BaseAudioContext>, Bindings::PeriodicWaveOptions const&);

    explicit PeriodicWave(JS::Realm&);
    virtual ~PeriodicWave() override;

    NonnullRefPtr<Rendering::PeriodicWaveData> render_data() const;

protected:
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

private:
    GC::Ptr<JS::Float32Array> m_real;
    GC::Ptr<JS::Float32Array> m_imag;
    bool m_normalize { true };
};

}
