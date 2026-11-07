/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/WebAudio/Rendering/AudioData.h>
#include <LibWeb/WebAudio/RustFFI.h>
namespace Web::WebAudio::Rendering {

AudioBufferContents::AudioBufferContents(Vector<Vector<float>> samples, float rate)
    : sample_rate(rate)
{
    Vector<ReadonlySpan<float>> views;
    for (auto const& channel : samples)
        views.append(channel.span());
    m_data = RustFFI::web_audio_buffer_contents_create(views.span(), rate);
    channels.resize(samples.size());
    for (size_t i = 0; i < samples.size(); ++i)
        RustFFI::web_audio_buffer_contents_channel(m_data, i, &channels[i]);
}
AudioBufferContents::~AudioBufferContents() { RustFFI::web_audio_buffer_contents_destroy(m_data); }
WaveShaperCurve::WaveShaperCurve(Vector<float> samples)
{
    m_data = RustFFI::web_audio_wave_shaper_curve_create(samples.span());
    RustFFI::web_audio_wave_shaper_curve_values(m_data, &values);
}
WaveShaperCurve::~WaveShaperCurve() { RustFFI::web_audio_wave_shaper_curve_destroy(m_data); }
PeriodicWaveData::PeriodicWaveData(Vector<float> real_values, Vector<float> imag_values, bool should_normalize)
    : normalize(should_normalize)
{
    m_data = RustFFI::web_audio_periodic_wave_data_create(real_values.span(), imag_values.span(), should_normalize);
    RustFFI::web_audio_periodic_wave_data_real(m_data, &real);
    RustFFI::web_audio_periodic_wave_data_imag(m_data, &imag);
}
PeriodicWaveData::PeriodicWaveData(void const* data, bool should_normalize)
    : normalize(should_normalize)
    , m_data(data)
{
    RustFFI::web_audio_periodic_wave_data_real(m_data, &real);
    RustFFI::web_audio_periodic_wave_data_imag(m_data, &imag);
}
PeriodicWaveData::~PeriodicWaveData() { RustFFI::web_audio_periodic_wave_data_destroy(m_data); }

}
