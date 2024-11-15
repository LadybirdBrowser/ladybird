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

GC_DEFINE_ALLOCATOR(GainNode);

GainNode::~GainNode() = default;

WebIDL::ExceptionOr<GC::Ref<GainNode>> GainNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, GainOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-gainnode-gainnode
WebIDL::ExceptionOr<GC::Ref<GainNode>> GainNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, GainOptions const& options)
{
    // Create the node and allocate memory
    auto node = realm.create<GainNode>(realm, context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#GainNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));
    return node;
}

GainNode::GainNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, GainOptions const& options)
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
