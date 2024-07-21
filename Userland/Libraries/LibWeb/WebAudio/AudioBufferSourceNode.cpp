/*
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioScheduledSourceNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioBufferSourceNode.h>
#include <LibWeb/WebAudio/AudioScheduledSourceNode.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(AudioBufferSourceNode);

AudioBufferSourceNode::AudioBufferSourceNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, AudioBufferSourceOptions const&)
    : AudioScheduledSourceNode(realm, context)
{
}

AudioBufferSourceNode::~AudioBufferSourceNode() = default;

WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBufferSourceNode>> AudioBufferSourceNode::create(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, AudioBufferSourceOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-audiobuffersourcenode-audiobuffersourcenode
WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBufferSourceNode>> AudioBufferSourceNode::construct_impl(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context, AudioBufferSourceOptions const& options)
{
    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent
    // MUST initialize the AudioNode this, with context and options as arguments.

    auto node = realm.vm().heap().allocate<AudioBufferSourceNode>(realm, realm, context, options);
    return node;
}

void AudioBufferSourceNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioBufferSourceNode);
}

void AudioBufferSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
