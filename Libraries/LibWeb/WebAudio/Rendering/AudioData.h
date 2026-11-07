/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once
#include <AK/AtomicRefCounted.h>
#include <AK/Span.h>
#include <AK/Vector.h>
namespace Web::WebAudio::Rendering {

struct AudioBufferContents : public AtomicRefCounted<AudioBufferContents> {
    AudioBufferContents(Vector<Vector<float>> channels, float sample_rate);
    ~AudioBufferContents();
    size_t length() const { return channels.is_empty() ? 0 : channels.first().size(); }
    double duration() const { return length() / static_cast<double>(sample_rate); }
    void const* rust_data() const { return m_data; }
    Vector<ReadonlySpan<float>> channels;
    float sample_rate { 0 };

private:
    void const* m_data { nullptr };
};
struct WaveShaperCurve : public AtomicRefCounted<WaveShaperCurve> {
    explicit WaveShaperCurve(Vector<float> values);
    ~WaveShaperCurve();
    void const* rust_data() const { return m_data; }
    ReadonlySpan<float> values;

private:
    void const* m_data { nullptr };
};
struct PeriodicWaveData : public AtomicRefCounted<PeriodicWaveData> {
    PeriodicWaveData(Vector<float> real, Vector<float> imag, bool normalize);
    PeriodicWaveData(void const* data, bool normalize);
    ~PeriodicWaveData();
    void const* rust_data() const { return m_data; }
    ReadonlySpan<float> real;
    ReadonlySpan<float> imag;
    bool normalize { true };

private:
    void const* m_data { nullptr };
};

}
