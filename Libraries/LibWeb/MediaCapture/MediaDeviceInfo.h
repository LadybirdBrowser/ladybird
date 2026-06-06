/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Types.h>
#include <LibWeb/Bindings/MediaDeviceInfo.h>
#include <LibWeb/Bindings/Wrappable.h>

namespace Web::Bindings {

enum class MediaDeviceKind : u8;

}

namespace Web::MediaCapture {

// https://w3c.github.io/mediacapture-main/#device-info
class MediaDeviceInfo final : public Bindings::Wrappable {
    WEB_WRAPPABLE(MediaDeviceInfo, Bindings::Wrappable);
    GC_DECLARE_ALLOCATOR(MediaDeviceInfo);

public:
    [[nodiscard]] static GC::Ref<MediaDeviceInfo> create(String device_id, Bindings::MediaDeviceKind kind, String label, String group_id);
    virtual ~MediaDeviceInfo() override;

    String device_id() const { return m_device_id; }
    Bindings::MediaDeviceKind kind() const { return m_kind; }
    String label() const { return m_label; }
    String group_id() const { return m_group_id; }

private:
    MediaDeviceInfo(String device_id, Bindings::MediaDeviceKind kind, String label, String group_id);

    String m_device_id;
    Bindings::MediaDeviceKind m_kind;
    String m_label;
    String m_group_id;
};

}
