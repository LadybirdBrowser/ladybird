/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>

namespace Web::WebAudio {

extern "C" {
void* web_rust_audio_param_create(JSVM*, void* context, void* owner, float default_value, float min_value, float max_value, u8 automation_rate, u8 fixed_rate);
void* web_rust_audio_param_context(void*);
JSCompletion web_rust_audio_param_set_value(JSVM*, void* realm, void* param, float value);
float web_rust_audio_param_intrinsic_value(void*, double time);
void const* web_rust_audio_param_render_param(void*);
}

GC::Ref<AudioParam> AudioParam::create(GC::Ref<BaseAudioContext> context, GC::Ptr<AudioNode> owner, float default_value, float min_value, float max_value, AutomationRate automation_rate, FixedAutomationRate fixed_rate)
{
    return *static_cast<AudioParam*>(web_rust_audio_param_create(JS::HostABI::vm_to_abi(JS::VM::the()), context.ptr(), owner.ptr(), default_value, min_value, max_value, to_underlying(automation_rate), to_underlying(fixed_rate)));
}
GC::Ref<BaseAudioContext> AudioParam::context() const { return *static_cast<BaseAudioContext*>(web_rust_audio_param_context(const_cast<AudioParam*>(this))); }
WebIDL::ExceptionOr<void> AudioParam::set_value(float value)
{
    auto& vm = JS::VM::the();
    TRY(JS::HostABI::completion_from_abi<void>(web_rust_audio_param_set_value(JS::HostABI::vm_to_abi(vm), vm.current_realm(), this, value)));
    return {};
}
float AudioParam::intrinsic_value_at_time(double time) const { return web_rust_audio_param_intrinsic_value(const_cast<AudioParam*>(this), time); }
NonnullRefPtr<Rendering::RenderAudioParam> AudioParam::render_param() const { return adopt_ref(*new Rendering::RenderAudioParam(web_rust_audio_param_render_param(const_cast<AudioParam*>(this)))); }

}
