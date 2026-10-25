/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/WebAudio/AudioArray.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/Rendering/RenderNodes.h>
#include <LibWeb/WebAudio/WaveShaperNode.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(WaveShaperNode);

WaveShaperNode::WaveShaperNode(GC::Ref<BaseAudioContext> context)
    : AudioNode(context)
{
}

WaveShaperNode::~WaveShaperNode() = default;

WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> WaveShaperNode::create(GC::Ref<BaseAudioContext> context, WaveShaperOptions const& options)
{
    // When the constructor is called with a BaseAudioContext c and an option object option, the user agent MUST
    // initialize the AudioNode this, with context and options as arguments.
    auto node = GC::Heap::the().allocate<WaveShaperNode>(context);

    // https://webaudio.github.io/web-audio-api/#WaveShaperNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = ChannelCountMode::Max;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    default_options.channel_count = 2;
    // FIXME: Set tail-time to yes when the oversample attribute is "2x" or "4x".

    TRY(node->initialize_audio_node_options(options, default_options));

    node->queue_render_node_creation(make<Rendering::WaveShaperRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size()));

    node->m_oversample = options.oversample;

    // Also, let [[curve set]] be an internal slot of this WaveShaperNode. Initialize this slot to false. If options is
    // given and specifies a curve, set [[curve set]] to true.
    // NB: Setting the curve attribute to the curve of options sets [[curve set]] to true.
    if (options.curve.has_value()) {
        auto& realm = HTML::relevant_realm(node->relevant_global_object());
        auto const& curve = *options.curve;
        auto array = TRY(JS::Float32Array::create(realm, curve.size()));
        overwrite_float32_array(*array, curve);
        TRY(node->set_curve(array));
    } else {
        node->send_parameters_to_render_node();
    }

    return node;
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-waveshapernode
WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> WaveShaperNode::create_for_constructor(GC::Ref<BaseAudioContext> context, WaveShaperOptions const& options)
{
    return create(context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
WebIDL::ExceptionOr<void> WaveShaperNode::set_curve(GC::Ptr<JS::Float32Array> new_curve)
{
    // A InvalidStateError MUST be thrown if this attribute is set with a Float32Array that has a length less than 2.
    if (new_curve && float32_array_length(*new_curve) < 2)
        return WebIDL::InvalidStateError::create("The curve must have at least two values"_utf16);

    // To set the curve attribute, execute these steps:
    // 1. Let new curve be a Float32Array to be assigned to curve or null. .
    // 2. If new curve is not null and [[curve set]] is true, throw an InvalidStateError and abort these steps.
    if (new_curve && m_curve_set)
        return WebIDL::InvalidStateError::create("The curve has already been set"_utf16);

    // 3. If new curve is not null, set [[curve set]] to true.
    if (new_curve)
        m_curve_set = true;

    // 4. Assign new curve to the curve attribute.
    m_curve = new_curve;

    // When this attribute is set, an internal copy of the curve is created by the WaveShaperNode. Subsequent
    // modifications of the contents of the array used to set the attribute therefore have no effect.
    m_curve_copy = new_curve ? adopt_ref(*new Rendering::WaveShaperCurve(copy_float32_array(*new_curve))) : nullptr;
    send_parameters_to_render_node();
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-oversample
void WaveShaperNode::set_oversample(OverSampleType oversample)
{
    m_oversample = oversample;
    send_parameters_to_render_node();
}

void WaveShaperNode::send_parameters_to_render_node()
{
    context()->queue_control_message(NodeMessage { SetWaveShaperParameters {
        .node_id = node_id(),
        .curve = m_curve_copy,
        .oversample = m_oversample,
    } });
}

void WaveShaperNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_curve);
}

}
