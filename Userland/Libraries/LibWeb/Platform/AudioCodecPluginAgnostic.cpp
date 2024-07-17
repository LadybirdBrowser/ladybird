/*
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/MemoryStream.h>
#include <AK/WeakPtr.h>
#include <LibAudio/Loader.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadedPromise.h>
#include <LibCore/Timer.h>

#include "AudioCodecPluginAgnostic.h"

namespace Web::Platform {

constexpr int update_interval = 50;

static AK::Duration timestamp_from_samples(i64 samples, u32 sample_rate)
{
    return AK::Duration::from_milliseconds(samples * 1000 / sample_rate);
}

static AK::Duration get_loader_timestamp(NonnullRefPtr<Audio::Loader> const& loader)
{
    return timestamp_from_samples(loader->loaded_samples(), loader->sample_rate());
}

ErrorOr<NonnullOwnPtr<AudioCodecPluginAgnostic>> AudioCodecPluginAgnostic::create(NonnullRefPtr<Audio::Loader> const& loader)
{
    auto duration = timestamp_from_samples(loader->total_samples(), loader->sample_rate());

    auto update_timer = Core::Timer::create();
    update_timer->set_interval(update_interval);

    auto plugin = TRY(adopt_nonnull_own_or_enomem(new (nothrow) AudioCodecPluginAgnostic(loader, duration, move(update_timer))));

    constexpr u32 latency_ms = 100;
    // FIXME: Audio loaders are hard-coded to output stereo audio. Once that changes, the channel count provided
    //        below should be retrieved from the audio loader instead of being hard-coded to 2.
    RefPtr<Audio::PlaybackStream> output = TRY(Audio::PlaybackStream::create(
        Audio::OutputState::Suspended, loader->sample_rate(), /* channels = */ 2, latency_ms,
        [&plugin = *plugin, loader](Bytes buffer, Audio::PcmSampleFormat format, size_t sample_count) -> ReadonlyBytes {
            VERIFY(format == Audio::PcmSampleFormat::Float32);

            auto samples_result = loader->get_more_samples(sample_count);

            if (samples_result.is_error()) {
                plugin.on_decoder_error(MUST(String::formatted("Decoding failure: {}", samples_result.error())));
                return buffer.trim(0);
            }

            auto samples = samples_result.release_value();
            VERIFY(samples.size() <= sample_count);

            FixedMemoryStream writing_stream { buffer };

            for (auto& sample : samples) {
                MUST(writing_stream.write_value(sample.left));
                MUST(writing_stream.write_value(sample.right));
            }

            // FIXME: Check if we have loaded samples past the current known duration, and if so, update it
            //        and notify the media element.
            return buffer.trim(writing_stream.offset());
        }));

    output->set_underrun_callback([&plugin = *plugin, loader, output]() {
        auto new_device_time = output->total_time_played().release_value_but_fixme_should_propagate_errors();
        auto new_media_time = timestamp_from_samples(loader->loaded_samples(), loader->sample_rate());
        plugin.m_main_thread_event_loop.deferred_invoke([&plugin, new_device_time, new_media_time]() {
            plugin.m_last_resume_in_device_time = new_device_time;
            plugin.m_last_resume_in_media_time = new_media_time;
        });
    });

    plugin->m_output = move(output);

    return plugin;
}

AudioCodecPluginAgnostic::AudioCodecPluginAgnostic(NonnullRefPtr<Audio::Loader> loader, AK::Duration duration, NonnullRefPtr<Core::Timer> update_timer)
    : m_loader(move(loader))
    , m_duration(duration)
    , m_main_thread_event_loop(Core::EventLoop::current())
    , m_update_timer(move(update_timer))
{
    m_update_timer->on_timeout = [this]() {
        update_timestamp();
    };
}

void AudioCodecPluginAgnostic::resume_playback()
{
    m_paused = false;
    m_output->resume()
        ->when_resolved([this](AK::Duration new_device_time) {
            m_main_thread_event_loop.deferred_invoke([this, new_device_time]() {
                m_last_resume_in_device_time = new_device_time;
                m_update_timer->start();
            });
        })
        .when_rejected([](Error&&) {
            // FIXME: Propagate errors.
        });
}

void AudioCodecPluginAgnostic::pause_playback()
{
    m_paused = true;
    m_output->drain_buffer_and_suspend()
        ->when_resolved([this]() -> ErrorOr<void> {
            auto new_media_time = timestamp_from_samples(m_loader->loaded_samples(), m_loader->sample_rate());
            auto new_device_time = TRY(m_output->total_time_played());
            m_main_thread_event_loop.deferred_invoke([this, new_media_time, new_device_time]() {
                m_last_resume_in_media_time = new_media_time;
                m_last_resume_in_device_time = new_device_time;
                m_update_timer->stop();
                update_timestamp();
            });
            return {};
        })
        .when_rejected([](Error&&) {
            // FIXME: Propagate errors.
        });
}

void AudioCodecPluginAgnostic::set_volume(double volume)
{
    m_output->set_volume(volume)->when_rejected([](Error&&) {
        // FIXME: Propagate errors.
    });
}

void AudioCodecPluginAgnostic::seek(double position)
{
    m_output->discard_buffer_and_suspend()
        ->when_resolved([this, position, was_paused = m_paused]() -> ErrorOr<void> {
            auto sample_position = static_cast<i32>(position * m_loader->sample_rate());
            auto seek_result = m_loader->seek(sample_position);
            if (seek_result.is_error())
                return Error::from_string_literal("Seeking in audio loader failed");

            auto new_media_time = get_loader_timestamp(m_loader);
            auto new_device_time = m_output->total_time_played().release_value_but_fixme_should_propagate_errors();

            m_main_thread_event_loop.deferred_invoke([this, was_paused, new_device_time, new_media_time]() {
                m_last_resume_in_device_time = new_device_time;
                m_last_resume_in_media_time = new_media_time;
                if (was_paused) {
                    update_timestamp();
                } else {
                    m_output->resume()->when_rejected([](Error&&) {
                        // FIXME: Propagate errors.
                    });
                }
            });

            return {};
        })
        .when_rejected([](Error&&) {
            // FIXME: Propagate errors.
        });
}

AK::Duration AudioCodecPluginAgnostic::duration()
{
    return m_duration;
}

void AudioCodecPluginAgnostic::update_timestamp()
{
    auto current_device_time_result = m_output->total_time_played();
    if (!current_device_time_result.is_error())
        m_last_good_device_time = current_device_time_result.release_value();
    auto current_device_time_delta = m_last_good_device_time - m_last_resume_in_device_time;

    auto current_media_time = m_last_resume_in_media_time + current_device_time_delta;
    current_media_time = min(current_media_time, m_duration);
    on_playback_position_updated(current_media_time);
}

}
