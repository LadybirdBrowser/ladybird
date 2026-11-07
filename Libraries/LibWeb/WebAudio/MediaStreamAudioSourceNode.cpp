/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudio/SpscAudioFrameRing.h>
#include <LibJS/Runtime/Realm.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMediaCodecs/AudioBlock.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebAudio/RustFFI.h>
namespace Web::WebAudio {

extern "C" MediaCapture::MediaStream* web_audio_media_source_stream_from_wrapper(JS::Object*);
extern "C" JS::Object* web_audio_media_source_stream_wrap(JS::Realm*, MediaCapture::MediaStream*);
MediaCapture::MediaStream* web_audio_media_source_stream_from_wrapper(JS::Object* object) { return Bindings::impl_from<MediaCapture::MediaStream>(object); }
JS::Object* web_audio_media_source_stream_wrap(JS::Realm* realm, MediaCapture::MediaStream* object) { return Bindings::wrap(Bindings::host_defined_wrapper_world(*realm), *realm, GC::Ref { *object }).ptr(); }
// Drop whole incoming blocks that cannot fit so the producer never mutates the consumer index.
static void push_frames_if_enough_room(Audio::SpscAudioFrameRing& ring, ReadonlySpan<float> interleaved_samples)
{
    auto channel_count = ring.channel_count();
    auto frame_count = interleaved_samples.size() / channel_count;
    if (ring.frames_free() < frame_count)
        return;
    [[maybe_unused]] auto frames_pushed = ring.try_push(interleaved_samples);
    VERIFY(frames_pushed == frame_count);
}

extern "C" void web_audio_media_source_audio_tracks(MediaCapture::MediaStream*, void*, void (*)(void*, void*));
extern "C" bool web_audio_media_source_track_id_less(MediaCapture::MediaStreamTrack*, MediaCapture::MediaStreamTrack*);
extern "C" u32 web_audio_media_source_track_channels(MediaCapture::MediaStreamTrack*);
extern "C" bool web_audio_media_source_has_live_track(MediaCapture::MediaStreamTrack*);
extern "C" RustFFI::FfiSharedCppObject web_audio_media_source_ring(u32);
extern "C" void web_audio_media_source_ring_callbacks(RustFFI::AudioFrameRingCallbacks*);
extern "C" RustFFI::FfiSharedCppObject web_audio_media_source_attach(MediaCapture::MediaStreamTrack*, Audio::SpscAudioFrameRing*, float);
extern "C" void web_audio_media_source_detach(MediaCapture::MediaStreamTrack*, MediaCapture::AudioFrameSink*);
void web_audio_media_source_audio_tracks(MediaCapture::MediaStream* stream, void* context, void (*callback)(void*, void*))
{
    for (auto track : stream->get_audio_tracks())
        callback(context, track.ptr());
}
bool web_audio_media_source_track_id_less(MediaCapture::MediaStreamTrack* a, MediaCapture::MediaStreamTrack* b) { return a->id() < b->id(); }
u32 web_audio_media_source_track_channels(MediaCapture::MediaStreamTrack* track) { return track->channel_count(); }
bool web_audio_media_source_has_live_track(MediaCapture::MediaStreamTrack* track) { return track->track_ready_state() == MediaCapture::MediaStreamTrackState::Live; }
RustFFI::FfiSharedCppObject web_audio_media_source_ring(u32 channels)
{
    return { new Audio::SpscAudioFrameRing(3 * 960, channels), [](void* ring) { static_cast<Audio::SpscAudioFrameRing*>(ring)->ref(); }, [](void* ring) { static_cast<Audio::SpscAudioFrameRing*>(ring)->unref(); } };
}
void web_audio_media_source_ring_callbacks(RustFFI::AudioFrameRingCallbacks* callbacks)
{
    *callbacks = {
        [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->channel_count(); },
        [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->frames_available(); },
        [](void* ring) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->frames_free(); },
        [](void* ring, Span<float> samples) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->try_pop(samples); },
        [](void* ring, ReadonlySpan<float> samples) { return static_cast<Audio::SpscAudioFrameRing*>(ring)->try_push(samples); },
    };
}
RustFFI::FfiSharedCppObject web_audio_media_source_attach(MediaCapture::MediaStreamTrack* track, Audio::SpscAudioFrameRing* ring, float sample_rate)
{
    auto channel_count = ring->channel_count();
    auto converter = Media::FFmpeg::FFmpegAudioConverter::try_create();
    if (converter.is_error())
        return { nullptr, [](void*) { }, [](void*) { } };
    auto channel_map = channel_count == 1 ? Audio::ChannelMap::mono() : Audio::ChannelMap::stereo();
    if (converter.value()->set_output_sample_specification(Audio::SampleSpecification(static_cast<u32>(sample_rate), channel_map)).is_error())
        return { nullptr, [](void*) { }, [](void*) { } };

    auto sink = adopt_ref(*new MediaCapture::AudioFrameSink);
    sink->on_frames = [ring = NonnullRefPtr { *ring }, converter = converter.release_value(), frames_received = u64 { 0 }, scratch = Vector<float>()](float const* samples, size_t frame_count, u8 channels, u32 sample_rate) mutable {
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
    track->add_audio_sink(*sink);
    return { &sink.leak_ref(), [](void* sink) { static_cast<MediaCapture::AudioFrameSink*>(sink)->ref(); }, [](void* sink) { static_cast<MediaCapture::AudioFrameSink*>(sink)->unref(); } };
}
void web_audio_media_source_detach(MediaCapture::MediaStreamTrack* track, MediaCapture::AudioFrameSink* sink) { track->remove_audio_sink(*sink); }

}
