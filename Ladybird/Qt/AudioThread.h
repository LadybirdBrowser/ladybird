/*
 * Copyright (c) 2023, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Endian.h>
#include <AK/MemoryStream.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibAudio/Loader.h>
#include <LibAudio/Sample.h>
#include <LibCore/SharedCircularQueue.h>
#include <QAudioFormat>
#include <QAudioSink>
#include <QByteArray>
#include <QMediaDevices>
#include <QThread>

namespace Ladybird {

static constexpr u32 UPDATE_RATE_MS = 10;

struct AudioTask {
    enum class Type {
        Stop,
        Play,
        Pause,
        Seek,
        Volume,
        RecreateAudioDevice,
    };

    Type type;
    Optional<double> data {};
};

using AudioTaskQueue = Core::SharedSingleProducerCircularQueue<AudioTask>;

class AudioThread final : public QThread { // We have to use QThread, otherwise internal Qt media QTimer objects do not work.
    Q_OBJECT

public:
    static ErrorOr<NonnullOwnPtr<AudioThread>> create(NonnullRefPtr<Audio::Loader> loader);

    ErrorOr<void> stop();

    AK::Duration duration() const { return m_duration; }

    ErrorOr<void> queue_task(AudioTask task);

Q_SIGNALS:
    void playback_position_updated(AK::Duration);

private:
    AudioThread(NonnullRefPtr<Audio::Loader> loader, AudioTaskQueue task_queue);

    enum class Paused {
        Yes,
        No,
    };

    void run() override;

    ErrorOr<Paused> play_next_samples(QAudioSink& audio_output, QIODevice& io_device);

    void enqueue_samples(QAudioSink const& audio_output, QIODevice& io_device, FixedArray<Audio::Sample> samples);

    template<typename T>
    void write_sample(FixedMemoryStream& stream, float sample)
    {
        // The values that need to be written to the stream vary depending on the output channel format, and isn't
        // particularly well documented. The value derivations performed below were adapted from a Qt example:
        // https://code.qt.io/cgit/qt/qtmultimedia.git/tree/examples/multimedia/audiooutput/audiooutput.cpp?h=6.4.2#n46
        LittleEndian<T> pcm;

        if constexpr (IsSame<T, u8>)
            pcm = static_cast<u8>((sample + 1.0f) / 2 * NumericLimits<u8>::max());
        else if constexpr (IsSame<T, i16>)
            pcm = static_cast<i16>(sample * NumericLimits<i16>::max());
        else if constexpr (IsSame<T, i32>)
            pcm = static_cast<i32>(sample * NumericLimits<i32>::max());
        else if constexpr (IsSame<T, float>)
            pcm = sample;
        else
            static_assert(DependentFalse<T>);

        MUST(stream.write_value(pcm));
    }

    NonnullRefPtr<Audio::Loader> m_loader;
    AudioTaskQueue m_task_queue;

    QByteArray m_sample_buffer;

    AK::Duration m_duration;
    AK::Duration m_position;
};

}
