/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <AK/Utf16String.h>
#include <LibWeb/Bindings/PlatformObject.h>

namespace Web::Bindings {

enum class MediaDeviceKind : u8;

}

namespace Web::MediaCapture {

// https://w3c.github.io/mediacapture-main/#device-info
class MediaDeviceInfo final : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaDeviceInfo, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MediaDeviceInfo);

public:
    [[nodiscard]] static GC::Ref<MediaDeviceInfo> create(JS::Realm&, Utf16String device_id, Bindings::MediaDeviceKind kind, Utf16String label, Utf16String group_id);
    virtual ~MediaDeviceInfo() override;

    Utf16String const& device_id() const { return m_device_id; }
    Bindings::MediaDeviceKind kind() const { return m_kind; }
    Utf16String const& label() const { return m_label; }
    Utf16String const& group_id() const { return m_group_id; }

private:
    MediaDeviceInfo(JS::Realm&, Utf16String device_id, Bindings::MediaDeviceKind kind, Utf16String label, Utf16String group_id);

    virtual void initialize(JS::Realm&) override;

    Utf16String m_device_id;
    Bindings::MediaDeviceKind m_kind;
    Utf16String m_label;
    Utf16String m_group_id;
};

}
