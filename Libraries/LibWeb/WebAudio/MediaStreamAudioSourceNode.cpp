/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibGC/Heap.h>
#include <LibMedia/Audio/SpscAudioFrameRing.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebAudio/AudioContext.h>
#include <LibWeb/WebAudio/BaseAudioContext.h>
#include <LibWeb/WebAudio/MediaStreamAudioSourceNode.h>
#include <LibWeb/WebAudio/Rendering/MediaStreamSourceRenderNode.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebAudio {

GC_DEFINE_ALLOCATOR(MediaStreamAudioSourceNode);

// Drop whole incoming blocks that cannot fit so the producer never mutates the consumer index.
static void push_frames_if_enough_room(Media::SpscAudioFrameRing& ring, ReadonlySpan<float> interleaved_samples)
{
    auto channel_count = ring.channel_count();
    auto frame_count = interleaved_samples.size() / channel_count;
    if (ring.frames_free() < frame_count)
        return;
    [[maybe_unused]] auto frames_pushed = ring.try_push(interleaved_samples);
    VERIFY(frames_pushed == frame_count);
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
    auto channel_count = m_input_track->channel_count() == 1 ? 1u : 2u;
    m_ring = adopt_ref(*new Media::SpscAudioFrameRing(ring_capacity_in_frames, channel_count));

    context()->queue_control_message(NodeMessage { SetMediaStreamSourceRing { node_id(), m_ring, channel_count } });

    auto converter = Media::FFmpeg::FFmpegAudioConverter::try_create();
    if (converter.is_error())
        return;
    auto channel_map = channel_count == 1 ? Audio::ChannelMap::mono() : Audio::ChannelMap::stereo();
    if (converter.value()->set_output_sample_specification(Audio::SampleSpecification(static_cast<u32>(context()->sample_rate()), channel_map)).is_error())
        return;

    m_sink = adopt_ref(*new MediaCapture::AudioFrameSink);
    m_sink->on_frames = [ring = m_ring, converter = converter.release_value(), frames_received = u64 { 0 }, scratch = Vector<float>()](float const* samples, size_t frame_count, u8 channels, u32 sample_rate) mutable {
        // Own the conversion state and reference only the ring, never the GC-managed source node.
        if (frame_count == 0 || channels == 0 || sample_rate == 0)
            return;
        auto output_channels = ring->channel_count();
        auto channel_map = output_channels == 1 ? Audio::ChannelMap::mono() : Audio::ChannelMap::stereo();
        Media::AudioBlock input;
        auto timestamp = AK::Duration::from_microseconds(static_cast<i64>(frames_received) * 1'000'000 / sample_rate);
        frames_received += frame_count;
        input.initialize(Audio::SampleSpecification(sample_rate, channel_map), timestamp, frame_count);
        for (size_t channel = 0; channel < output_channels; ++channel) {
            auto output = input.channel_data(channel);
            for (size_t frame = 0; frame < frame_count; ++frame)
                output[frame] = samples[frame * channels + (channel < channels ? channel : 0)];
        }
        if (converter->push_block(input).is_error())
            return;
        while (true) {
            Media::AudioBlock converted;
            if (converter->retrieve_block(converted).is_error() || converted.is_empty())
                break;
            scratch.resize(converted.frame_count() * output_channels);
            for (size_t frame = 0; frame < converted.frame_count(); ++frame) {
                for (size_t channel = 0; channel < output_channels; ++channel)
                    scratch[frame * output_channels + channel] = converted.channel_data(channel)[frame];
            }
            push_frames_if_enough_room(*ring, scratch);
        }
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
