/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/CellAllocator.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioWorkletGlobalScope.h>
#include <LibWeb/WebAudio/AudioWorkletProcessor.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioWorkletProcessor);

AudioWorkletProcessor::~AudioWorkletProcessor() = default;

WebIDL::ExceptionOr<GC::Ref<AudioWorkletProcessor>> AudioWorkletProcessor::construct_impl(JS::Realm& realm)
{
    auto& global_scope = as<AudioWorkletGlobalScope>(realm.global_object());
    auto pending_port = global_scope.take_pending_processor_port();
    if (!pending_port)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Illegal constructor"sv };

    return realm.create<AudioWorkletProcessor>(realm, *pending_port);
}

AudioWorkletProcessor::AudioWorkletProcessor(JS::Realm& realm, GC::Ref<HTML::MessagePort> port)
    : Bindings::PlatformObject(realm)
    , m_port(port)
{
}

void AudioWorkletProcessor::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioWorkletProcessor);
    Base::initialize(realm);
}

void AudioWorkletProcessor::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_port);
}

}
