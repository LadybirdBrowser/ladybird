/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/WaveShaperNode.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(WaveShaperNode);

WaveShaperNode::~WaveShaperNode() = default;

static WebIDL::ExceptionOr<GC::Ptr<JS::Float32Array>> create_curve_from_options(JS::Realm& realm, Optional<Vector<float>> const& curve)
{
    if (!curve.has_value())
        return GC::Ptr<JS::Float32Array> {};

    auto const& curve_values = curve.value();
    auto curve_byte_buffer = TRY_OR_THROW_OOM(realm.vm(), ByteBuffer::copy({ curve_values.data(), curve_values.size() * sizeof(float) }));
    auto curve_array_buffer = JS::ArrayBuffer::create(realm, move(curve_byte_buffer));
    return GC::Ptr<JS::Float32Array> { JS::Float32Array::create(realm, curve_values.size(), *curve_array_buffer) };
}

WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> WaveShaperNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, WaveShaperOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-waveshapernode
WebIDL::ExceptionOr<GC::Ref<WaveShaperNode>> WaveShaperNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, WaveShaperOptions const& options)
{
    auto node = realm.create<WaveShaperNode>(realm, context, options);

    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Max;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 2;

    TRY(node->initialize_audio_node_options(options, default_options));

    node->m_curve = TRY(create_curve_from_options(realm, options.curve));

    return node;
}

WaveShaperNode::WaveShaperNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, WaveShaperOptions const& options)
    : AudioNode(realm, context)
    , m_oversample(options.oversample)
{
}

void WaveShaperNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WaveShaperNode);
    Base::initialize(realm);
}

void WaveShaperNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_curve);
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-curve
WebIDL::ExceptionOr<void> WaveShaperNode::set_curve(Optional<GC::Root<WebIDL::BufferSource>> const& curve)
{
    if (!curve.has_value()) {
        m_curve = nullptr;
        context()->notify_audio_graph_changed();
        return {};
    }

    auto& vm = this->vm();
    auto& raw_object = *curve.value()->raw_object();
    if (!is<JS::Float32Array>(raw_object))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Float32Array");

    m_curve = &static_cast<JS::Float32Array&>(raw_object);
    context()->notify_audio_graph_changed();
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-waveshapernode-oversample
void WaveShaperNode::set_oversample(Bindings::OverSampleType oversample)
{
    m_oversample = oversample;
    context()->notify_audio_graph_changed();
}

}
