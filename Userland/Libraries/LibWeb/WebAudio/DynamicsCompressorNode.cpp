/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/DynamicsCompressorNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/DynamicsCompressorNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(DynamicsCompressorNode);

DynamicsCompressorNode::~DynamicsCompressorNode() = default;

WebIDL::ExceptionOr<GC::Ref<DynamicsCompressorNode>> DynamicsCompressorNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, DynamicsCompressorOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-dynamicscompressornode-dynamicscompressornode
WebIDL::ExceptionOr<GC::Ref<DynamicsCompressorNode>> DynamicsCompressorNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, DynamicsCompressorOptions const& options)
{
    // FIXME: Invoke "Initialize the AudioNode" steps.
    return realm.vm().heap().allocate<DynamicsCompressorNode>(realm, realm, context, options);
}

DynamicsCompressorNode::DynamicsCompressorNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, DynamicsCompressorOptions const& options)
    : AudioNode(realm, context)
    , m_threshold(AudioParam::create(realm, options.threshold, -100, 0, Bindings::AutomationRate::KRate))
    , m_knee(AudioParam::create(realm, options.knee, 0, 40, Bindings::AutomationRate::KRate))
    , m_ratio(AudioParam::create(realm, options.ratio, 1, 20, Bindings::AutomationRate::KRate))
    , m_attack(AudioParam::create(realm, options.attack, 0, 1, Bindings::AutomationRate::KRate))
    , m_release(AudioParam::create(realm, options.release, 0, 1, Bindings::AutomationRate::KRate))
{
}

void DynamicsCompressorNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DynamicsCompressorNode);
}

void DynamicsCompressorNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_threshold);
    visitor.visit(m_knee);
    visitor.visit(m_ratio);
    visitor.visit(m_attack);
    visitor.visit(m_release);
}

}
