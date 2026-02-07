/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibWeb/WebAudio/Worklet/WorkletNodeDefinition.h>

namespace IPC {

template<>
inline ErrorOr<void> encode(Encoder& encoder, Web::WebAudio::Render::WorkletParameterDataEntry const& entry)
{
    TRY(encoder.encode(entry.name));
    TRY(encoder.encode(entry.value));
    return {};
}

template<>
inline ErrorOr<Web::WebAudio::Render::WorkletParameterDataEntry> decode(Decoder& decoder)
{
    Web::WebAudio::Render::WorkletParameterDataEntry entry;
    entry.name = TRY(decoder.decode<String>());
    entry.value = TRY(decoder.decode<double>());
    return entry;
}

}
