/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/Audio/AudioDevices.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaStreamAudioDestinationNodePrototype.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/MediaStreamAudioDestinationNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(MediaStreamAudioDestinationNode);

MediaStreamAudioDestinationNode::MediaStreamAudioDestinationNode(JS::Realm& realm, GC::Ref<AudioContext> context)
    : AudioNode(realm, context)
    , m_stream(MediaCapture::MediaStream::create(realm))
{
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioDestinationNode>> MediaStreamAudioDestinationNode::create(JS::Realm& realm, GC::Ref<AudioContext> context, AudioNodeOptions const& options)
{
    return construct_impl(realm, context, options);
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioDestinationNode>> MediaStreamAudioDestinationNode::construct_impl(JS::Realm& realm, GC::Ref<AudioContext> context, AudioNodeOptions const& options)
{
    auto node = realm.create<MediaStreamAudioDestinationNode>(realm, context);

    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = Bindings::ChannelCountMode::Explicit;
    default_options.channel_interpretation = Bindings::ChannelInterpretation::Speakers;
    TRY(node->initialize_audio_node_options(options, default_options));

    u32 channel_count = static_cast<u32>(node->channel_count());
    u32 sample_rate_hz = static_cast<u32>(context->sample_rate());
    Media::AudioDeviceInfo output_device_info {
        .type = AudioServer::DeviceInfo::Type::Output,
        .device_handle = 0,
        .label = ByteString {},
        .dom_device_id = ByteString {},
        .group_id = ByteString {},
        .sample_rate_hz = sample_rate_hz,
        .channel_count = channel_count,
        // FIXME: Thread through an explicit output channel map once MediaStreamAudioDestinationNode
        // can expose channel-position semantics instead of only channel count.
        .channel_layout = Audio::ChannelMap::guess_by_channel_count(channel_count),
        .is_default = false,
    };
    auto track = MediaCapture::MediaStreamTrack::create_audio_output_track(realm, output_device_info);
    node->m_stream->add_track(track);

    return node;
}

void MediaStreamAudioDestinationNode::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaStreamAudioDestinationNode);
    Base::initialize(realm);
}

void MediaStreamAudioDestinationNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_stream);
}

}
