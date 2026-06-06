/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Vector.h>
#include <LibGC/Weak.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioBuffer.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

class WindowOrWorkerGlobalScopeMixin;

}

namespace Web::WebAudio {

// https://webaudio.github.io/web-audio-api/#AudioBuffer
class AudioBuffer final : public Bindings::Wrappable {
    WEB_WRAPPABLE(AudioBuffer, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(AudioBuffer);

public:
    static ErrorOr<GC::Ref<AudioBuffer>> create(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);
    static WebIDL::ExceptionOr<GC::Ref<AudioBuffer>> construct_impl(HTML::WindowOrWorkerGlobalScopeMixin&, Bindings::AudioBufferOptions const&);

    virtual ~AudioBuffer() override;

    float sample_rate() const;
    WebIDL::UnsignedLong length() const;
    double duration() const;
    WebIDL::UnsignedLong number_of_channels() const;
    WebIDL::ExceptionOr<GC::Ref<JS::Float32Array>> get_channel_data(JS::Realm&, WebIDL::UnsignedLong channel) const;
    WebIDL::ExceptionOr<void> copy_from_channel(JS::Realm&, GC::Root<JS::Float32Array> const&, WebIDL::UnsignedLong channel_number, WebIDL::UnsignedLong buffer_offset = 0) const;
    WebIDL::ExceptionOr<void> copy_to_channel(JS::Realm&, GC::Root<JS::Float32Array> const&, WebIDL::UnsignedLong channel_number, WebIDL::UnsignedLong buffer_offset = 0);

private:
    struct Channel {
        ByteBuffer data;
        mutable Vector<GC::Weak<JS::Float32Array>> views;
    };

    explicit AudioBuffer(Bindings::AudioBufferOptions const&);

    virtual void visit_edges(GC::Cell::Visitor&) override;
    virtual size_t external_memory_size() const override;

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-number-of-channels-slot
    // The number of audio channels for this AudioBuffer, which is an unsigned long.
    //
    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-internal-data-slot
    // A data block holding the audio sample data.
    Vector<Channel> m_channels; // [[internal data]] / [[number_of_channels]]

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-length-slot
    // The length of each channel of this AudioBuffer, which is an unsigned long.
    WebIDL::UnsignedLong m_length {}; // [[length]]

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-sample-rate-slot
    // The sample-rate, in Hz, of this AudioBuffer, a float.
    float m_sample_rate {}; // [[sample rate]]
};

}
