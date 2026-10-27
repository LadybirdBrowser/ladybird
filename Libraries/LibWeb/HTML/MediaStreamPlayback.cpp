/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <AK/Mutex.h>
#include <LibMedia/FFmpeg/FFmpegAudioConverter.h>
#include <LibMedia/Sinks/AudioOutputFactory.h>
#include <LibMediaCodecs/AudioBlock.h>
#include <LibWeb/HTML/MediaStreamPlayback.h>

namespace Web::HTML {

static constexpr u32 TARGET_LATENCY_MS = 100;

// Live audio that the output can't keep up with is dropped rather than played late.
static constexpr u32 MAXIMUM_QUEUED_MILLISECONDS = 200;

// Fed on whichever thread produces the track's audio, drained on the audio output's thread.
struct MediaStreamPlayback::TrackInput final : public AtomicRefCounted<TrackInput> {
    Mutex mutex;
    OwnPtr<Media::FFmpeg::FFmpegAudioConverter> converter;
    Audio::SampleSpecification converter_output_specification;
    i64 frames_received { 0 };
    // Interleaved, in the output's sample specification.
    Vector<float> samples;
};

struct MediaStreamPlayback::Mixer final : public AtomicRefCounted<Mixer> {
    Mutex mutex;
    Vector<NonnullRefPtr<TrackInput>> inputs;
    Audio::SampleSpecification output_specification;
    Atomic<bool> playing { false };
};

static void receive_frames(MediaStreamPlayback::Mixer& mixer, MediaStreamPlayback::TrackInput& input, float const* samples, size_t frame_count, u8 channel_count, u32 sample_rate)
{
    if (!mixer.playing.load(AK::MemoryOrder::memory_order_relaxed) || frame_count == 0 || channel_count == 0 || sample_rate == 0)
        return;

    Audio::SampleSpecification output_specification;
    {
        MutexLocker locker(mixer.mutex);
        output_specification = mixer.output_specification;
    }
    if (!output_specification.is_valid())
        return;

    MutexLocker locker(input.mutex);
    if (!input.converter || input.converter_output_specification != output_specification) {
        auto converter_or_error = Media::FFmpeg::FFmpegAudioConverter::try_create();
        if (converter_or_error.is_error())
            return;
        auto converter = converter_or_error.release_value();
        if (converter->set_output_sample_specification(output_specification).is_error())
            return;
        input.converter = move(converter);
        input.converter_output_specification = output_specification;
        input.samples.clear();
    }

    // FIXME: Downmix sources with more than two channels instead of keeping the first two.
    auto used_channel_count = min<size_t>(channel_count, 2);
    auto channel_map = used_channel_count == 1 ? Audio::ChannelMap::mono() : Audio::ChannelMap::stereo();
    Media::AudioBlock block;
    block.initialize(Audio::SampleSpecification(sample_rate, channel_map), input.frames_received, frame_count);
    input.frames_received += static_cast<i64>(frame_count);
    for (size_t channel = 0; channel < used_channel_count; ++channel) {
        auto destination = block.channel_data(channel);
        for (size_t frame = 0; frame < frame_count; ++frame)
            destination[frame] = samples[(frame * channel_count) + channel];
    }
    if (input.converter->push_block(block).is_error())
        return;

    while (true) {
        Media::AudioBlock converted;
        if (input.converter->retrieve_block(converted).is_error() || converted.is_empty())
            break;
        auto offset = input.samples.size();
        input.samples.resize(offset + converted.sample_count());
        converted.copy_to_interleaved(input.samples.span().slice(offset));
    }

    auto maximum_samples = static_cast<size_t>(output_specification.sample_rate()) * output_specification.channel_count() * MAXIMUM_QUEUED_MILLISECONDS / 1000;
    if (input.samples.size() > maximum_samples)
        input.samples.remove(0, input.samples.size() - maximum_samples);
}

NonnullOwnPtr<MediaStreamPlayback> MediaStreamPlayback::create(NonnullRefPtr<Media::AudioOutputFactory> audio_output_factory)
{
    return adopt_own(*new MediaStreamPlayback(move(audio_output_factory)));
}

MediaStreamPlayback::MediaStreamPlayback(NonnullRefPtr<Media::AudioOutputFactory> audio_output_factory)
    : m_mixer(adopt_ref(*new Mixer))
    , m_audio_output_factory(move(audio_output_factory))
{
}

MediaStreamPlayback::~MediaStreamPlayback()
{
    m_mixer->playing.store(false);
    // This can run while the garbage collector is sweeping, when the tracks may already be gone, so the sinks
    // are only switched off here. Callers stop playback properly through set_tracks({}) beforehand.
    for (auto& subscription : m_subscriptions)
        subscription.sink->deactivate();
}

void MediaStreamPlayback::set_tracks(ReadonlySpan<GC::Ref<MediaCapture::MediaStreamTrack>> tracks)
{
    for (size_t index = m_subscriptions.size(); index-- > 0;) {
        auto track = m_subscriptions[index].track.ptr();
        auto still_present = track && any_of(tracks, [&](auto const& candidate) { return candidate.ptr() == track.ptr(); });
        if (!still_present)
            remove_input(index);
    }

    for (auto const& track : tracks) {
        if (!track->is_audio())
            continue;
        if (any_of(m_subscriptions, [&](auto const& subscription) { return subscription.track.ptr().ptr() == track.ptr(); }))
            continue;

        auto input = adopt_ref(*new TrackInput);
        auto sink = adopt_ref(*new MediaCapture::AudioFrameSink);
        sink->on_frames = [mixer = m_mixer, input](float const* samples, size_t frame_count, u8 channel_count, u32 sample_rate) {
            receive_frames(*mixer, *input, samples, frame_count, channel_count, sample_rate);
        };
        {
            MutexLocker locker(m_mixer->mutex);
            m_mixer->inputs.append(input);
        }
        m_subscriptions.append({ track, sink, input });
        track->add_audio_sink(sink);
    }
}

void MediaStreamPlayback::remove_input(size_t index)
{
    auto subscription = m_subscriptions.take(index);
    if (auto track = subscription.track.ptr())
        track->remove_audio_sink(*subscription.sink);
    else
        subscription.sink->deactivate();

    MutexLocker locker(m_mixer->mutex);
    m_mixer->inputs.remove_first_matching([&](auto const& input) { return input.ptr() == subscription.input.ptr(); });
}

void MediaStreamPlayback::play()
{
    if (m_playing)
        return;
    m_playing = true;
    m_playing_since = MonotonicTime::now();
    m_mixer->playing.store(true);

    if (m_output) {
        (void)m_output->resume();
        return;
    }
    if (m_creating_output)
        return;
    m_creating_output = true;

    auto data_callback = [mixer = m_mixer](Span<float> buffer, MonotonicTime) -> ReadonlySpan<float> {
        buffer.fill(0.0f);
        MutexLocker mixer_locker(mixer->mutex);
        for (auto& input : mixer->inputs) {
            MutexLocker input_locker(input->mutex);
            auto sample_count = min(input->samples.size(), buffer.size());
            for (size_t index = 0; index < sample_count; ++index)
                buffer[index] += input->samples[index];
            input->samples.remove(0, sample_count);
        }
        // A short read is padded with silence, so the output keeps asking for more instead of treating the
        // stream as finished.
        return buffer;
    };

    auto promise = m_audio_output_factory->open_output(Audio::OutputState::Suspended, TARGET_LATENCY_MS, move(data_callback));
    promise->when_resolved([weak_self = make_weak_ptr()](auto& stream) {
        if (!weak_self)
            return;
        auto& self = *weak_self;
        self.m_creating_output = false;
        self.m_output = stream;
        {
            MutexLocker locker(self.m_mixer->mutex);
            self.m_mixer->output_specification = stream->sample_specification();
        }
        (void)stream->set_volume(self.m_volume);
        if (self.m_playing)
            (void)stream->resume();
    });
    promise->when_rejected([weak_self = make_weak_ptr()](auto& error) {
        dbgln("MediaStreamPlayback: Unable to open an audio output: {}", error);
        if (weak_self)
            weak_self->m_creating_output = false;
    });
}

void MediaStreamPlayback::pause()
{
    if (!m_playing)
        return;
    m_playing = false;
    if (m_playing_since.has_value())
        m_time_played += MonotonicTime::now() - m_playing_since.release_value();
    m_mixer->playing.store(false);

    if (m_output)
        (void)m_output->discard_buffer_and_suspend();

    // A paused live stream skips what arrived in the meantime rather than resuming from where it stopped.
    MutexLocker locker(m_mixer->mutex);
    for (auto& input : m_mixer->inputs) {
        MutexLocker input_locker(input->mutex);
        input->samples.clear();
    }
}

void MediaStreamPlayback::set_volume(double volume)
{
    m_volume = volume;
    if (m_output)
        (void)m_output->set_volume(volume);
}

double MediaStreamPlayback::current_time() const
{
    auto time = m_time_played;
    if (m_playing_since.has_value())
        time += MonotonicTime::now() - *m_playing_since;
    return time.to_seconds_f64();
}

}
