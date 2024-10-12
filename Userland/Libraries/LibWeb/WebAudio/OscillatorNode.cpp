/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/OscillatorNodePrototype.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/OscillatorNode.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(OscillatorNode);

OscillatorNode::~OscillatorNode() = default;

WebIDL::ExceptionOr<JS::NonnullGCPtr<OscillatorNode>> OscillatorNode::create(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, OscillatorOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-oscillatornode
WebIDL::ExceptionOr<JS::NonnullGCPtr<OscillatorNode>> OscillatorNode::construct_impl(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, OscillatorOptions const& options)
{
    // FIXME: Invoke "Initialize the AudioNode" steps.
    TRY(verify_valid_type(realm, options.type));
    auto node = realm.vm().heap().allocate<OscillatorNode>(realm, realm, context, options);
    return node;
}

OscillatorNode::OscillatorNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, OscillatorOptions const& options)
    : AudioScheduledSourceNode(realm, context)
    , m_frequency(AudioParam::create(realm, options.frequency, -context->nyquist_frequency(), context->nyquist_frequency(), Bindings::AutomationRate::ARate))
{
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
Bindings::OscillatorType OscillatorNode::type() const
{
    return m_type;
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
WebIDL::ExceptionOr<void> OscillatorNode::verify_valid_type(JS::Realm& realm, Bindings::OscillatorType type)
{
    // The shape of the periodic waveform. It may directly be set to any of the type constant values except
    // for "custom". ⌛ Doing so MUST throw an InvalidStateError exception. The setPeriodicWave() method can
    // be used to set a custom waveform, which results in this attribute being set to "custom". The default
    // value is "sine". When this attribute is set, the phase of the oscillator MUST be conserved.
    if (type == Bindings::OscillatorType::Custom)
        return WebIDL::InvalidStateError::create(realm, "Oscillator node type cannot be set to 'custom'"_string);

    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-oscillatornode-type
WebIDL::ExceptionOr<void> OscillatorNode::set_type(Bindings::OscillatorType type)
{
    TRY(verify_valid_type(realm(), type));
    m_type = type;
    return {};
}

void OscillatorNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(OscillatorNode);
}

void OscillatorNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_frequency);
}

}
