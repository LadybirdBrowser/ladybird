/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::WebAudio {

struct AudioBufferOptions {
    WebIDL::UnsignedLong number_of_channels { 1 };
    WebIDL::UnsignedLong length {};
    float sample_rate {};
};

// https://webaudio.github.io/web-audio-api/#AudioBuffer
class AudioBuffer final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(AudioBuffer, Bindings::PlatformObject);
    JS_DECLARE_ALLOCATOR(AudioBuffer);

public:
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBuffer>> create(JS::Realm&, WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);
    static WebIDL::ExceptionOr<JS::NonnullGCPtr<AudioBuffer>> construct_impl(JS::Realm&, AudioBufferOptions const&);

    virtual ~AudioBuffer() override;

    float sample_rate() const;
    WebIDL::UnsignedLong length() const;
    double duration() const;
    WebIDL::UnsignedLong number_of_channels() const;
    WebIDL::ExceptionOr<JS::NonnullGCPtr<JS::Float32Array>> get_channel_data(WebIDL::UnsignedLong channel) const;
    WebIDL::ExceptionOr<void> copy_from_channel(JS::Handle<WebIDL::BufferSource> const&, WebIDL::UnsignedLong channel_number, WebIDL::UnsignedLong buffer_offset = 0) const;
    WebIDL::ExceptionOr<void> copy_to_channel(JS::Handle<WebIDL::BufferSource> const&, WebIDL::UnsignedLong channel_number, WebIDL::UnsignedLong buffer_offset = 0);

private:
    explicit AudioBuffer(JS::Realm&, AudioBufferOptions const&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-number-of-channels-slot
    // The number of audio channels for this AudioBuffer, which is an unsigned long.
    //
    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-internal-data-slot
    // A data block holding the audio sample data.
    Vector<JS::NonnullGCPtr<JS::Float32Array>> m_channels; // [[internal data]] / [[number_of_channels]]

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-length-slot
    // The length of each channel of this AudioBuffer, which is an unsigned long.
    WebIDL::UnsignedLong m_length {}; // [[length]]

    // https://webaudio.github.io/web-audio-api/#dom-audiobuffer-sample-rate-slot
    // The sample-rate, in Hz, of this AudioBuffer, a float.
    float m_sample_rate {}; // [[sample rate]]
};

}
