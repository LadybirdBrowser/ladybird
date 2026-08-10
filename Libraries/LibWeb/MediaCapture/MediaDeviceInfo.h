/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/MediaDeviceInfo.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::MediaCapture {

using MediaDeviceKind = Bindings::MediaDeviceKind;

// https://w3c.github.io/mediacapture-main/#device-info
class MediaDeviceInfo final : public Bindings::GCAllocatedWrappable {
    WEB_WRAPPABLE(MediaDeviceInfo, Bindings::GCAllocatedWrappable);
    GC_DECLARE_ALLOCATOR(MediaDeviceInfo);

public:
    [[nodiscard]] static GC::Ref<MediaDeviceInfo> create(Utf16String device_id, MediaDeviceKind kind, Utf16String label, Utf16String group_id);
    virtual ~MediaDeviceInfo() override;

    Utf16String const& device_id() const { return m_device_id; }
    MediaDeviceKind kind() const { return m_kind; }
    Utf16String const& label() const { return m_label; }
    Utf16String const& group_id() const { return m_group_id; }
    GC::Ref<JS::Object> to_json(JS::VM&) const;

private:
    MediaDeviceInfo(Utf16String device_id, MediaDeviceKind kind, Utf16String label, Utf16String group_id);

    Utf16String m_device_id;
    MediaDeviceKind m_kind;
    Utf16String m_label;
    Utf16String m_group_id;
};

}
