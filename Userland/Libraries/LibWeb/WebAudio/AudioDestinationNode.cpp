/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioDestinationNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(AudioDestinationNode);

AudioDestinationNode::AudioDestinationNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context)
    : AudioNode(realm, context)
{
}

AudioDestinationNode::~AudioDestinationNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-audiodestinationnode-maxchannelcount
WebIDL::UnsignedLong AudioDestinationNode::max_channel_count()
{
    dbgln("FIXME: Implement Audio::DestinationNode::max_channel_count()");
    return 2;
}

JS::NonnullGCPtr<AudioDestinationNode> AudioDestinationNode::construct_impl(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context)
{
    return realm.heap().allocate<AudioDestinationNode>(realm, realm, context);
}

void AudioDestinationNode::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(AudioDestinationNode);
}

void AudioDestinationNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
