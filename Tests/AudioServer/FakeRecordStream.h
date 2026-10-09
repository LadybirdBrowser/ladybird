/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Vector.h>
#include <LibAudio/RecordStream.h>

// A capture stream the test feeds by hand.
class FakeRecordStream final : public Audio::RecordStream {
public:
    static constexpr u32 SAMPLE_RATE = 48000;
    static constexpr u32 CHANNEL_COUNT = 2;

    inline static Atomic<size_t> live_count { 0 };

    FakeRecordStream(Audio::SampleSpecification const& specification, RecordCallback callback)
        : m_specification(specification)
        , m_callback(move(callback))
    {
        live_count++;
    }
    virtual ~FakeRecordStream() override { live_count--; }

    virtual Audio::SampleSpecification const& sample_specification() const override { return m_specification; }

    // One device callback worth of input, every sample set to `value`.
    void capture(size_t frame_count, float value)
    {
        Vector<float> samples;
        samples.resize(frame_count * m_specification.channel_count());
        samples.fill(value);
        m_callback(ReadonlyBytes { reinterpret_cast<u8 const*>(samples.data()), samples.size() * sizeof(float) }, m_specification);
    }

private:
    Audio::SampleSpecification m_specification;
    RecordCallback m_callback;
};
