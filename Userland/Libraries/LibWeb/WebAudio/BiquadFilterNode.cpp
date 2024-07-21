/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/BiquadFilterNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/BiquadFilterNode.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(BiquadFilterNode);

BiquadFilterNode::BiquadFilterNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, BiquadFilterOptions const&)
    : AudioNode(realm, context)
{
}

BiquadFilterNode::~BiquadFilterNode() = default;

WebIDL::ExceptionOr<JS::NonnullGCPtr<BiquadFilterNode>> BiquadFilterNode::create(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, BiquadFilterOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-biquadfilternode-biquadfilternode
WebIDL::ExceptionOr<JS::NonnullGCPtr<BiquadFilterNode>> BiquadFilterNode::construct_impl(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, BiquadFilterOptions const& options)
{
    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
    // MUST initialize the AudioNode this, with context and options as arguments.

    auto node = realm.vm().heap().allocate<BiquadFilterNode>(realm, realm, context, options);
    return node;
}

void BiquadFilterNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(BiquadFilterNode);
}

void BiquadFilterNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
