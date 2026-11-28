/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <CameraServer/CameraBackend.h>

namespace CameraServer {

Vector<Camera::DeviceInfo> with_preferred_default(Vector<Camera::DeviceInfo> devices, ByteString const& preferred_device_id)
{
    if (preferred_device_id.is_empty() || !any_of(devices, [&](auto const& device) { return device.dom_device_id == preferred_device_id; }))
        return devices;
    for (auto& device : devices)
        device.is_default = device.dom_device_id == preferred_device_id;
    return devices;
}

#if !defined(CAMERASERVER_PLATFORM_BACKEND)

namespace {

// FIXME: Implement camera backends for macOS (AVFoundation) and Windows (Media Foundation).
class NoCameras final : public CameraBackend {
public:
    virtual void watch_devices(Function<void(Vector<Camera::DeviceInfo>)> on_device_list) override
    {
        on_device_list({});
    }

    virtual ErrorOr<NonnullOwnPtr<CameraStream>> open(ByteString const&, Camera::StreamRequest, NonnullRefPtr<CameraStreamListener>) override
    {
        return Error::from_string_literal("Camera capture is not supported on this platform");
    }
};

}

NonnullOwnPtr<CameraBackend> create_platform_camera_backend()
{
    return make<NoCameras>();
}

#endif

}
