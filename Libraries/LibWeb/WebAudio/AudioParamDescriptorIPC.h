/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/WebAudio/AudioParamDescriptor.h>

namespace IPC {

template<>
inline ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::AudioParamDescriptor const& descriptor)
{
    TRY(encoder.encode(descriptor.name.to_string()));
    TRY(encoder.encode(descriptor.default_value));
    TRY(encoder.encode(descriptor.min_value));
    TRY(encoder.encode(descriptor.max_value));
    TRY(encoder.encode(static_cast<u8>(descriptor.automation_rate)));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::AudioParamDescriptor> decode(Decoder& decoder)
{
    Web::WebAudio::AudioParamDescriptor descriptor;
    descriptor.name = TRY(decoder.decode<String>());
    descriptor.default_value = TRY(decoder.decode<float>());
    descriptor.min_value = TRY(decoder.decode<float>());
    descriptor.max_value = TRY(decoder.decode<float>());
    descriptor.automation_rate = static_cast<Web::Bindings::AutomationRate>(TRY(decoder.decode<u8>()));
    return descriptor;
}

}
