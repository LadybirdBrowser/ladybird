/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/AudioDestinationNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>

namespace Web::WebAudio {

JS_DEFINE_ALLOCATOR(AudioDestinationNode);

AudioDestinationNode::AudioDestinationNode(JS::Realm& realm, JS::NonnullGCPtr<BaseAudioContext> context)
    : AudioNode(realm, context)
{
}

AudioDestinationNode::~AudioDestinationNode() = default;

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
