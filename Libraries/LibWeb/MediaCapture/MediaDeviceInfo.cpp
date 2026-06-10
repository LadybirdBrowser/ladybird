/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>

namespace Web::MediaCapture {

GC_DEFINE_ALLOCATOR(MediaDeviceInfo);

// https://w3c.github.io/mediacapture-main/#device-info
GC::Ref<MediaDeviceInfo> MediaDeviceInfo::create(String device_id, MediaDeviceKind kind, String label, String group_id)
{
    auto device_info = GC::Heap::the().allocate<MediaDeviceInfo>(move(device_id), kind, move(label), move(group_id));

    // AD-HOC: device, mediaDevices, exposure checks handled by the caller.
    return device_info;
}

MediaDeviceInfo::MediaDeviceInfo(String device_id, MediaDeviceKind kind, String label, String group_id)
    : m_device_id(move(device_id))
    , m_kind(kind)
    , m_label(move(label))
    , m_group_id(move(group_id))
{
}

MediaDeviceInfo::~MediaDeviceInfo() = default;

JS::Object* MediaDeviceInfo::to_json(JS::Realm& realm) const
{
    auto& vm = realm.vm();
    auto result = JS::Object::create(realm, realm.intrinsics().object_prototype());

    MUST(result->create_data_property("deviceId"_utf16_fly_string, JS::PrimitiveString::create(vm, device_id())));
    MUST(result->create_data_property("kind"_utf16_fly_string, JS::PrimitiveString::create(vm, Bindings::idl_enum_to_string(kind()))));
    MUST(result->create_data_property("label"_utf16_fly_string, JS::PrimitiveString::create(vm, label())));
    MUST(result->create_data_property("groupId"_utf16_fly_string, JS::PrimitiveString::create(vm, group_id())));

    return result;
}

}
