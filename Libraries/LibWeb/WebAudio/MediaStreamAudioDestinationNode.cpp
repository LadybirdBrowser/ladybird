/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/EventLoop.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebAudio/Rendering/MediaStreamDestinationRenderNode.h>
#include <LibWeb/WebAudio/RustFFI.h>
namespace Web::WebAudio {

extern "C" void web_audio_media_destination_create_stream(MediaCapture::MediaStream**, MediaCapture::MediaStreamTrack**);
extern "C" RustFFI::FfiSharedCppObject web_audio_media_destination_create_state(MediaCapture::MediaStreamTrack*, u64, u32, u32, void**);
extern "C" void web_audio_media_destination_clear_drain(Rendering::MediaStreamDestinationSharedState*);
extern "C" JS::Object* web_audio_media_destination_wrap_stream(JS::Realm*, MediaCapture::MediaStream*);
void web_audio_media_destination_create_stream(MediaCapture::MediaStream** out_stream, MediaCapture::MediaStreamTrack** out_track)
{
    auto track = MediaCapture::MediaStreamTrack::create(Bindings::MediaStreamTrackKind::Audio, "MediaStreamAudioDestinationNode"_utf16);
    auto stream = MediaCapture::MediaStream::create();
    stream->add_track(track);
    *out_stream = stream.ptr();
    *out_track = track.ptr();
}
RustFFI::FfiSharedCppObject web_audio_media_destination_create_state(MediaCapture::MediaStreamTrack* track, u64 node_id, u32 channel_count, u32 sample_rate, void** render_node)
{
    // Advertise the delivery format in the track's settings so downstream consumers (e.g. a
    // MediaStreamAudioSourceNode built on this track) can size their pipelines for it.
    // FIXME: Changing the node's channelCount after construction should change the track's
    //        channel count as well; the ring's layout is currently fixed at creation.
    MediaCapture::MediaTrackSettings settings;
    settings.sample_rate = sample_rate;
    settings.channel_count = channel_count;
    track->set_settings(move(settings));

    // The renderer produces audio in bursts of up to ~100 ms (its device buffer target), and
    // the control thread drains on a deferred-invoke cadence after each burst; 8192 frames
    // (~170 ms at 48 kHz) absorb a full burst plus scheduling jitter.
    static constexpr size_t ring_capacity_in_frames = 8192;
    auto ring = adopt_ref(*new Audio::SpscAudioFrameRing(ring_capacity_in_frames, channel_count));
    auto shared_state = adopt_ref(*new Rendering::MediaStreamDestinationSharedState(move(ring)));

    // The drain callback runs (and is later cleared) exclusively on the control thread; it is
    // the only place a GC root crosses into the shared state, so the render thread never
    // touches GC-managed memory.
    shared_state->drain = [track = GC::make_root(*track), ring = shared_state->ring, sample_rate, chunk = Vector<float>()]() mutable {
        // Forward the rendered audio to the track in render-quantum-sized chunks — the
        // granularity real-time consumers pace themselves against.
        auto channel_count = ring->channel_count();
        chunk.resize(128 * channel_count);
        while (true) {
            auto frame_count = ring->try_pop(chunk);
            if (frame_count == 0)
                break;
            track->deliver_audio_frames(chunk.data(), frame_count, static_cast<u8>(channel_count), sample_rate);
        }
    };

    Rendering::MediaStreamDestinationRenderNode node(NodeID(node_id), 128, channel_count, shared_state, Core::EventLoop::current());
    *render_node = node.release_rust_node();
    return { &shared_state.leak_ref(), [](void* state) { static_cast<Rendering::MediaStreamDestinationSharedState*>(state)->ref(); }, [](void* state) { static_cast<Rendering::MediaStreamDestinationSharedState*>(state)->unref(); } };
}
void web_audio_media_destination_clear_drain(Rendering::MediaStreamDestinationSharedState* state) { state->drain = nullptr; }
JS::Object* web_audio_media_destination_wrap_stream(JS::Realm* realm, MediaCapture::MediaStream* stream) { return Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, GC::Ref { *stream }).ptr(); }

}
