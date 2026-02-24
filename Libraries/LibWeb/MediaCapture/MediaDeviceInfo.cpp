/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaDeviceInfoPrototype.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>

namespace Web::MediaCapture {

GC_DEFINE_ALLOCATOR(MediaDeviceInfo);

// https://w3c.github.io/mediacapture-main/#device-info
GC::Ref<MediaDeviceInfo> MediaDeviceInfo::create(JS::Realm& realm, String device_id, Bindings::MediaDeviceKind kind, String label, String group_id)
{
    auto device_info = realm.create<MediaDeviceInfo>(realm, move(device_id), kind, move(label), move(group_id));

    // AD-HOC: device, mediaDevices, exposure checks handled by the caller.
    return device_info;
}

MediaDeviceInfo::MediaDeviceInfo(JS::Realm& realm, String device_id, Bindings::MediaDeviceKind kind, String label, String group_id)
    : Bindings::PlatformObject(realm)
    , m_device_id(move(device_id))
    , m_kind(kind)
    , m_label(move(label))
    , m_group_id(move(group_id))
{
}

MediaDeviceInfo::~MediaDeviceInfo() = default;

void MediaDeviceInfo::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaDeviceInfo);
    Base::initialize(realm);
}

}
