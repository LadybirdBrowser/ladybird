/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>
#include <LibWeb/WebIDL/AbstractOperations.h>

namespace Web::MediaCapture {

GC_DEFINE_ALLOCATOR(MediaDeviceInfo);

// https://w3c.github.io/mediacapture-main/#device-info
GC::Ref<MediaDeviceInfo> MediaDeviceInfo::create(Utf16String device_id, MediaDeviceKind kind, Utf16String label, Utf16String group_id)
{
    auto device_info = GC::Heap::the().allocate<MediaDeviceInfo>(move(device_id), kind, move(label), move(group_id));

    // AD-HOC: device, mediaDevices, exposure checks handled by the caller.
    return device_info;
}

MediaDeviceInfo::MediaDeviceInfo(Utf16String device_id, MediaDeviceKind kind, Utf16String label, Utf16String group_id)
    : m_device_id(move(device_id))
    , m_kind(kind)
    , m_label(move(label))
    , m_group_id(move(group_id))
{
}

MediaDeviceInfo::~MediaDeviceInfo() = default;

JS::Object* MediaDeviceInfo::to_json(JS::VM& vm) const
{
    auto& realm = *vm.current_realm();
    auto result = JS::Object::create(realm, realm.intrinsics().object_prototype());

    MUST(result->create_data_property("deviceId"_utf16_fly_string, WebIDL::primitive_string_from_string(vm, device_id())));
    MUST(result->create_data_property("kind"_utf16_fly_string, WebIDL::primitive_string_from_string(vm, Bindings::idl_enum_to_string(kind()))));
    MUST(result->create_data_property("label"_utf16_fly_string, WebIDL::primitive_string_from_string(vm, label())));
    MUST(result->create_data_property("groupId"_utf16_fly_string, WebIDL::primitive_string_from_string(vm, group_id())));

    return result;
}

}
