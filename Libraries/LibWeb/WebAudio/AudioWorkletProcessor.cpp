/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/AudioWorkletProcessor.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioWorkletProcessor);

// https://webaudio.github.io/web-audio-api/#dom-audioworkletprocessor-audioworkletprocessor
WebIDL::ExceptionOr<GC::Ref<AudioWorkletProcessor>> AudioWorkletProcessor::construct_impl(JS::Realm& realm)
{
    // 1. Let nodeName be the value of the current pending processor construction data's node name... (steps
    //    collapsed): When the constructor is invoked outside of AudioWorkletNode instantiation, there is no
    //    pending processor construction data, and a TypeError must be thrown.
    auto* global_scope = Bindings::impl_from<AudioWorkletGlobalScope>(&realm.global_object());
    if (!global_scope)
        return realm.vm().throw_completion<JS::TypeError>("AudioWorkletProcessor can only be constructed in an AudioWorkletGlobalScope"sv);

    auto pending_data = global_scope->take_pending_processor_construction_data();
    if (!pending_data.has_value())
        return realm.vm().throw_completion<JS::TypeError>("AudioWorkletProcessor cannot be constructed directly; it is instantiated by AudioWorkletNode"sv);

    // 2. Set this's port to the pending construction data's transferred port.
    return GC::Heap::the().allocate<AudioWorkletProcessor>(pending_data->port);
}

AudioWorkletProcessor::AudioWorkletProcessor(GC::Ref<HTML::MessagePort> port)
    : m_port(port)
{
}

AudioWorkletProcessor::~AudioWorkletProcessor() = default;

GC::Ptr<Bindings::Wrappable> AudioWorkletProcessor::relevant_global_impl() const
{
    // Pin wrapper creation to the worklet realm: the processor's port lives on the worklet global.
    return m_port->relevant_global_impl();
}

void AudioWorkletProcessor::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_port);
}

}
