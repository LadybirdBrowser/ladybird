/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibTest/TestCase.h>

// A device stream the test drives by hand: it records control calls and renders a callback on demand.
class FakeDeviceStream final : public Audio::PlaybackStream {
public:
    static constexpr u32 SAMPLE_RATE = 48000;
    static constexpr u32 CHANNEL_COUNT = 2;

    FakeDeviceStream(Audio::OutputState initial_state, AudioDataRequestCallback callback)
        : m_playing(initial_state == Audio::OutputState::Playing)
        , m_callback(move(callback))
    {
    }

    Audio::SampleSpecification sample_specification() const override { return { SAMPLE_RATE, Audio::ChannelMap::stereo() }; }

    NonnullRefPtr<Core::ThreadedPromise<void>> resume() override
    {
        m_playing = true;
        resume_count++;
        auto promise = Core::ThreadedPromise<void>::create();
        promise->resolve();
        return promise;
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> drain_buffer_and_suspend() override
    {
        m_playing = false;
        drain_count++;
        auto promise = Core::ThreadedPromise<void>::create();
        promise->resolve();
        return promise;
    }

    NonnullRefPtr<Core::ThreadedPromise<void>> discard_buffer_and_suspend() override
    {
        m_playing = false;
        auto promise = Core::ThreadedPromise<void>::create();
        promise->resolve();
        return promise;
    }

    void notify_data_available() override { }

    NonnullRefPtr<Core::ThreadedPromise<void>> set_volume(double) override
    {
        auto promise = Core::ThreadedPromise<void>::create();
        promise->resolve();
        return promise;
    }

    bool is_playing() const { return m_playing; }

    // One device callback worth of output.
    Vector<float> render(size_t frame_count)
    {
        Vector<float> buffer;
        buffer.resize(frame_count * CHANNEL_COUNT);
        auto written = m_callback(buffer, MonotonicTime::now() + latency);
        EXPECT_EQ(written.size(), buffer.size());
        return buffer;
    }

    size_t resume_count { 0 };
    size_t drain_count { 0 };
    // What the device claims to hold ahead of the next render.
    AK::Duration latency;

private:
    bool m_playing { false };
    AudioDataRequestCallback m_callback;
};
