/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCamera/DeviceInfo.h>
#include <LibCamera/StreamRequest.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Camera::DeviceInfo const& device)
{
    TRY(encoder.encode(device.dom_device_id));
    TRY(encoder.encode(device.label));
    TRY(encoder.encode(device.group_id));
    TRY(encoder.encode(device.is_default));
    return {};
}

template<>
ErrorOr<Camera::DeviceInfo> decode(Decoder& decoder)
{
    Camera::DeviceInfo device;
    device.dom_device_id = TRY(decoder.decode<ByteString>());
    device.label = TRY(decoder.decode<ByteString>());
    device.group_id = TRY(decoder.decode<ByteString>());
    device.is_default = TRY(decoder.decode<bool>());
    return device;
}

template<>
ErrorOr<void> encode(Encoder& encoder, Camera::StreamRequest const& request)
{
    TRY(encoder.encode(request.width));
    TRY(encoder.encode(request.height));
    TRY(encoder.encode(request.frame_rate));
    return {};
}

template<>
ErrorOr<Camera::StreamRequest> decode(Decoder& decoder)
{
    Camera::StreamRequest request;
    request.width = TRY(decoder.decode<u32>());
    request.height = TRY(decoder.decode<u32>());
    request.frame_rate = TRY(decoder.decode<u32>());
    return request;
}

}
