/*
 * Copyright (c) 2025, Mehran Kamal <me@mehrankamal.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/MediaDeviceInfoPrototype.h>
#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Platform/MediaDevice.h>

namespace Web::MediaCapture {

class MediaDeviceInfo : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(MediaDeviceInfo, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(MediaDeviceInfo);

public:
    static GC::Ref<MediaDeviceInfo> create(JS::Realm&, Platform::MediaDevice const&, MediaDevices&);

    String const& device_id() const { return m_device_id; }
    Bindings::MediaDeviceKind const& kind() const { return m_kind; }
    String const& label() const { return m_label; }
    String const& group_id() const { return m_group_id; }

private:
    MediaDeviceInfo(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    String m_device_id {};
    Bindings::MediaDeviceKind m_kind {};
    String m_label;
    String m_group_id {};
};

}
