/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StdLibExtras.h>
#include <LibWeb/WebAudio/Rendering/AudioBus.h>
#include <LibWeb/WebAudio/RustFFI.h>

namespace Web::WebAudio::Rendering {

AudioBus::AudioBus(size_t channel_count, size_t frame_count)
    : m_bus(RustFFI::web_audio_bus_create(channel_count, frame_count))
{
}

AudioBus::AudioBus(AudioBus const& other)
    : m_bus(RustFFI::web_audio_bus_clone(other.m_bus))
{
}

AudioBus::AudioBus(AudioBus&& other)
    : m_bus(exchange(other.m_bus, nullptr))
    , m_owned(other.m_owned)
{
}

AudioBus& AudioBus::operator=(AudioBus const& other)
{
    if (this != &other) {
        auto* copy = RustFFI::web_audio_bus_clone(other.m_bus);
        if (m_owned)
            RustFFI::web_audio_bus_destroy(m_bus);
        m_bus = copy;
        m_owned = true;
    }
    return *this;
}

AudioBus& AudioBus::operator=(AudioBus&& other)
{
    if (this != &other) {
        if (m_owned)
            RustFFI::web_audio_bus_destroy(m_bus);
        m_bus = exchange(other.m_bus, nullptr);
        m_owned = other.m_owned;
    }
    return *this;
}

AudioBus::~AudioBus()
{
    if (m_owned)
        RustFFI::web_audio_bus_destroy(m_bus);
}

AudioBus AudioBus::borrow(void* bus)
{
    return AudioBus(bus);
}

size_t AudioBus::channel_count() const
{
    return RustFFI::web_audio_bus_channel_count(m_bus);
}

size_t AudioBus::frame_count() const
{
    return RustFFI::web_audio_bus_frame_count(m_bus);
}

Span<float> AudioBus::channel(size_t index)
{
    Span<float> samples;
    RustFFI::web_audio_bus_channel_mut(m_bus, index, &samples);
    return samples;
}

ReadonlySpan<float> AudioBus::channel(size_t index) const
{
    ReadonlySpan<float> samples;
    RustFFI::web_audio_bus_channel(m_bus, index, &samples);
    return samples;
}

void AudioBus::zero()
{
    RustFFI::web_audio_bus_zero(m_bus);
}

bool AudioBus::is_silent() const
{
    return RustFFI::web_audio_bus_is_silent(m_bus);
}

void AudioBus::set_channel_count(size_t channel_count)
{
    RustFFI::web_audio_bus_set_channel_count(m_bus, channel_count);
}

void AudioBus::copy_from(AudioBus const& source)
{
    RustFFI::web_audio_bus_copy_from(m_bus, source.m_bus);
}

void AudioBus::sum_from(AudioBus const& source, Bindings::ChannelInterpretation interpretation)
{
    RustFFI::web_audio_bus_sum_from(m_bus, source.m_bus, interpretation == Bindings::ChannelInterpretation::Speakers);
}

}
