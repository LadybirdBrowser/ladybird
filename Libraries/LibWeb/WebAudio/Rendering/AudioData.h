/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Vector.h>

namespace Web::WebAudio::Rendering {

// An immutable snapshot of an AudioBuffer's sample data, created on the control thread when the buffer's content is
// acquired and shared with the rendering thread.
// https://webaudio.github.io/web-audio-api/#acquire-the-content
struct AudioBufferContents : public AtomicRefCounted<AudioBufferContents> {
    AudioBufferContents(Vector<Vector<float>> channels, float sample_rate)
        : channels(move(channels))
        , sample_rate(sample_rate)
    {
    }

    size_t length() const { return channels.is_empty() ? 0 : channels.first().size(); }
    double duration() const { return length() / static_cast<double>(sample_rate); }

    Vector<Vector<float>> channels;
    float sample_rate { 0 };
};

// An immutable snapshot of a PeriodicWave's Fourier coefficients.
// https://webaudio.github.io/web-audio-api/#PeriodicWave
struct PeriodicWaveData : public AtomicRefCounted<PeriodicWaveData> {
    PeriodicWaveData(Vector<float> real, Vector<float> imag, bool normalize)
        : real(move(real))
        , imag(move(imag))
        , normalize(normalize)
    {
    }

    Vector<float> real;
    Vector<float> imag;
    bool normalize { true };
};

}
