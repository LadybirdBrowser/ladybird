/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/AudioParamMap.h>

namespace Web::WebAudio {

extern "C" {
void* web_rust_audio_param_map_create(JSVM*);
void web_rust_audio_param_map_set_entry(JSVM*, void* map, FlatPtr key, void* param);
void web_rust_audio_param_map_values(void* map, void* context, void (*callback)(void*, void*));
}
GC::Ref<AudioParamMap> AudioParamMap::create()
{
    return *static_cast<AudioParamMap*>(web_rust_audio_param_map_create(JS::HostABI::vm_to_abi(JS::VM::the())));
}
void AudioParamMap::set_entry(Utf16FlyString key, GC::Ref<AudioParam> value)
{
    web_rust_audio_param_map_set_entry(JS::HostABI::vm_to_abi(JS::VM::the()), this, key.raw_identity(), value.ptr());
}
GC::RootVector<GC::Ref<AudioParam>> AudioParamMap::values() const
{
    GC::RootVector<GC::Ref<AudioParam>> values;
    web_rust_audio_param_map_values(const_cast<AudioParamMap*>(this), &values, [](void* context, void* param) {
        static_cast<GC::RootVector<GC::Ref<AudioParam>>*>(context)->append(*static_cast<AudioParam*>(param));
    });
    return values;
}

}
