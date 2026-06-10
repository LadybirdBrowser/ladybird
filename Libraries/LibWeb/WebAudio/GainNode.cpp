/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/NumericLimits.h>
#include <LibGC/Heap.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/AudioParam.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/GainNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(GainNode);

GainNode::~GainNode() = default;

WebIDL::ExceptionOr<GC::Ref<GainNode>> GainNode::create(GC::Ref<BaseAudioContext> context, GainOptions const& options)
{
    // Create the node and allocate memory
    auto node = GC::Heap::the().allocate<GainNode>(context, options);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#GainNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = ChannelCountMode::Max;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(options, default_options));
    return node;
}

// https://webaudio.github.io/web-audio-api/#dom-gainnode-gainnode
WebIDL::ExceptionOr<GC::Ref<GainNode>> GainNode::create_for_constructor(GC::Ref<BaseAudioContext> context, GainOptions const& options)
{
    return create(context, options);
}

GainNode::GainNode(GC::Ref<BaseAudioContext> context, GainOptions const& options)
    : AudioNode(context)
    , m_gain(AudioParam::create(context, options.gain, NumericLimits<float>::lowest(), NumericLimits<float>::max(), AutomationRate::ARate))
{
}

void GainNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_gain);
}

}
