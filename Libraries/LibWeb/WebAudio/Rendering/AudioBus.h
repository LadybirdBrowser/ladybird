/*
 * Copyright (c) 2026, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Span.h>
#include <LibWeb/Bindings/AudioNode.h>
#include <LibWeb/Export.h>

namespace Web::WebAudio::Rendering {

// A block of audio samples for a single render quantum, holding one buffer of frames per channel.
class WEB_API AudioBus {
public:
    AK_ALLOC_WITH_KMALLOC;

    AudioBus(size_t channel_count, size_t frame_count);
    AudioBus(AudioBus const&);
    AudioBus(AudioBus&&);
    AudioBus& operator=(AudioBus const&);
    AudioBus& operator=(AudioBus&&);
    ~AudioBus();

    size_t channel_count() const;
    size_t frame_count() const;

    Span<float> channel(size_t index);
    ReadonlySpan<float> channel(size_t index) const;

    void const* rust_bus() const { return m_bus; }

    static AudioBus borrow(void*);

    void zero();
    bool is_silent() const;
    void set_channel_count(size_t);
    void copy_from(AudioBus const&);

    void sum_from(AudioBus const& source, Bindings::ChannelInterpretation);

private:
    explicit AudioBus(void* bus)
        : m_bus(bus)
        , m_owned(false)
    {
    }
    void* m_bus { nullptr };
    bool m_owned { true };
};

}
