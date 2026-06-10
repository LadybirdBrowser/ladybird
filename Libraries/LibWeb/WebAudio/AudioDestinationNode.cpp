/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2024, Bar Yemini <bar.ye651@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/AudioDestinationNode.h>
#include <LibWeb/WebAudio/AudioNode.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/OfflineAudioContext.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(AudioDestinationNode);

AudioDestinationNode::AudioDestinationNode(GC::Ref<BaseAudioContext> context, WebIDL::UnsignedLong channel_count)
    : AudioNode(context, channel_count)
{
}

AudioDestinationNode::~AudioDestinationNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-audiodestinationnode-maxchannelcount
WebIDL::UnsignedLong AudioDestinationNode::max_channel_count()
{
    dbgln("FIXME: Implement Audio::DestinationNode::max_channel_count()");
    return 2;
}

WebIDL::ExceptionOr<GC::Ref<AudioDestinationNode>> AudioDestinationNode::create(GC::Ref<BaseAudioContext> context, WebIDL::UnsignedLong channel_count)
{
    auto node = GC::Heap::the().allocate<AudioDestinationNode>(context, channel_count);

    // Default options for channel count and interpretation
    // https://webaudio.github.io/web-audio-api/#AudioDestinationNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count_mode = ChannelCountMode::Explicit;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    default_options.channel_count = channel_count;
    // FIXME: Set tail-time to no

    TRY(node->initialize_audio_node_options(AudioNodeOptions {}, default_options));

    return node;
}

// https://webaudio.github.io/web-audio-api/#dom-audionode-channelcount
WebIDL::ExceptionOr<void> AudioDestinationNode::set_channel_count(WebIDL::UnsignedLong channel_count)
{
    if (channel_count == this->channel_count())
        return {};

    // The behavior depends on whether the destination node is the destination of an AudioContext
    // or OfflineAudioContext:

    // AudioContext: The channel count MUST be between 1 and maxChannelCount. An IndexSizeError
    // exception MUST be thrown for any attempt to set the count outside this range.
    if (is<AudioContext>(*context())) {
        if (channel_count < 1 || channel_count > max_channel_count())
            return WebIDL::IndexSizeError::create("Channel index is out of range"_utf16);
    }

    // OfflineAudioContext: The channel count cannot be changed. An InvalidStateError exception MUST
    // be thrown for any attempt to change the value.
    if (is<OfflineAudioContext>(*context()))
        return WebIDL::InvalidStateError::create("Cannot change channel count in an OfflineAudioContext"_utf16);

    return AudioNode::set_channel_count(channel_count);
}

}
