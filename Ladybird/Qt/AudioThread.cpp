/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AudioThread.h"
#include <LibWeb/Platform/AudioCodecPlugin.h>

namespace Ladybird {

struct AudioDevice {
    static AudioDevice create(Audio::Loader const& loader)
    {
        auto const& device_info = QMediaDevices::defaultAudioOutput();

        auto format = device_info.preferredFormat();
        format.setSampleRate(static_cast<int>(loader.sample_rate()));
        format.setChannelCount(2);

        auto audio_output = make<QAudioSink>(device_info, format);
        return AudioDevice { move(audio_output) };
    }

    AudioDevice(AudioDevice&&) = default;

    AudioDevice& operator=(AudioDevice&& device)
    {
        if (audio_output) {
            audio_output->stop();
            io_device = nullptr;
        }

        swap(audio_output, device.audio_output);
        swap(io_device, device.io_device);
        return *this;
    }

    ~AudioDevice()
    {
        if (audio_output)
            audio_output->stop();
    }

    OwnPtr<QAudioSink> audio_output;
    QIODevice* io_device { nullptr };

private:
    explicit AudioDevice(NonnullOwnPtr<QAudioSink> output)
        : audio_output(move(output))
    {
        io_device = audio_output->start();
    }
};

ErrorOr<NonnullOwnPtr<AudioThread>> AudioThread::create(NonnullRefPtr<Audio::Loader> loader)
{
    auto task_queue = TRY(AudioTaskQueue::create());
    return adopt_nonnull_own_or_enomem(new (nothrow) AudioThread(move(loader), move(task_queue)));
}

ErrorOr<void> AudioThread::stop()
{
    TRY(queue_task({ AudioTask::Type::Stop }));
    wait();

    return {};
}

ErrorOr<void> AudioThread::queue_task(AudioTask task)
{
    return m_task_queue.blocking_enqueue(move(task), []() {
        usleep(UPDATE_RATE_MS * 1000);
    });
}

AudioThread::AudioThread(NonnullRefPtr<Audio::Loader> loader, AudioTaskQueue task_queue)
    : m_loader(move(loader))
    , m_task_queue(move(task_queue))
{
    auto duration = static_cast<double>(m_loader->total_samples()) / static_cast<double>(m_loader->sample_rate());
    m_duration = AK::Duration::from_milliseconds(static_cast<i64>(duration * 1000.0));
}

void AudioThread::run()
{
    auto devices = make<QMediaDevices>();
    auto audio_device = AudioDevice::create(m_loader);

    connect(devices, &QMediaDevices::audioOutputsChanged, this, [this]() {
        queue_task({ AudioTask::Type::RecreateAudioDevice }).release_value_but_fixme_should_propagate_errors();
    });

    auto paused = Paused::Yes;

    while (true) {
        auto& audio_output = audio_device.audio_output;
        auto* io_device = audio_device.io_device;

        if (auto result = m_task_queue.dequeue(); result.is_error()) {
            VERIFY(result.error() == AudioTaskQueue::QueueStatus::Empty);
        } else {
            auto task = result.release_value();

            switch (task.type) {
            case AudioTask::Type::Stop:
                return;

            case AudioTask::Type::Play:
                audio_output->resume();
                paused = Paused::No;
                break;

            case AudioTask::Type::Pause:
                audio_output->suspend();
                paused = Paused::Yes;
                break;

            case AudioTask::Type::Seek:
                VERIFY(task.data.has_value());
                m_position = Web::Platform::AudioCodecPlugin::set_loader_position(m_loader, *task.data, m_duration);

                if (paused == Paused::Yes)
                    Q_EMIT playback_position_updated(m_position);

                break;

            case AudioTask::Type::Volume:
                VERIFY(task.data.has_value());
                audio_output->setVolume(*task.data);
                break;

            case AudioTask::Type::RecreateAudioDevice:
                audio_device = AudioDevice::create(m_loader);
                continue;
            }
        }

        if (paused == Paused::No) {
            if (auto result = play_next_samples(*audio_output, *io_device); result.is_error()) {
                // FIXME: Propagate the error to the HTMLMediaElement.
            } else {
                Q_EMIT playback_position_updated(m_position);
                paused = result.value();
            }
        }

        usleep(UPDATE_RATE_MS * 1000);
    }
}

ErrorOr<AudioThread::Paused> AudioThread::play_next_samples(QAudioSink& audio_output, QIODevice& io_device)
{
    bool all_samples_loaded = m_loader->loaded_samples() >= m_loader->total_samples();

    if (all_samples_loaded) {
        audio_output.suspend();
        (void)m_loader->reset();

        m_position = m_duration;
        return Paused::Yes;
    }

    auto bytes_available = audio_output.bytesFree();
    auto bytes_per_sample = audio_output.format().bytesPerSample();
    auto channel_count = audio_output.format().channelCount();
    auto samples_to_load = bytes_available / bytes_per_sample / channel_count;

    auto samples = TRY(Web::Platform::AudioCodecPlugin::read_samples_from_loader(*m_loader, samples_to_load));
    enqueue_samples(audio_output, io_device, move(samples));

    m_position = Web::Platform::AudioCodecPlugin::current_loader_position(m_loader);
    return Paused::No;
}

void AudioThread::enqueue_samples(QAudioSink const& audio_output, QIODevice& io_device, FixedArray<Audio::Sample> samples)
{
    auto buffer_size = samples.size() * audio_output.format().bytesPerSample() * audio_output.format().channelCount();

    if (buffer_size > static_cast<size_t>(m_sample_buffer.size()))
        m_sample_buffer.resize(buffer_size);

    FixedMemoryStream stream { Bytes { m_sample_buffer.data(), buffer_size } };

    for (auto const& sample : samples) {
        switch (audio_output.format().sampleFormat()) {
        case QAudioFormat::UInt8:
            write_sample<u8>(stream, sample.left);
            write_sample<u8>(stream, sample.right);
            break;
        case QAudioFormat::Int16:
            write_sample<i16>(stream, sample.left);
            write_sample<i16>(stream, sample.right);
            break;
        case QAudioFormat::Int32:
            write_sample<i32>(stream, sample.left);
            write_sample<i32>(stream, sample.right);
            break;
        case QAudioFormat::Float:
            write_sample<float>(stream, sample.left);
            write_sample<float>(stream, sample.right);
            break;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    io_device.write(m_sample_buffer.data(), buffer_size);
}

}
