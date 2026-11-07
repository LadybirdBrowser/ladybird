/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Span.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/AudioBuffer.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/WebAudio/Rendering/AudioData.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::WebAudio {

class AudioBuffer final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(AudioBuffer, audio_buffer)

public:
    static ErrorOr<GC::Ref<AudioBuffer>> create(WebIDL::UnsignedLong number_of_channels, WebIDL::UnsignedLong length, float sample_rate);
    float sample_rate() const;
    WebIDL::UnsignedLong length() const;
    double duration() const;
    WebIDL::UnsignedLong number_of_channels() const;
    WebIDL::ExceptionOr<Bytes> channel_data(WebIDL::UnsignedLong channel);
    WebIDL::ExceptionOr<GC::Ref<JS::Float32Array>> get_channel_data(JS::Object& relevant_global_object, WebIDL::UnsignedLong channel);
    RefPtr<Rendering::AudioBufferContents> acquire_contents();
};

}
