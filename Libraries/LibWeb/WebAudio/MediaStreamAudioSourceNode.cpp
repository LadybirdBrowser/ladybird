/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibGC/Heap.h>
#include <LibMedia/Audio/SpscAudioFrameRing.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/MediaStreamAudioSourceNode.h>
#include <LibWeb/WebAudio/Rendering/MediaStreamSourceRenderNode.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(MediaStreamAudioSourceNode);

// Pushes interleaved frames, discarding the oldest queued audio when the ring is full so a
// stalled consumer resumes at the live edge instead of replaying a ring full of stale audio.
// NB: Popping from the producing side bends the SPSC discipline. It can only race with the
//     consumer while the ring is simultaneously full and being drained, in which case the
//     worst outcome is a repeated or skipped fragment in audio that is already glitching
//     from the overflow; the ring's masked indices keep the race memory-safe.
static void push_frames_dropping_oldest(Media::SpscAudioFrameRing& ring, ReadonlySpan<float> interleaved_samples, Vector<float>& discard_scratch)
{
    auto channel_count = ring.channel_count();
    auto frame_count = interleaved_samples.size() / channel_count;
    auto free_frames = ring.frames_free();
    if (free_frames < frame_count) {
        auto frames_to_discard = min(frame_count - free_frames, ring.frames_available());
        discard_scratch.resize(frames_to_discard * channel_count);
        ring.try_pop(discard_scratch);
    }
    ring.try_push(interleaved_samples);
}

MediaStreamAudioSourceNode::MediaStreamAudioSourceNode(GC::Ref<AudioContext> context, MediaStreamAudioSourceOptions const& options)
    : AudioNode(context)
    , m_media_stream(*options.media_stream)
{
}

MediaStreamAudioSourceNode::~MediaStreamAudioSourceNode() = default;

// https://webaudio.github.io/web-audio-api/#dom-mediastreamaudiosourcenode-mediastreamaudiosourcenode
WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> MediaStreamAudioSourceNode::create(GC::Ref<AudioContext> context, MediaStreamAudioSourceOptions const& options)
{
    // 1. If the mediaStream member of options does not reference a MediaStream that has at least one MediaStreamTrack
    //    whose kind attribute has the value "audio", throw an InvalidStateError and abort these steps. Else, let this
    //    stream be inputStream.
    auto audio_tracks = options.media_stream->get_audio_tracks();
    if (audio_tracks.is_empty())
        return WebIDL::InvalidStateError::create("MediaStream has no audio tracks"_utf16);

    // 2. Let tracks be the list of all MediaStreamTracks of inputStream that have a kind of "audio".
    // 3. Sort the elements in tracks based on their id attribute using an ordering on sequences of code unit values.
    quick_sort(audio_tracks, [](auto const& a, auto const& b) { return a->id() < b->id(); });

    // 4. Initialize the AudioNode this, with context and options as arguments.
    auto node = GC::Heap::the().allocate<MediaStreamAudioSourceNode>(context, options);

    // https://webaudio.github.io/web-audio-api/#MediaStreamAudioSourceNode
    AudioNodeDefaultOptions default_options;
    default_options.channel_count = 2;
    default_options.channel_count_mode = ChannelCountMode::Max;
    default_options.channel_interpretation = ChannelInterpretation::Speakers;
    TRY(node->initialize_audio_node_options({}, default_options));

    // 5. Set an internal slot [[input track]] on this MediaStreamAudioSourceNode to be the first element of tracks.
    //    This is the track used as the input audio for this MediaStreamAudioSourceNode.
    node->m_input_track = audio_tracks.first();

    node->queue_render_node_creation(make<Rendering::MediaStreamSourceRenderNode>(node->node_id(), BaseAudioContext::render_quantum_size()));
    node->attach_input_track();

    return node;
}

WebIDL::ExceptionOr<GC::Ref<MediaStreamAudioSourceNode>> MediaStreamAudioSourceNode::create_for_constructor(GC::Ref<AudioContext> context, MediaStreamAudioSourceOptions const& options)
{
    return create(context, options);
}

void MediaStreamAudioSourceNode::attach_input_track()
{
    VERIFY(m_input_track);

    // The ring must absorb at least one PulseAudio fragment (960 frames, 20 ms at 48 kHz) per
    // push; three fragments of headroom (rounded up to 4096 frames by the ring) keep the
    // producer ahead of render-thread scheduling jitter without accumulating latency, since
    // the render node drains a quantum at a time and holds the steady-state fill near one
    // fragment.
    static constexpr size_t ring_capacity_in_frames = 3 * 960;
    auto channel_count = m_input_track->channel_count() != 0 ? m_input_track->channel_count() : 2;
    m_ring = adopt_ref(*new Media::SpscAudioFrameRing(ring_capacity_in_frames, channel_count));

    context()->queue_control_message(NodeMessage { SetMediaStreamSourceRing { node_id(), m_ring, channel_count } });

    m_sink = adopt_ref(*new MediaCapture::AudioFrameSink);
    m_sink->on_frames = [ring = m_ring, conversion_scratch = Vector<float>(), discard_scratch = Vector<float>()](float const* samples, size_t frame_count, u8 channel_count, u32) mutable {
        // Runs on the producing thread (the PulseAudio main loop for microphone-backed
        // tracks). Only the ring is captured — never the GC-managed node, which may be
        // collected while a callback is in flight.
        if (frame_count == 0 || channel_count == 0)
            return;

        auto ring_channel_count = ring->channel_count();
        if (channel_count == ring_channel_count) {
            push_frames_dropping_oldest(*ring, { samples, frame_count * channel_count }, discard_scratch);
            return;
        }

        // The delivered layout can disagree with the negotiated settings the ring was created
        // for. AD-HOC: Convert with a naive mix that replicates the first channel upwards and
        // drops surplus channels.
        conversion_scratch.resize(frame_count * ring_channel_count);
        for (size_t frame = 0; frame < frame_count; ++frame) {
            for (u32 channel = 0; channel < ring_channel_count; ++channel) {
                auto source_channel = channel < channel_count ? channel : 0u;
                conversion_scratch[frame * ring_channel_count + channel] = samples[frame * channel_count + source_channel];
            }
        }
        push_frames_dropping_oldest(*ring, conversion_scratch, discard_scratch);
    };
    m_input_track->add_audio_sink(*m_sink);
}

void MediaStreamAudioSourceNode::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_stream);
    visitor.visit(m_input_track);
}

void MediaStreamAudioSourceNode::finalize()
{
    Base::finalize();

    // NB: Finalizers run before any dead cell is destroyed, so detaching from the input track
    //     is safe even when the track is collected in the same garbage collection cycle.
    if (m_input_track && m_sink)
        m_input_track->remove_audio_sink(*m_sink);
}

}
