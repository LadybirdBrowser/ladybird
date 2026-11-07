/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
#include <AK/Span.h>
#include <LibWeb/WebAudio/Rendering/AudioData.h>

namespace Web::WebAudio::Rendering {

class ConvolverKernel : public AtomicRefCounted<ConvolverKernel> {
public:
    static RefPtr<ConvolverKernel> create(AudioBufferContents const&, size_t partition_size, bool normalize);
    ~ConvolverKernel();

    size_t channel_count() const;
    size_t partition_count() const;
    size_t bin_count() const;
    ReadonlySpan<float> head(size_t channel) const;
    ReadonlySpan<float> partition_real(size_t channel, size_t partition) const;
    ReadonlySpan<float> partition_imag(size_t channel, size_t partition) const;

private:
    friend class ConvolverDelayLine;

    explicit ConvolverKernel(void const* kernel)
        : m_kernel(kernel)
    {
    }

    void const* m_kernel { nullptr };
};

class ConvolverDelayLine : public AtomicRefCounted<ConvolverDelayLine> {
public:
    static constexpr size_t MAX_CHANNEL_COUNT = 2;

    static NonnullRefPtr<ConvolverDelayLine> create(ConvolverKernel const&);
    ~ConvolverDelayLine();

    size_t length() const;
    Span<float> real(size_t channel);
    Span<float> imag(size_t channel);

private:
    explicit ConvolverDelayLine(void* delay_line)
        : m_delay_line(delay_line)
    {
    }

    void* m_delay_line { nullptr };
};

}
