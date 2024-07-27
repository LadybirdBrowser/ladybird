/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/GainNode.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(GainNode);

GainNode::~GainNode() = default;

JS::NonnullGCPtr<GainNode> GainNode::create(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, GainOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-gainnode-gainnode
JS::NonnullGCPtr<GainNode> GainNode::construct_impl(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, GainOptions const& options)
{
    // FIXME: Invoke "Initialize the AudioNode" steps.
    return realm.vm().heap().allocate<GainNode>(realm, realm, context, options);
}

GainNode::GainNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, GainOptions const& options)
    : AudioNode(realm, context)
    , m_gain(AudioParam::create(realm, options.gain, NumericLimits<float>::lowest(), NumericLimits<float>::max(), Bindings::AutomationRate::ARate))
{
}

void GainNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(GainNode);
}

void GainNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_gain);
}

}
