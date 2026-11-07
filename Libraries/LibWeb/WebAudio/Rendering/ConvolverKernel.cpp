/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/ConvolverKernel.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

RefPtr<ConvolverKernel> ConvolverKernel::create(AudioBufferContents const& contents, size_t partition_size, bool normalize)
{
    Vector<ReadonlySpan<float>> channels;
    channels.ensure_capacity(contents.channels.size());
    for (auto const& channel : contents.channels)
        channels.unchecked_append(channel);
    auto const* kernel = RustFFI::web_audio_convolver_kernel_create(channels.span(), contents.sample_rate, partition_size, normalize);
    if (!kernel)
        return nullptr;
    return adopt_ref(*new ConvolverKernel(kernel));
}

ConvolverKernel::~ConvolverKernel()
{
    RustFFI::web_audio_convolver_kernel_destroy(m_kernel);
}

size_t ConvolverKernel::channel_count() const
{
    return RustFFI::web_audio_convolver_kernel_channel_count(m_kernel);
}

size_t ConvolverKernel::partition_count() const
{
    return RustFFI::web_audio_convolver_kernel_partition_count(m_kernel);
}

size_t ConvolverKernel::bin_count() const
{
    return RustFFI::web_audio_convolver_kernel_bin_count(m_kernel);
}

ReadonlySpan<float> ConvolverKernel::head(size_t channel) const
{
    ReadonlySpan<float> samples;
    RustFFI::web_audio_convolver_kernel_head(m_kernel, channel, &samples);
    return samples;
}

ReadonlySpan<float> ConvolverKernel::partition_real(size_t channel, size_t partition) const
{
    ReadonlySpan<float> samples;
    RustFFI::web_audio_convolver_kernel_partition_real(m_kernel, channel, partition, &samples);
    return samples;
}

ReadonlySpan<float> ConvolverKernel::partition_imag(size_t channel, size_t partition) const
{
    ReadonlySpan<float> samples;
    RustFFI::web_audio_convolver_kernel_partition_imag(m_kernel, channel, partition, &samples);
    return samples;
}

NonnullRefPtr<ConvolverDelayLine> ConvolverDelayLine::create(ConvolverKernel const& kernel)
{
    return adopt_ref(*new ConvolverDelayLine(RustFFI::web_audio_convolver_delay_line_create(kernel.m_kernel)));
}

ConvolverDelayLine::~ConvolverDelayLine()
{
    RustFFI::web_audio_convolver_delay_line_destroy(m_delay_line);
}

size_t ConvolverDelayLine::length() const
{
    return RustFFI::web_audio_convolver_delay_line_length(m_delay_line);
}

Span<float> ConvolverDelayLine::real(size_t channel)
{
    Span<float> samples;
    RustFFI::web_audio_convolver_delay_line_real(m_delay_line, channel, &samples);
    return samples;
}

Span<float> ConvolverDelayLine::imag(size_t channel)
{
    Span<float> samples;
    RustFFI::web_audio_convolver_delay_line_imag(m_delay_line, channel, &samples);
    return samples;
}

}
