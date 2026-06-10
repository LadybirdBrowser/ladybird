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
#include <LibGC/Root.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/MediaStreamConstraints.h>
#include <LibWeb/DOM/EventTarget.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

class MediaDeviceInfo;
class MediaStream;

using MediaStreamConstraints = Bindings::MediaStreamConstraints;
using MediaTrackSupportedConstraints = Bindings::MediaTrackSupportedConstraints;

// https://w3c.github.io/mediacapture-main/#mediadevices
class MediaDevices final : public DOM::EventTarget {
    WEB_WRAPPABLE(MediaDevices, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaDevices);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    [[nodiscard]] static GC::Ref<MediaDevices> create(HTML::Window&);

    MediaTrackSupportedConstraints get_supported_constraints();
    void enumerate_devices(GC::Ref<WebIDL::Promise>);
    void get_user_media(JS::Realm&, Optional<MediaStreamConstraints> const& constraints, GC::Ref<WebIDL::Promise>);

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

    struct PendingGetUserMediaRequest final {
        GC::Ref<WebIDL::Promise> promise;
        Optional<Vector<String>> requested_device_ids;
    };

    struct PendingEnumerateDevicesRequest final {
        GC::Ref<WebIDL::Promise> promise;
    };

    explicit MediaDevices(HTML::Window&);
    virtual void finalize() override;
    void initialize_pending_request_state_change_listener();

    bool microphone_information_can_be_exposed();
    bool can_use_microphone_feature() const;
    bool can_use_camera_feature() const;
    bool device_information_can_be_exposed();
    bool device_enumeration_can_proceed();
    bool get_user_media_can_proceed() const;
    bool is_in_view() const;
    bool has_live_device_of_kind(StringView kind) const;
    void set_device_information_exposure(bool audio_requested, bool video_requested, bool value);
    void queue_enumerate_devices_task(GC::Ref<WebIDL::Promise>);
    void queue_get_user_media_task(GC::Ref<WebIDL::Promise>, Optional<Vector<String>> requested_device_ids);
    void process_pending_enumerate_devices_requests();
    void process_pending_get_user_media_requests();
    GC::RootVector<GC::Ref<MediaDeviceInfo>> create_list_of_device_info_objects(Vector<StoredDevice> const& device_list);
    void run_device_change_notification_steps(Vector<StoredDevice> const& device_list);
    static Vector<StoredDevice> current_audio_device_snapshot();
    void did_observe_audio_device_cache_update();
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTML::Window> m_window;

    // https://w3c.github.io/mediacapture-main/#mediadevices
    // [[devicesLiveMap]]
    HashMap<String, bool> m_devices_live_map;
    // [[devicesAccessibleMap]]
    HashMap<String, bool> m_devices_accessible_map;
    // [[storedDeviceList]]
    Vector<StoredDevice> m_stored_device_list;
    // [[canExposeCameraInfo]]
    bool m_can_expose_camera_info { false };
    bool camera_information_can_be_exposed() const;
    // [[canExposeMicrophoneInfo]]
    bool m_can_expose_microphone_info { false };
    // [[mediaStreamTrackSources]]
    // FIXME: Replace provider IDs with concrete source objects when MediaCapture source modeling lands.
    HashTable<u64> m_media_stream_track_sources;
    Vector<PendingEnumerateDevicesRequest> m_pending_enumerate_devices_requests;
    Vector<PendingGetUserMediaRequest> m_pending_get_user_media_requests;
    GC::Ptr<DOM::IDLEventListener> m_pending_request_state_change_listener;
    Optional<u64> m_audio_device_cache_listener_id;
};

}
