/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibAudioServer/SingleSinkSessionClient.h>
#include <LibCore/System.h>
#include <LibMedia/Audio/ChannelMap.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <LibMedia/Sinks/AudioMixingSink.h>
#include <LibThreading/Thread.h>

namespace Media {

static constexpr u32 s_target_latency_ms = 50;
static constexpr i64 s_no_seek = NumericLimits<i64>::min();
static constexpr u64 s_no_output_timing = NumericLimits<u64>::max();

static AK::Duration monotonic_now()
{
    return AK::Duration::from_milliseconds(AK::MonotonicTime::now().milliseconds());
}

static Audio::SampleSpecification sample_specification_for_output_sink(AudioServer::OutputSink const& session)
{
    return Audio::SampleSpecification(
        session.sample_rate,
        session.channel_layout);
}

struct AudioMixingSink::AudioServerOutput {
    explicit AudioServerOutput(NonnullRefPtr<AudioServer::SingleSinkSessionClient> single_sink_session)
        : single_sink_session(move(single_sink_session))
    {
    }

    NonnullRefPtr<AudioServer::SingleSinkSessionClient> single_sink_session;
    Optional<AudioServer::OutputSink> stream;

    RefPtr<Threading::Thread> thread;

    Threading::Mutex session_mutex;
    Threading::Mutex wake_mutex;
    Threading::ConditionVariable wake { wake_mutex };

    u64 wake_generation { 0 };
    Atomic<bool> should_exit { false };
};

AudioMixingSink::TrackMixingData::TrackMixingData(NonnullRefPtr<AudioDataProvider> const& provider)
    : provider(provider)
{
}

AudioMixingSink::AudioMixingSink(NonnullRefPtr<AudioServer::SingleSinkSessionClient> single_sink_session)
    : m_main_thread_event_loop(Core::EventLoop::current())
{
    m_audio_server_output = make<AudioServerOutput>(move(single_sink_session));
}

AudioMixingSink::~AudioMixingSink()
{
    if (m_audio_server_output) {
        m_audio_server_output->should_exit.store(true, AK::MemoryOrder::memory_order_release);
        {
            Threading::MutexLocker locker { m_audio_server_output->wake_mutex };
            ++m_audio_server_output->wake_generation;
            m_audio_server_output->wake.signal();
        }
        if (m_audio_server_output->thread)
            (void)m_audio_server_output->thread->join();

        auto single_sink_session = m_audio_server_output->single_sink_session;
        {
            Threading::MutexLocker locker { m_audio_server_output->session_mutex };
            if (m_audio_server_output->stream.has_value())
                m_audio_server_output->stream.clear();
        }

        (void)single_sink_session->release_output_sink_if_any();

        m_audio_server_output = nullptr;
    }
}

void AudioMixingSink::will_be_destroyed()
{
    revoke_weak_ptrs();
}

ErrorOr<NonnullRefPtr<AudioMixingSink>> AudioMixingSink::try_create()
{
    auto single_sink_session = TRY(AudioServer::SingleSinkSessionClient::try_create());
    auto sink = TRY(try_make_ref_counted<AudioMixingSink>(*single_sink_session));

    sink->m_audio_server_output->thread = Threading::Thread::construct("AudioMixingSink"sv, [self = sink.ptr()] {
        return self->sink_thread_main();
    });

    sink->m_audio_server_output->thread->start();

    return sink;
}

intptr_t AudioMixingSink::sink_thread_main()
{
    AudioServerOutput& output = *m_audio_server_output;
    Vector<float> scratch;

    size_t sample_spec_spin_cycles = 0;
    constexpr size_t WARN_SAMPLE_SPEC_SPIN_CYCLES = 100;  // 100 x 4ms = 400ms at which point we warn
    constexpr size_t CRASH_SAMPLE_SPEC_SPIN_CYCLES = 500; // 500 x 4ms = 2s at which point we crash

    while (!output.should_exit.load(AK::MemoryOrder::memory_order_acquire)) {
        process_control_commands();
        if (!m_playing.load(MemoryOrder::memory_order_acquire)) {
            sample_spec_spin_cycles = 0;
            Threading::MutexLocker locker { output.wake_mutex };
            u64 observed_generation = output.wake_generation;
            output.wake.wait_while([&] {
                return observed_generation == output.wake_generation
                    && !output.should_exit.load(AK::MemoryOrder::memory_order_acquire)
                    && !m_playing.load(MemoryOrder::memory_order_acquire);
            });
            continue;
        }

        Audio::SampleSpecification current_spec = current_sample_specification();

        u8 const channels = current_spec.channel_count();
        if (channels == 0) {
            (void)Core::System::sleep_ms(4);
            ++sample_spec_spin_cycles;
            if (sample_spec_spin_cycles == WARN_SAMPLE_SPEC_SPIN_CYCLES)
                warnln("AudioMixingSink: Output sample spec turnaround exceeded 400ms");
            if (sample_spec_spin_cycles >= CRASH_SAMPLE_SPEC_SPIN_CYCLES)
                VERIFY_NOT_REACHED();
            continue;
        }
        sample_spec_spin_cycles = 0;
        size_t frames_to_write = 512;
        Optional<AudioServer::SharedCircularBuffer*> ring;
        size_t bytes_per_frame = sizeof(float) * channels;
        bool should_sleep_for_backpressure = false;

        Optional<AudioServer::TimingReader::Snapshot> timing_snapshot;
        {
            Threading::MutexLocker session_locker { output.session_mutex };
            if (output.stream.has_value()) {
                ring = &output.stream->ring;
                size_t bytes_free = ring.value()->available_to_write();
                size_t frames_free = bytes_free / bytes_per_frame;
                timing_snapshot = output.stream->timing.read_snapshot();
                if (frames_free < 64) {
                    should_sleep_for_backpressure = true;
                } else {
                    frames_to_write = min<size_t>(frames_free, frames_to_write);
                }
            }
        }

        if (should_sleep_for_backpressure) {
            (void)Core::System::sleep_ms(4);
            continue;
        }

        if (timing_snapshot.has_value()) {
            m_output_device_played_frames.store(timing_snapshot->device_played_frames, MemoryOrder::memory_order_release);
        } else {
            m_output_device_played_frames.store(s_no_output_timing, MemoryOrder::memory_order_release);
        }

        scratch.resize(frames_to_write * channels);

        bool did_mix_audio = false;
        (void)write_audio_data_to_playback_stream(scratch.span(), &did_mix_audio, false);
        if (!did_mix_audio)
            scratch.span().fill(0.0f);

        double const volume = m_volume.load(MemoryOrder::memory_order_acquire);
        if (volume != 1.0) {
            for (auto& sample : scratch)
                sample = static_cast<float>(sample * volume);
        }

        if (ring.has_value()) {
            ReadonlyBytes bytes { reinterpret_cast<u8 const*>(scratch.data()), scratch.size() * sizeof(float) };
            size_t bytes_written = ring.value()->try_write(bytes);
            size_t frames_written = bytes_per_frame > 0 ? (bytes_written / bytes_per_frame) : 0;
            if (frames_written > 0)
                m_next_sample_to_write += static_cast<i64>(frames_written);

            if (frames_written == 0)
                (void)Core::System::sleep_ms(4);
        } else {
            m_next_sample_to_write += static_cast<i64>(frames_to_write);
            auto sleep_ms = static_cast<u32>(ceil(1000.0 * static_cast<double>(frames_to_write) / static_cast<double>(current_spec.sample_rate())));
            if (sleep_ms > 0)
                (void)Core::System::sleep_ms(sleep_ms);
        }
    }

    return 0;
}

AK::Duration AudioMixingSink::current_time() const
{
    Audio::SampleSpecification sample_specification = current_sample_specification();
    if (!sample_specification.is_valid())
        return AK::Duration::zero();

    i64 const current_seek_ms = m_temporary_time.load(MemoryOrder::memory_order_acquire);
    if (current_seek_ms != s_no_seek)
        return AK::Duration::from_milliseconds(current_seek_ms);

    bool const playing = m_playing.load(MemoryOrder::memory_order_acquire);

    i64 next_sample_to_write = m_next_sample_to_write.load(MemoryOrder::memory_order_acquire);
    u64 const output_device_played_frames = m_output_device_played_frames.load(MemoryOrder::memory_order_acquire);
    if (output_device_played_frames != s_no_output_timing) {
        i64 session_base_sample = m_current_session_start_sample.load(MemoryOrder::memory_order_acquire);
        i64 played_sample = max<i64>(0, session_base_sample + static_cast<i64>(output_device_played_frames));

        AK::Duration timing_time = AK::Duration::from_time_units(played_sample, 1, sample_specification.sample_rate());
        AK::Duration max_time = AK::Duration::from_time_units(next_sample_to_write, 1, sample_specification.sample_rate());
        return min(timing_time, max_time);
    }

    if (m_audio_server_output)
        return AK::Duration::from_time_units(max<i64>(0, next_sample_to_write), 1, sample_specification.sample_rate());

    i64 const last_media_time_ms = m_last_media_time.load(MemoryOrder::memory_order_acquire);
    AK::Duration wall_clock_time = AK::Duration::from_milliseconds(last_media_time_ms);
    if (playing) {
        i64 const now_ms = monotonic_now().to_milliseconds();
        i64 const last_stream_time_ms = m_last_stream_time.load(MemoryOrder::memory_order_acquire);
        wall_clock_time += AK::Duration::from_milliseconds(max<i64>(0, now_ms - last_stream_time_ms));
    }

    AK::Duration max_time = AK::Duration::from_time_units(next_sample_to_write, 1, sample_specification.sample_rate());
    return min(max_time, wall_clock_time);
}

void AudioMixingSink::resume()
{
    bool const was_playing = m_playing.exchange(true, MemoryOrder::memory_order_acq_rel);
    if (!was_playing) {
        m_last_stream_time.store(monotonic_now().to_milliseconds(), MemoryOrder::memory_order_release);
        request_output_sink_if_needed();
    }
    wake_sink_thread();
}

void AudioMixingSink::pause()
{
    bool const was_playing = m_playing.exchange(false, MemoryOrder::memory_order_acq_rel);
    if (was_playing) {
        i64 const now_ms = monotonic_now().to_milliseconds();
        i64 const last_stream_time_ms = m_last_stream_time.load(MemoryOrder::memory_order_acquire);
        i64 const last_media_time_ms = m_last_media_time.load(MemoryOrder::memory_order_acquire);
        i64 const elapsed_ms = max<i64>(0, now_ms - last_stream_time_ms);
        m_last_media_time.store(last_media_time_ms + elapsed_ms, MemoryOrder::memory_order_release);
        m_last_stream_time.store(now_ms, MemoryOrder::memory_order_release);
    }
    wake_sink_thread();
}

void AudioMixingSink::process_control_commands()
{
    i64 const set_seek_ms = m_set_seek_ms.exchange(s_no_seek, MemoryOrder::memory_order_acq_rel);
    if (set_seek_ms != s_no_seek)
        apply_time_change(AK::Duration::from_milliseconds(set_seek_ms));
}

void AudioMixingSink::set_time(AK::Duration time)
{
    m_set_seek_ms.store(time.to_milliseconds(), MemoryOrder::memory_order_release);
    wake_sink_thread();
}

void AudioMixingSink::set_volume(double volume)
{
    m_volume.store(volume, MemoryOrder::memory_order_release);
    wake_sink_thread();
}

Audio::SampleSpecification AudioMixingSink::device_sample_specification() const
{
    Threading::MutexLocker locker { m_sample_spec_mutex };
    return m_device_sample_specification;
}

Audio::SampleSpecification AudioMixingSink::current_sample_specification() const
{
    Threading::MutexLocker locker { m_sample_spec_mutex };
    return m_sample_specification;
}

void AudioMixingSink::set_provider(Track const& track, RefPtr<AudioDataProvider> const& provider)
{
    Audio::SampleSpecification sample_specification = current_sample_specification();

    {
        Threading::MutexLocker locker { m_track_mutex };
        m_track_mixing_datas.remove(track);
        if (provider == nullptr)
            return;

        m_track_mixing_datas.set(track, TrackMixingData(*provider));
    }

    if (sample_specification.is_valid()) {
        provider->set_output_sample_specification(sample_specification);
        provider->start();
    } else {
        request_output_sink_if_needed();
    }
}

RefPtr<AudioDataProvider> AudioMixingSink::provider(Track const& track) const
{
    Threading::MutexLocker locker { m_track_mutex };
    auto mixing_data = m_track_mixing_datas.get(track);
    if (!mixing_data.has_value())
        return nullptr;
    return mixing_data->provider;
}

ReadonlySpan<float> AudioMixingSink::write_audio_data_to_playback_stream(Span<float> buffer, bool* did_mix_audio, bool advance_cursor)
{
    Audio::SampleSpecification sample_specification = current_sample_specification();

    VERIFY(sample_specification.is_valid());
    VERIFY(buffer.size() > 0);

    auto channel_count = sample_specification.channel_count();
    auto sample_count = buffer.size() / channel_count;
    buffer.fill(0.0f);

    Threading::MutexLocker mixing_data_locker { m_track_mutex };
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

            if (current_block.sample_specification() != sample_specification) {
                if (!go_to_next_block())
                    break;
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
    Threading::MutexLocker locker { m_track_mutex };
    auto track_data = m_track_mixing_datas.find(track);
    if (track_data == m_track_mixing_datas.end())
        return;
    track_data->value.current_block.clear();
}

void AudioMixingSink::apply_sample_specification(Audio::SampleSpecification sample_specification)
{
    if (!sample_specification.is_valid())
        return;

    AK::Duration current = current_time();

    Vector<NonnullRefPtr<AudioDataProvider>> providers;
    {
        Threading::MutexLocker track_locker { m_track_mutex };
        providers.ensure_capacity(m_track_mixing_datas.size());
    }

    {
        Threading::MutexLocker locker { m_sample_spec_mutex };
        m_sample_specification = sample_specification;
    }

    m_next_sample_to_write.store(current.to_time_units(1, sample_specification.sample_rate()), MemoryOrder::memory_order_release);
    m_last_media_time.store(current.to_milliseconds(), MemoryOrder::memory_order_release);
    m_last_stream_time.store(monotonic_now().to_milliseconds(), MemoryOrder::memory_order_release);
    m_temporary_time.store(s_no_seek, MemoryOrder::memory_order_release);

    {
        Threading::MutexLocker track_locker { m_track_mutex };
        for (auto& [track, track_data] : m_track_mixing_datas) {
            providers.append(track_data.provider);
            track_data.current_block.clear();
        }
    }

    for (auto& provider : providers) {
        provider->set_output_sample_specification(sample_specification);
        provider->start();
    }
}

void AudioMixingSink::wake_sink_thread()
{
    if (!m_audio_server_output)
        return;

    Threading::MutexLocker locker { m_audio_server_output->wake_mutex };
    ++m_audio_server_output->wake_generation;
    m_audio_server_output->wake.signal();
}

void AudioMixingSink::request_output_sink_if_needed()
{
    if (!m_audio_server_output)
        return;

    AudioServerOutput& audio_server_output = *m_audio_server_output;

    {
        Threading::MutexLocker locker { audio_server_output.session_mutex };
        if (audio_server_output.stream.has_value())
            return;
    }

    auto single_sink_session = audio_server_output.single_sink_session;

    auto weak_self = make_weak_ptr();
    m_main_thread_event_loop.deferred_invoke([weak_self, single_sink_session = move(single_sink_session)] {
        auto self = weak_self.strong_ref();
        if (!self || !self->m_audio_server_output)
            return;

        (void)single_sink_session->request_output_sink(
            [weak_self](AudioServer::OutputSink const& output_sink) {
                auto self = weak_self.strong_ref();
                if (!self || !self->m_audio_server_output)
                    return;

                Audio::SampleSpecification device_specification = sample_specification_for_output_sink(output_sink);
                if (!device_specification.is_valid()) {
                    (void)self->m_audio_server_output->single_sink_session->destroy_output_sink();
                    return;
                }

                {
                    Threading::MutexLocker session_locker { self->m_audio_server_output->session_mutex };
                    self->m_audio_server_output->stream = output_sink;
                }

                bool should_apply_new_sample_specification = false;
                Audio::SampleSpecification target_sample_specification = device_specification;
                {
                    Threading::MutexLocker locker { self->m_sample_spec_mutex };
                    self->m_device_sample_specification = device_specification;
                    should_apply_new_sample_specification = self->m_sample_specification != target_sample_specification;
                }

                self->m_current_session_start_sample.store(self->m_next_sample_to_write.load(MemoryOrder::memory_order_acquire), MemoryOrder::memory_order_release);

                if (should_apply_new_sample_specification)
                    self->apply_sample_specification(target_sample_specification);
            },
            [weak_self](u64, ByteString const&) {
                auto self = weak_self.strong_ref();
                if (!self || !self->m_audio_server_output)
                    return;
                {
                    Threading::MutexLocker session_locker { self->m_audio_server_output->session_mutex };
                    self->m_audio_server_output->stream.clear();
                }
            },
            0,
            s_target_latency_ms);
    });
}

void AudioMixingSink::apply_time_change(AK::Duration time)
{
    AudioServerOutput& output = *m_audio_server_output;
    bool was_playing_during_seek = m_playing.load(MemoryOrder::memory_order_acquire);

    m_last_media_time.store(time.to_milliseconds(), MemoryOrder::memory_order_release);
    m_last_stream_time.store(monotonic_now().to_milliseconds(), MemoryOrder::memory_order_release);

    Audio::SampleSpecification sample_specification = current_sample_specification();
    if (sample_specification.is_valid()) {
        m_temporary_time.store(s_no_seek, MemoryOrder::memory_order_release);
        m_next_sample_to_write.store(time.to_time_units(1, sample_specification.sample_rate()), MemoryOrder::memory_order_release);
    } else {
        m_temporary_time.store(time.to_milliseconds(), MemoryOrder::memory_order_release);
        m_next_sample_to_write.store(0, MemoryOrder::memory_order_release);
    }

    i64 const next_sample_to_write = m_next_sample_to_write.load(MemoryOrder::memory_order_acquire);
    i64 current_session_start_sample = next_sample_to_write;
    {
        Threading::MutexLocker session_locker { output.session_mutex };
        if (output.stream.has_value()) {
            Optional<AudioServer::TimingReader::Snapshot> timing_snapshot = output.stream->timing.read_snapshot();
            output.stream->ring.discard_all();

            if (timing_snapshot.has_value()) {
                i64 const device_played_frames = static_cast<i64>(min<u64>(timing_snapshot->device_played_frames, static_cast<u64>(NumericLimits<i64>::max())));
                current_session_start_sample = next_sample_to_write - device_played_frames;
            }
        }
    }
    m_current_session_start_sample.store(current_session_start_sample, MemoryOrder::memory_order_release);

    {
        Threading::MutexLocker track_locker { m_track_mutex };
        for (auto& [track, track_data] : m_track_mixing_datas)
            track_data.current_block.clear();
    }

    m_output_device_played_frames.store(s_no_output_timing, MemoryOrder::memory_order_release);

    if (was_playing_during_seek)
        request_output_sink_if_needed();
}

}
