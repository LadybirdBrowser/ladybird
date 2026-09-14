/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/AtomicRefCounted.h>
#include <AK/FixedArray.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <AK/Vector.h>
#include <LibWeb/WebAudio/Rendering/AudioData.h>

namespace Web::WebAudio::Rendering {

// An impulse response prepared for convolution on the rendering thread, built on the control thread when a
// ConvolverNode's buffer is set so the rendering thread never has to transform a whole impulse response in a single
// quantum.
//
// The first partition is kept in the time domain: it is the one that carries the quietest part of the output, where the
// round-off of a transform round trip is most audible. Everything after it is cut into equally sized partitions and
// transformed. Only the first half of each partition's spectrum plus the Nyquist bin is stored; the remaining bins
// follow from the Hermitian symmetry of the transform of a real signal.
// https://webaudio.github.io/web-audio-api/#ConvolverNode
class ConvolverKernel : public AtomicRefCounted<ConvolverKernel> {
public:
    static RefPtr<ConvolverKernel> create(AudioBufferContents const&, size_t partition_size, bool normalize);

    size_t channel_count() const { return m_channels.size(); }

    // The number of partitions that are held in the frequency domain, which excludes the time-domain head.
    size_t partition_count() const { return m_partition_count; }
    size_t bin_count() const { return m_bin_count; }

    ReadonlySpan<float> head(size_t channel) const { return m_channels[channel].head; }

    ReadonlySpan<float> partition_real(size_t channel, size_t partition) const
    {
        return m_channels[channel].real.span().slice(partition * m_bin_count, m_bin_count);
    }

    ReadonlySpan<float> partition_imag(size_t channel, size_t partition) const
    {
        return m_channels[channel].imag.span().slice(partition * m_bin_count, m_bin_count);
    }

private:
    struct Channel {
        Vector<float> head;
        Vector<float> real;
        Vector<float> imag;
    };

    ConvolverKernel(Vector<Channel> channels, size_t partition_count, size_t bin_count)
        : m_channels(move(channels))
        , m_partition_count(partition_count)
        , m_bin_count(bin_count)
    {
    }

    Vector<Channel> m_channels;
    size_t m_partition_count { 0 };
    size_t m_bin_count { 0 };
};

// The ring of input spectra a ConvolverRenderNode multiplies with the kernel's partitions, one slot per partition plus
// one for the window written in the current quantum. It is sized after the kernel, so it is allocated here on the
// control thread and handed over together with the kernel, keeping allocation off the rendering thread.
class ConvolverDelayLine : public AtomicRefCounted<ConvolverDelayLine> {
public:
    // The input to a ConvolverNode is mono or stereo and cannot be increased.
    // https://webaudio.github.io/web-audio-api/#ConvolverNode
    static constexpr size_t MAX_CHANNEL_COUNT = 2;

    static NonnullRefPtr<ConvolverDelayLine> create(ConvolverKernel const&);

    size_t length() const { return m_length; }

    Span<float> real(size_t channel) { return m_real[channel].span(); }
    Span<float> imag(size_t channel) { return m_imag[channel].span(); }

private:
    ConvolverDelayLine(size_t length, Array<FixedArray<float>, MAX_CHANNEL_COUNT> real, Array<FixedArray<float>, MAX_CHANNEL_COUNT> imag)
        : m_length(length)
        , m_real(move(real))
        , m_imag(move(imag))
    {
    }

    size_t m_length { 0 };
    Array<FixedArray<float>, MAX_CHANNEL_COUNT> m_real;
    Array<FixedArray<float>, MAX_CHANNEL_COUNT> m_imag;
};

}
