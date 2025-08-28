/*
 * Copyright (c) 2025, Mehran Kamal <me@mehrankamal.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaDeviceInfoPrototype.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/Platform/MediaDevice.h>

namespace Web::MediaCapture {

static Bindings::MediaDeviceKind to_media_device_kind(Platform::MediaDeviceKind const& kind)
{
    switch (kind) {
    case Platform::MediaDeviceKind::AudioInput:
        return Bindings::MediaDeviceKind::Audioinput;
    case Platform::MediaDeviceKind::VideoInput:
        return Bindings::MediaDeviceKind::Videoinput;
    case Platform::MediaDeviceKind::AudioOutput:
        return Bindings::MediaDeviceKind::Audiooutput;
    default:
        VERIFY_NOT_REACHED();
    }
}

GC_DEFINE_ALLOCATOR(MediaDeviceInfo);

// https://www.w3.org/TR/mediacapture-streams/#creating-a-device-info-object
GC::Ref<MediaDeviceInfo> MediaDeviceInfo::create(JS::Realm& realm, Platform::MediaDevice const& device, MediaDevices& media_devices)
{
    // 1. Let deviceInfo be a new MediaDeviceInfo object to represent device.
    auto device_info = realm.create<MediaDeviceInfo>(realm);

    // 2. Initialize deviceInfo.kind for device.
    device_info->m_kind = to_media_device_kind(device.kind);

    // 3. If deviceInfo.kind is equal to "videoinput" and camera information can be exposed on mediaDevices is false, return deviceInfo.
    if (device_info->m_kind == Bindings::MediaDeviceKind::Videoinput && !media_devices.camera_information_can_be_exposed())
        return device_info;

    // 4. If deviceInfo.kind is equal to "audioinput" and micro phone information can be exposed on mediaDevices is false, return deviceInfo.
    if (device_info->m_kind == Bindings::MediaDeviceKind::Audioinput && !media_devices.microphone_information_can_be_exposed())
        return device_info;

    // 5. Initialize deviceInfo.label for device.
    device_info->m_label = device.label;

    // FIXME: 6. If a stored deviceId exists for device, initialize deviceInfo.deviceId to that value. Otherwise, let deviceInfo.deviceId be a newly generated unique identifier as described under deviceId.

    // FIXME: 7. If device belongs to the same physical device as a device already represented for document, initialize deviceInfo.groupId to the groupId value of the existing MediaDeviceInfo object. Otherwise, let deviceInfo.groupId be a newly generated unique identifier as described under groupId.

    // 8. Return deviceInfo
    return device_info;
}

MediaDeviceInfo::MediaDeviceInfo(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

void MediaDeviceInfo::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaDeviceInfo);
    Base::initialize(realm);
}

void MediaDeviceInfo::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
