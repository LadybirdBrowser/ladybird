/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibAudioServerClient/Client.h>
#include <LibCore/System.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibThreading/Thread.h>

#include "AudioMixingSink.h"

namespace Media {

static constexpr u32 s_target_latency_ms = 50;

static AK::Duration monotonic_now()
{
    return AK::Duration::from_milliseconds(AK::MonotonicTime::now().milliseconds());
}

static Audio::ChannelMap channel_map_for_channel_count(u32 channel_count)
{
    if (channel_count == 1)
        return Audio::ChannelMap::mono();
    if (channel_count == 2)
        return Audio::ChannelMap::stereo();
    if (channel_count == 4)
        return Audio::ChannelMap::quadrophonic();
    if (channel_count == 6)
        return Audio::ChannelMap::surround_5_1();
    if (channel_count == 8)
        return Audio::ChannelMap::surround_7_1();

    Vector<Audio::Channel, 8> channels;
    channels.resize(channel_count);
    for (auto& ch : channels)
        ch = Audio::Channel::Unknown;
    return Audio::ChannelMap(channels);
}

struct AudioMixingSink::AudioServerOutput {
    explicit AudioServerOutput(NonnullRefPtr<AudioServerClient::Client> client)
        : client(move(client))
    {
    }

    NonnullRefPtr<AudioServerClient::Client> client;
    Optional<AudioServerClient::Client::AudioOutputSession> session;

    Threading::Mutex session_mutex;

    RefPtr<Threading::Thread> thread;

    Threading::Mutex wake_mutex;
    Threading::ConditionVariable wake { wake_mutex };
    bool wake_requested { false };
    Atomic<bool> should_exit { false };
};

AudioMixingSink::TrackMixingData::TrackMixingData(NonnullRefPtr<AudioDataProvider> const& provider)
    : provider(provider)
{
}

ErrorOr<NonnullRefPtr<AudioMixingSink>> AudioMixingSink::try_create()
{
    auto default_client = AudioServerClient::Client::default_client();
    if (!default_client)
        return Error::from_string_literal("AudioMixingSink: no default AudioServer client");

    auto weak_ref = TRY(try_make_ref_counted<AudioMixingSinkWeakReference>());
    auto sink = TRY(try_make_ref_counted<AudioMixingSink>(weak_ref, *default_client));
    weak_ref->emplace(*sink);

    auto format_or_error = sink->m_audio_server_output->client->get_output_device_format();
    if (format_or_error.is_error())
        return Error::from_string_literal("AudioMixingSink: failed to query output device format");

    auto format = format_or_error.release_value();
    if (format.sample_rate == 0 || format.channel_count == 0)
        return Error::from_string_literal("AudioMixingSink: invalid output device format");

    {
        Threading::MutexLocker locker { sink->m_mutex };
        sink->m_sample_specification = Audio::SampleSpecification(format.sample_rate, channel_map_for_channel_count(format.channel_count));
    }

    auto maybe_session = sink->m_audio_server_output->client->create_audio_output_session(s_target_latency_ms);
    if (maybe_session.is_error())
        return Error::from_string_literal("AudioMixingSink: failed to create audio output session");

    {
        Threading::MutexLocker locker { sink->m_audio_server_output->session_mutex };
        sink->m_audio_server_output->session = maybe_session.release_value();
    }

    sink->m_audio_server_output->thread = Threading::Thread::construct("AudioMixingSink"sv, [weak_self = sink->m_weak_self] {
        auto self = weak_self->take_strong();
        if (!self)
            return 0;

        AudioServerOutput& output = *self->m_audio_server_output;
        Vector<float> scratch;

        while (!output.should_exit.load(AK::MemoryOrder::memory_order_acquire)) {
            if (!self->m_playing) {
                Threading::MutexLocker locker { output.wake_mutex };
                output.wake_requested = false;
                output.wake.wait_while([&] { return !output.wake_requested && !output.should_exit.load(AK::MemoryOrder::memory_order_acquire); });
                continue;
            }

            u8 const channels = self->m_sample_specification.channel_count();
            if (channels == 0) {
                (void)Core::System::sleep_ms(1);
                continue;
            }

            Threading::MutexLocker session_locker { output.session_mutex };
            if (!output.session.has_value()) {
                (void)Core::System::sleep_ms(1);
                continue;
            }

            Core::SharedSingleProducerCircularBuffer& ring = output.session->ring;
            size_t const bytes_per_frame = sizeof(float) * channels;
            size_t bytes_free = ring.available_to_write();
            size_t frames_free = bytes_free / bytes_per_frame;

            if (frames_free < 64) {
                (void)Core::System::sleep_ms(1);
                continue;
            }

            size_t frames_to_write = min<size_t>(frames_free, 512);
            scratch.resize(frames_to_write * channels);

            bool did_mix_audio = false;
            (void)self->write_audio_data_to_playback_stream(scratch.span(), &did_mix_audio, false);
            if (!did_mix_audio)
                scratch.span().fill(0.0f);

            if (self->m_volume != 1.0) {
                double volume = self->m_volume;
                for (auto& sample : scratch)
                    sample = static_cast<float>(sample * volume);
            }

            ReadonlyBytes bytes { reinterpret_cast<u8 const*>(scratch.data()), scratch.size() * sizeof(float) };
            size_t bytes_written = ring.try_write(bytes);
            size_t frames_written = bytes_per_frame > 0 ? (bytes_written / bytes_per_frame) : 0;
            if (frames_written > 0)
                self->m_next_sample_to_write += static_cast<i64>(frames_written);

            if (frames_written == 0)
                (void)Core::System::sleep_ms(1);
        }

        return 0;
    });

    sink->m_audio_server_output->thread->start();

    return sink;
}

AudioMixingSink::AudioMixingSink(AudioMixingSinkWeakReference& weak_ref, NonnullRefPtr<AudioServerClient::Client> client)
    : m_main_thread_event_loop(Core::EventLoop::current())
    , m_weak_self(weak_ref)
{
    m_audio_server_output = make<AudioServerOutput>(move(client));
}

AudioMixingSink::~AudioMixingSink()
{
    if (m_audio_server_output) {
        m_audio_server_output->should_exit.store(true, AK::MemoryOrder::memory_order_release);
        {
            Threading::MutexLocker locker { m_audio_server_output->wake_mutex };
            m_audio_server_output->wake_requested = true;
            m_audio_server_output->wake.signal();
        }
        if (m_audio_server_output->thread)
            (void)m_audio_server_output->thread->join();

        {
            Threading::MutexLocker locker { m_audio_server_output->session_mutex };
            if (m_audio_server_output->session.has_value()) {
                (void)m_audio_server_output->client->destroy_audio_output_session(m_audio_server_output->session->session_id);
                m_audio_server_output->session.clear();
            }
        }

        m_audio_server_output = nullptr;
    }

    m_weak_self->revoke();
}

AK::Duration AudioMixingSink::current_time() const
{
    if (!m_sample_specification.is_valid())
        return AK::Duration::zero();
    if (m_temporary_time.has_value())
        return m_temporary_time.value();

    AK::Duration time = m_last_media_time;
    if (m_playing)
        time += monotonic_now() - m_last_stream_time;

    AK::Duration max_time = AK::Duration::from_time_units(m_next_sample_to_write.load(MemoryOrder::memory_order_acquire), 1, m_sample_specification.sample_rate());
    return min(time, max_time);
}

void AudioMixingSink::resume()
{
    m_playing = true;

    if (!m_audio_server_output)
        return;

    m_last_stream_time = monotonic_now();

    Threading::MutexLocker locker { m_audio_server_output->wake_mutex };
    m_audio_server_output->wake_requested = true;
    m_audio_server_output->wake.signal();
}

void AudioMixingSink::pause()
{
    m_playing = false;

    if (!m_audio_server_output)
        return;

    AK::Duration now = monotonic_now();
    m_main_thread_event_loop.deferred_invoke([self = RefPtr { this }, now]() {
        if (!self)
            return;
        self->m_last_media_time = self->m_last_media_time + (now - self->m_last_stream_time);
        self->m_last_stream_time = now;
    });
}

void AudioMixingSink::set_time(AK::Duration time)
{
    m_temporary_time = time;

    if (!m_audio_server_output)
        return;

    m_main_thread_event_loop.deferred_invoke([self = RefPtr { this }, time]() {
        if (!self)
            return;

        AudioServerOutput& output = *self->m_audio_server_output;

        self->m_last_media_time = time;
        self->m_last_stream_time = monotonic_now();
        self->m_temporary_time = {};

        {
            Threading::MutexLocker mixing_locker { self->m_mutex };
            self->m_next_sample_to_write = time.to_time_units(1, self->m_sample_specification.sample_rate());
            for (auto& [track, track_data] : self->m_track_mixing_datas)
                track_data.current_block.clear();
        }

        Threading::MutexLocker session_locker { output.session_mutex };
        if (!output.session.has_value())
            return;

        u64 old_session_id = output.session->session_id;
        (void)output.client->destroy_audio_output_session(old_session_id);

        auto new_session_or_error = output.client->create_audio_output_session(s_target_latency_ms);
        if (!new_session_or_error.is_error())
            output.session = new_session_or_error.release_value();
    });
}

void AudioMixingSink::set_volume(double volume)
{
    m_volume = volume;
}

void AudioMixingSink::set_provider(Track const& track, RefPtr<AudioDataProvider> const& provider)
{
    Threading::MutexLocker locker { m_mutex };
    m_track_mixing_datas.remove(track);
    if (provider == nullptr)
        return;

    // The provider must have its output sample specification set before it starts decoding, or
    // we'll drop some samples due to a mismatch.
    m_track_mixing_datas.set(track, TrackMixingData(*provider));
    if (m_sample_specification.is_valid()) {
        provider->set_output_sample_specification(m_sample_specification);
        provider->start();
    }
}

RefPtr<AudioDataProvider> AudioMixingSink::provider(Track const& track) const
{
    auto mixing_data = m_track_mixing_datas.get(track);
    if (!mixing_data.has_value())
        return nullptr;
    return mixing_data->provider;
}

ReadonlySpan<float> AudioMixingSink::write_audio_data_to_playback_stream(Span<float> buffer, bool* did_mix_audio, bool advance_cursor)
{
    VERIFY(m_sample_specification.is_valid());
    VERIFY(buffer.size() > 0);

    auto channel_count = m_sample_specification.channel_count();
    auto sample_count = buffer.size() / channel_count;
    buffer.fill(0.0f);

    Threading::MutexLocker mixing_data_locker { m_mutex };
    auto buffer_start = m_next_sample_to_write.load();

    bool mixed_any_samples = false;

    for (auto& [track, track_data] : m_track_mixing_datas) {
        auto next_sample = buffer_start;
        auto samples_end = next_sample + static_cast<i64>(sample_count);

        auto go_to_next_block = [&] {
            auto new_block = track_data.provider->retrieve_block();
            if (new_block.is_empty())
                return false;

            track_data.current_block = move(new_block);
            return true;
        };

        if (track_data.current_block.is_empty()) {
            if (!go_to_next_block())
                continue;
        }

        while (!track_data.current_block.is_empty()) {
            auto& current_block = track_data.current_block;
            auto current_block_sample_count = static_cast<i64>(current_block.sample_count());

            if (current_block.sample_specification() != m_sample_specification) {
                if (!go_to_next_block())
                    break;
                current_block.clear();
                continue;
            }

            auto first_sample_offset = current_block.timestamp_in_samples();
            if (first_sample_offset >= samples_end)
                break;

            auto block_end = first_sample_offset + current_block_sample_count;
            if (block_end <= next_sample) {
                if (!go_to_next_block())
                    break;
                continue;
            }

            next_sample = max(next_sample, first_sample_offset);

            VERIFY(next_sample >= first_sample_offset);
            auto index_in_block = static_cast<size_t>((next_sample - first_sample_offset) * channel_count);
            VERIFY(index_in_block < current_block.data_count());

            VERIFY(next_sample >= buffer_start);
            auto index_in_buffer = static_cast<size_t>((next_sample - buffer_start) * channel_count);
            VERIFY(index_in_buffer < buffer.size());

            VERIFY(current_block.data_count() >= index_in_block);
            auto write_count = current_block.data_count() - index_in_block;
            write_count = min(write_count, buffer.size() - index_in_buffer);
            VERIFY(write_count > 0);
            VERIFY(index_in_buffer + write_count <= buffer.size());
            VERIFY(write_count % channel_count == 0);

            for (size_t i = 0; i < write_count; i++)
                buffer[index_in_buffer + i] += current_block.data()[index_in_block + i];

            mixed_any_samples = true;

            auto write_end = index_in_block + write_count;
            if (write_end == current_block.data_count()) {
                if (!go_to_next_block())
                    break;
                continue;
            }
            VERIFY(write_end < current_block.data_count());

            next_sample += static_cast<i64>(write_count / channel_count);
            if (next_sample == samples_end)
                break;
            VERIFY(next_sample < samples_end);
        }
    }

    if (did_mix_audio)
        *did_mix_audio = mixed_any_samples;

    if (advance_cursor)
        m_next_sample_to_write += static_cast<i64>(sample_count);
    return buffer;
}

void AudioMixingSink::clear_track_data(Track const& track)
{
    Threading::MutexLocker locker { m_mutex };
    auto track_data = m_track_mixing_datas.find(track);
    if (track_data == m_track_mixing_datas.end())
        return;
    track_data->value.current_block.clear();
}

}
