/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/ConvolverNodePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebAudio/AudioBuffer.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/ConvolverNode.h>
#include <LibWeb/WebAudio/RenderNodes/ConvolverRenderNode.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(ConvolverNode);

ConvolverNode::ConvolverNode(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ConvolverOptions const& options)
    : AudioNode(realm, context)
    , m_buffer(options.buffer)
    , m_normalize(!options.disable_normalization)
{
}

ConvolverNode::~ConvolverNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-convolvernode-buffer
WebIDL::ExceptionOr<void> ConvolverNode::set_buffer(GC::Ptr<AudioBuffer> buffer)
{
    if (buffer && !impulse_buffer_is_valid(*buffer))
        return WebIDL::NotSupportedError::create(realm(), "Convolver buffer is incompatible with this context"_utf16);

    m_buffer = buffer;
    context()->notify_audio_graph_changed();
    return {};
}

// https://webaudio.github.io/web-audio-api/#dom-convolvernode-normalize
void ConvolverNode::set_normalize(bool normalize)
{
    m_normalize = normalize;
    context()->notify_audio_graph_changed();
}

// https://webaudio.github.io/web-audio-api/#audionode-channelcount-constraints
WebIDL::ExceptionOr<void> ConvolverNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    if (channel_count == 0 || channel_count > 2)
        return WebIDL::NotSupportedError::create(realm(), "ConvolverNode does not support channel count greater than 2"_utf16);

    return AudioNode::set_channel_count(channel_count);
}

// https://webaudio.github.io/web-audio-api/#audionode-channelcountmode-constraints
WebIDL::ExceptionOr<void> ConvolverNode::set_channel_count_mode(Bindings::ChannelCountMode mode)
{
    if (mode == Bindings::ChannelCountMode::Max)
        return WebIDL::NotSupportedError::create(realm(), "ConvolverNode does not support max as channelCountMode"_utf16);

    return AudioNode::set_channel_count_mode(mode);
}

bool ConvolverNode::impulse_buffer_is_valid(AudioBuffer const& buffer) const
{
    // https://webaudio.github.io/web-audio-api/#dom-convolvernode-buffer
    // The buffer must have 1, 2, or 4 channels and match the context sample rate.
    auto channel_count = buffer.number_of_channels();
    if (channel_count != 1 && channel_count != 2 && channel_count != 4)
        return false;

    if (buffer.sample_rate() != context()->sample_rate())
        return false;

    if (buffer.length() == 0)
        return false;

    return true;
}

WebIDL::ExceptionOr<GC::Ref<ConvolverNode>> ConvolverNode::create(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ConvolverOptions const& options)
{
    return construct_impl(realm, context, options);
}

// https://webaudio.github.io/web-audio-api/#dom-convolvernode-convolvernode
WebIDL::ExceptionOr<GC::Ref<ConvolverNode>> ConvolverNode::construct_impl(JS::Realm& realm, GC::Ref<BaseAudioContext> context, ConvolverOptions const& options)
{
    auto node = realm.create<ConvolverNode>(realm, context, options);

    // Default options for channel count and interpretation.
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = Bindings::ChannelCountMode::ClampedMax;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    default_options.channel_count = 2;

    TRY(node->initialize_audio_node_options(options, default_options));

    if (node->m_buffer && !node->impulse_buffer_is_valid(*node->m_buffer))
        return WebIDL::NotSupportedError::create(realm, "Convolver buffer is incompatible with this context"_utf16);

    return node;
}

void ConvolverNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(ConvolverNode);
    Base::initialize(realm);
}

void ConvolverNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_buffer);
}

}
