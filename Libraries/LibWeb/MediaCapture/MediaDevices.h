/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <LibGC/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/MediaCapture/MediaStreamConstraints.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

class MediaDeviceInfo;

// https://w3c.github.io/mediacapture-main/#mediadevices
class MediaDevices final : public DOM::EventTarget {
    WEB_PLATFORM_OBJECT(MediaDevices, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaDevices);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<MediaDevices> create(JS::Realm&);

    GC::Ref<WebIDL::Promise> enumerate_devices();
    MediaTrackSupportedConstraints get_supported_constraints();
    GC::Ref<WebIDL::Promise> get_user_media(Optional<MediaStreamConstraints> const& constraints = {});

    void set_ondevicechange(WebIDL::CallbackType* event_handler);
    WebIDL::CallbackType* ondevicechange();

private:
    struct StoredDevice final {
        String dom_device_id;
        String kind;
        String label;
        String group_id;
        bool is_default { false };
    };

    explicit MediaDevices(JS::Realm&);

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;

    bool microphone_information_can_be_exposed();
    bool can_use_microphone_feature() const;
    bool can_use_camera_feature() const;
    bool device_information_can_be_exposed();
    bool device_enumeration_can_proceed();
    void set_device_information_exposure(bool audio_requested, bool video_requested, bool value);
    Vector<GC::Ref<MediaDeviceInfo>> create_list_of_device_info_objects(Vector<StoredDevice> const& device_list);
    void run_device_change_notification_steps(Vector<StoredDevice> const& device_list);
    void did_observe_audio_device_cache_update();

    // https://w3c.github.io/mediacapture-main/#mediadevices
    // [[devicesLiveMap]]
    HashMap<String, bool> m_devices_live_map;
    // [[devicesAccessibleMap]]
    HashMap<String, bool> m_devices_accessible_map;
    // [[kindsAccessibleMap]]
    HashMap<String, bool> m_kinds_accessible_map;
    // [[storedDeviceList]]
    Vector<StoredDevice> m_stored_device_list;
    // [[canExposeCameraInfo]]
    bool can_expose_camera_info() const;
    // [[canExposeMicrophoneInfo]]
    bool can_expose_microphone_info() const;
    // [[mediaStreamTrackSources]]
    // FIXME: Replace provider IDs with concrete source objects when MediaCapture source modeling lands.
    HashTable<u64> m_media_stream_track_sources;
    Optional<u64> m_audio_device_cache_listener_id;
};

}
