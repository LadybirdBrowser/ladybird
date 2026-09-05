/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibGC/Heap.h>
#include <LibMedia/Audio/SpscAudioFrameRing.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/MediaStreamAudioDestinationNode.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(MediaStreamAudioDestinationNode);

MediaStreamAudioDestinationNode::MediaStreamAudioDestinationNode(GC::Ref<AudioContext> context, GC::Ref<MediaCapture::MediaStream> stream, GC::Ref<MediaCapture::MediaStreamTrack> track)
    : AudioNode(context)
    , m_stream(stream)
    , m_track(track)
{
}

MediaStreamAudioDestinationNode::~MediaStreamAudioDestinationNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiodestinationnode-mediastreamaudiodestinationnode
WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioDestinationNode>> MediaStreamAudioDestinationNode::create(GC::Ref<AudioContext> context, AudioNodeOptions const& options)
{
    // https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiodestinationnode-stream
    // A MediaStream containing a single MediaStreamTrack with the same number of channels as
    // the node itself, and whose kind attribute has the value "audio".
    // NB: The track is deviceless — it carries no deviceId, so registering audio sinks on it
    //     never opens a capture stream; it is fed exclusively by this node's render node.
    auto track = MediaCapture::MediaStreamTrack::create(Bindings::MediaStreamTrackKind::Audio, "MediaStreamAudioDestinationNode"_utf16);
    auto stream = MediaCapture::MediaStream::create();
    stream->add_track(track);

    // 1. Initialize the AudioNode this, with context and options as arguments.
    auto node = GC::Heap::the().allocate<MediaStreamAudioDestinationNode>(context, stream, track);

    // https://webaudio.github.io/web-audio-api/#MediaStreamAudioDestinationNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = ChannelCountMode::Explicit;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    TRY(node->initialize_audio_node_options(options, default_options));

    // Advertise the delivery format in the track's settings so downstream consumers (e.g. a
    // MediaStreamAudioSourceNode built on this track) can size their pipelines for it.
    // FIXME: Changing the node's channelCount after construction should change the track's
    //        channel count as well; the ring's layout is currently fixed at creation.
    auto sample_rate = static_cast<u32>(context->sample_rate());
    MediaCapture::MediaTrackSettings settings;
    settings.sample_rate = sample_rate;
    settings.channel_count = node->channel_count();
    track->set_settings(move(settings));

    // The renderer produces audio in bursts of up to ~100 ms (its device buffer target), and
    // the control thread drains on a deferred-invoke cadence after each burst; 8192 frames
    // (~170 ms at 48 kHz) absorb a full burst plus scheduling jitter.
    static constexpr size_t ring_capacity_in_frames = 8192;
    auto ring = adopt_ref(*new Media::SpscAudioFrameRing(ring_capacity_in_frames, node->channel_count()));
    node->m_shared_state = adopt_ref(*new Rendering::MediaStreamDestinationSharedState(move(ring)));

    // The drain callback runs (and is later cleared) exclusively on the control thread; it is
    // the only place a GC root crosses into the shared state, so the render thread never
    // touches GC-managed memory.
    node->m_shared_state->drain = [track = GC::make_root(track), ring = node->m_shared_state->ring, sample_rate, chunk = Vector<float>()]() mutable {
        // Forward the rendered audio to the track in render-quantum-sized chunks — the
        // granularity real-time consumers pace themselves against.
        auto channel_count = ring->channel_count();
        chunk.resize(BaseAudioContext::render_quantum_size() * channel_count);
        while (true) {
            auto frame_count = ring->try_pop(chunk);
            if (frame_count == 0)
                break;
            track->deliver_audio_frames(chunk.data(), frame_count, static_cast<u8>(channel_count), sample_rate);
        }
    };

    node->queue_render_node_creation(make<Rendering::MediaStreamDestinationRenderNode>(
        node->node_id(), BaseAudioContext::render_quantum_size(), node->channel_count(),
        *node->m_shared_state, Core::EventLoop::current()));

    return node;
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioDestinationNode>> MediaStreamAudioDestinationNode::create_for_constructor(GC::Ref<AudioContext> context, AudioNodeOptions const& options)
{
    return create(context, options);
}

void MediaStreamAudioDestinationNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_stream);
    visitor.visit(m_track);
}

void MediaStreamAudioDestinationNode::finalize()
{
    // Drop the drain callback — and with it the GC root it holds on the track — on the
    // control thread. Any drain the render node still has queued becomes a no-op.
    if (m_shared_state)
        m_shared_state->drain = nullptr;

    Base::finalize();
}

}
