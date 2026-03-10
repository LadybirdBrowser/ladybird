/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <AudioServer/InputStream.h>
#include <AudioServer/OutputDriver.h>
#include <AudioServer/Server.h>

namespace AudioServer {

Vector<DeviceInfo> Server::enumerate_platform_devices()
{
    return Vector<DeviceInfo> {};
}

ErrorOr<NonnullOwnPtr<OutputDriver>> create_platform_output_driver(
    DeviceHandle,
    OutputState,
    u32,
    OutputDriver::SampleSpecificationCallback&&,
    OutputDriver::AudioDataRequestCallback&&)
{
    return Error::from_string_literal("Audio output is not available for this platform");
}

class StubInputStream final : public InputStream {
public:
    ~StubInputStream() override = default;
    explicit StubInputStream() = default;
};

ErrorOr<NonnullRefPtr<InputStream>> create_platform_input_stream(DeviceHandle, u32, u32, u64)
{
    return adopt_nonnull_ref_or_enomem(new (nothrow) StubInputStream());
}

}
