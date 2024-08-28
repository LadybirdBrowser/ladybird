/*
 * Copyright (c) 2024, Olekoop <mlglol360xd@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "PlaybackStreamOboe.h"
#include <AK/Atomic.h>
#include <AK/SourceLocation.h>
#include <LibCore/SharedCircularQueue.h>
#include <LibCore/ThreadedPromise.h>
#include <memory>

#include <oboe/Oboe.h>

namespace Audio {

class OboeCallback : public oboe::AudioStreamDataCallback {
public:
    virtual oboe::DataCallbackResult onAudioReady(oboe::AudioStream* oboeStream, void* audioData, int32_t numFrames) override
    {
        Bytes output_buffer {
            reinterpret_cast<u8*>(audioData),
            static_cast<size_t>(numFrames * oboeStream->getChannelCount() * sizeof(float))
        };
        auto written_bytes = m_data_request_callback(output_buffer, PcmSampleFormat::Float32, numFrames);
        if (written_bytes.is_empty())
            return oboe::DataCallbackResult::Stop;

        auto timestamp = oboeStream->getTimestamp(CLOCK_MONOTONIC);
        if (timestamp == oboe::Result::OK) {
            m_number_of_samples_enqueued = timestamp.value().position;
        } else {
            // Fallback for OpenSLES
            m_number_of_samples_enqueued += numFrames;
        }
        auto last_sample_time = static_cast<i64>(m_number_of_samples_enqueued / oboeStream->getSampleRate());
        m_last_sample_time.store(last_sample_time);

        float* output = (float*)audioData;
        for (int frames = 0; frames < numFrames; frames++) {
            for (int channels = 0; channels < oboeStream->getChannelCount(); channels++) {
                *output++ *= m_volume.load();
            }
        }
        return oboe::DataCallbackResult::Continue;
    }
    OboeCallback(PlaybackStream::AudioDataRequestCallback data_request_callback)
        : m_data_request_callback(move(data_request_callback))
    {
    }
    AK::Duration last_sample_time() const
    {
        return AK::Duration::from_seconds(m_last_sample_time.load());
    }
    void set_volume(float volume)
    {
        m_volume.store(volume);
    }

private:
    PlaybackStream::AudioDataRequestCallback m_data_request_callback;
    Atomic<i64> m_last_sample_time { 0 };
    size_t m_number_of_samples_enqueued { 0 };
    Atomic<float> m_volume { 1.0 };
};

class PlaybackStreamOboe::Storage : public RefCounted<PlaybackStreamOboe::Storage> {
public:
    Storage(std::shared_ptr<oboe::AudioStream> stream, std::shared_ptr<OboeCallback> oboe_callback)
        : m_stream(move(stream))
        , m_oboe_callback(move(oboe_callback))
    {
    }
    std::shared_ptr<oboe::AudioStream> stream() const { return m_stream; }
    std::shared_ptr<OboeCallback> oboe_callback() const { return m_oboe_callback; }

private:
    std::shared_ptr<oboe::AudioStream> m_stream;
    std::shared_ptr<OboeCallback> m_oboe_callback;
};

PlaybackStreamOboe::PlaybackStreamOboe(NonnullRefPtr<Storage> storage)
    : m_storage(move(storage))
{
}

ErrorOr<NonnullRefPtr<PlaybackStream>> PlaybackStreamOboe::create(OutputState initial_output_state, u32 sample_rate, u8 channels, u32, AudioDataRequestCallback&& data_request_callback)
{
    std::shared_ptr<oboe::AudioStream> stream;
    auto oboe_callback = std::make_shared<OboeCallback>(move(data_request_callback));
    oboe::AudioStreamBuilder builder;
    auto result = builder.setSharingMode(oboe::SharingMode::Shared)
                      ->setPerformanceMode(oboe::PerformanceMode::LowLatency)
                      ->setFormat(oboe::AudioFormat::Float)
                      ->setDataCallback(oboe_callback)
                      ->setChannelCount(channels)
                      ->setSampleRate(sample_rate)
                      ->openStream(stream);

    if (result != oboe::Result::OK)
        return Error::from_string_literal("Oboe failed to start");

    if (initial_output_state == OutputState::Playing)
        stream->requestStart();

    auto storage = TRY(adopt_nonnull_ref_or_enomem(new PlaybackStreamOboe::Storage(move(stream), move(oboe_callback))));
    return TRY(adopt_nonnull_ref_or_enomem(new (nothrow) PlaybackStreamOboe(move(storage))));
}

PlaybackStreamOboe::~PlaybackStreamOboe() = default;

void PlaybackStreamOboe::set_underrun_callback(Function<void()>)
{
    // FIXME: Implement this.
}

NonnullRefPtr<Core::ThreadedPromise<AK::Duration>> PlaybackStreamOboe::resume()
{
    auto promise = Core::ThreadedPromise<AK::Duration>::create();
    auto time = MUST(total_time_played());
    m_storage->stream()->start();
    promise->resolve(move(time));
    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamOboe::drain_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    m_storage->stream()->stop();
    promise->resolve();
    return promise;
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamOboe::discard_buffer_and_suspend()
{
    auto promise = Core::ThreadedPromise<void>::create();
    m_storage->stream()->pause();
    m_storage->stream()->flush();
    promise->resolve();
    return promise;
}

ErrorOr<AK::Duration> PlaybackStreamOboe::total_time_played()
{
    return m_storage->oboe_callback()->last_sample_time();
}

NonnullRefPtr<Core::ThreadedPromise<void>> PlaybackStreamOboe::set_volume(double volume)
{
    auto promise = Core::ThreadedPromise<void>::create();
    m_storage->oboe_callback()->set_volume(volume);
    promise->resolve();
    return promise;
}

}
