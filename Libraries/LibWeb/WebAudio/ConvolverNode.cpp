/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ConvolverNode.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ConvolverNode);

ConvolverNode::ConvolverNode(GC::Ref<BaseAudioContext> context, ConvolverOptions const& options)
    : AudioNode(context)
    , m_normalize(!options.disable_normalization)
{
}

ConvolverNode::~ConvolverNode() = default;

WebIDL::ExceptionOr<GC::Ref<ConvolverNode>> ConvolverNode::create(GC::Ref<BaseAudioContext> context, ConvolverOptions const& options)
{
    // NB: Creating a node through its factory method is defined to run the same steps as its constructor.
    return create_for_constructor(context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-convolvernode-convolvernode
WebIDL::ExceptionOr<GC::Ref<ConvolverNode>> ConvolverNode::create_for_constructor(GC::Ref<BaseAudioContext> context, ConvolverOptions const& options)
{
    // NB: The node and its render node are created up front, so that step 2 has something to hand the impulse
    //     response to.
    auto node = GC::Heap::the().allocate<ConvolverNode>(context, options);
    node->queue_render_node_creation(make<Rendering::ConvolverRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size()));

    // 1. Set the attributes normalize to the inverse of the value of disableNormalization.
    // NB: Done by the constructor above, so that step 2 already sees the final value.

    // 2. If buffer exists, set the buffer attribute to its value.
    if (options.buffer.has_value())
        TRY(node->set_buffer(*options.buffer));

    // 3. Let o be new AudioNodeOptions dictionary.
    // 4. If channelCount exists in options, set channelCount on o with the same value.
    // 5. If channelCountMode exists in options, set channelCountMode on o with the same value.
    // 6. If channelInterpretation exists in options, set channelInterpretation on o with the same value.
    // 7. Initialize the AudioNode this, with c and o as argument.
    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = ChannelCountMode::ClampedMax;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    // FIXME: Set tail-time to yes
    TRY(node->initialize_audio_node_options(options, default_options));

    return node;
}

// https://webaudio.github.io/web-audio-api/#dom-convolvernode-buffer
WebIDL::ExceptionOr<void> ConvolverNode::set_buffer(GC::Ptr<AudioBuffer> buffer)
{
    // 1. If the buffer number of channels is not 1, 2, 4, or if the sample-rate of the buffer is not the same as the
    //    sample-rate of its associated BaseAudioContext, a NotSupportedError MUST be thrown.
    // AD-HOC: The steps do not cover a null buffer, which clears the impulse response and leaves the node silent.
    if (buffer) {
        auto number_of_channels = buffer->number_of_channels();
        if (number_of_channels != 1 && number_of_channels != 2 && number_of_channels != 4)
            return WebIDL::NotSupportedError::create("ConvolverNode buffer must have 1, 2 or 4 channels"_utf16);

        if (buffer->sample_rate() != context()->sample_rate())
            return WebIDL::NotSupportedError::create("ConvolverNode buffer must have the same sample rate as its context"_utf16);
    }

    m_buffer = buffer;

    // 2. Acquire the content of the AudioBuffer.
    // NB: The acquired content is cut into partitions and transformed here, and the delay line that the rendering
    //     thread pairs those partitions with is sized and allocated alongside it, so that the rendering thread
    //     receives both ready to use and never has to build either during a render quantum.
    RefPtr<Rendering::ConvolverKernel> kernel;
    RefPtr<Rendering::ConvolverDelayLine> delay_line;
    if (buffer) {
        if (auto contents = buffer->acquire_contents())
            kernel = Rendering::ConvolverKernel::create(*contents, BaseAudioContext::render_quantum_size(), m_normalize);
        if (kernel)
            delay_line = Rendering::ConvolverDelayLine::create(*kernel);
    }

    context()->queue_control_message(NodeMessage { SetConvolverKernel {
        .node_id = node_id(),
        .kernel = move(kernel),
        .delay_line = move(delay_line),
    } });

    return {};
}

// https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
WebIDL::ExceptionOr<void> ConvolverNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    // The channel count cannot be greater than two, and a NotSupportedError exception MUST be thrown for any attempt
    // to change it to a value greater than two.
    if (channel_count > 2)
        return WebIDL::NotSupportedError::create("ConvolverNode does not support channel count greater than 2"_utf16);

    return AudioNode::set_channel_count(channel_count);
}

// https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
WebIDL::ExceptionOr<void> ConvolverNode::set_channel_count_mode(ChannelCountMode mode)
{
    // The channel count mode cannot be set to "max", and a NotSupportedError exception MUST be thrown for any attempt
    // to set it to "max".
    if (mode == ChannelCountMode::Max)
        return WebIDL::NotSupportedError::create("ConvolverNode does not support 'max' as channelCountMode."_utf16);

    return AudioNode::set_channel_count_mode(mode);
}

void ConvolverNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_buffer);
}

}
