/*
 * Copyright (c) 2025, Mehran Kamal <me@mehrankamal.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>
#include <LibWeb/Platform/MediaDevice.h>

namespace Web::MediaCapture {

class MediaDevices : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(MediaDevices, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaDevices);

public:
    static GC::Ref<MediaDevices> create(JS::Realm&);
    GC::Ref<WebIDL::Promise> enumerate_devices();

private:
    MediaDevices(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    bool device_enumeration_can_proceed();
    bool device_information_can_be_exposed();
    bool camera_information_can_be_exposed();
    bool microphone_information_can_be_exposed();
    bool is_in_view();

    Vector<GC::Ref<MediaDeviceInfo>> create_list_of_device_info_objects();
    bool do_expose_non_camera_and_non_microphone_device(Platform::MediaDevice const&, Vector<GC::Ref<MediaDeviceInfo>> const&, Vector<GC::Ref<MediaDeviceInfo>> const&);

    HashMap<String, String> m_devices_live_map;
    HashMap<String, String> m_devices_accessible_map;
    HashMap<String, String> m_kinds_accessible;
    Vector<Platform::MediaDevice> m_stored_devices;
    bool m_can_expose_camera_info { false };
    bool m_can_expose_microphone_info { false };
    Vector<String> m_media_stream_track_sources;

    friend MediaDeviceInfo;
};

}
