/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebAudio/PeriodicWave.h>

namespace Web::WebAudio {

extern "C" {
JSCompletion web_rust_periodic_wave_create(JSVM*, void* realm, bool, ReadonlySpan<float>, bool, ReadonlySpan<float>, bool, void**);
void const* web_rust_periodic_wave_render_data(void* cell, bool* normalize);
}

WebIDL::ExceptionOr<GC::Ref<PeriodicWave>> PeriodicWave::create_for_constructor(GC::Ref<BaseAudioContext>, PeriodicWaveOptions const& options)
{
    auto& vm = JS::VM::the();
    void* wave = nullptr;
    TRY(JS::HostABI::completion_from_abi<void>(web_rust_periodic_wave_create(JS::HostABI::vm_to_abi(vm), vm.current_realm(), options.real.has_value(), options.real.has_value() ? options.real->span() : ReadonlySpan<float> {}, options.imag.has_value(), options.imag.has_value() ? options.imag->span() : ReadonlySpan<float> {}, options.disable_normalization, &wave)));
    return GC::Ref { *static_cast<PeriodicWave*>(wave) };
}

NonnullRefPtr<Rendering::PeriodicWaveData> PeriodicWave::render_data() const
{
    bool normalize;
    auto* data = web_rust_periodic_wave_render_data(const_cast<PeriodicWave*>(this), &normalize);
    return make_ref_counted<Rendering::PeriodicWaveData>(data, normalize);
}

}
