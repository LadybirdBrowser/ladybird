/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AudioServer/AudioOutputDeviceInfo.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

template<>
inline ErrorOr<void> IPC::encode(Encoder& encoder, AudioServer::AudioOutputDeviceInfo const& info)
{
    TRY(encoder.encode(info.device_id));
    TRY(encoder.encode(info.label));
    TRY(encoder.encode(info.persistent_id));
    TRY(encoder.encode(info.sample_rate_hz));
    TRY(encoder.encode(info.channel_count));
    TRY(encoder.encode(info.is_default));
    return {};
}

template<>
inline ErrorOr<AudioServer::AudioOutputDeviceInfo> IPC::decode(Decoder& decoder)
{
    AudioServer::AudioOutputDeviceInfo info;
    info.device_id = TRY(decoder.decode<AudioServer::AudioOutputDeviceID>());
    info.label = TRY(decoder.decode<ByteString>());
    info.persistent_id = TRY(decoder.decode<ByteString>());
    info.sample_rate_hz = TRY(decoder.decode<u32>());
    info.channel_count = TRY(decoder.decode<u32>());
    info.is_default = TRY(decoder.decode<bool>());
    return info;
}
