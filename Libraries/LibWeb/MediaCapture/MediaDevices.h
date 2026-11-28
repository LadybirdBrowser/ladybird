/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/Utf16FlyString.h>
#include <AK/Utf16String.h>
#include <AK/Vector.h>
#include <LibCamera/StreamRequest.h>
#include <LibGC/Forward.h>
#include <LibGC/Root.h>
#include <LibGC/Weak.h>
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

// https://w3c.github.io/mediacapture-main/#dfn-required-constraints
struct DeviceIdConstraint final {
    Vector<Utf16String> device_ids;
    // Only an exact constraint rules other devices out. Any other is a preference that falls back to them.
    bool is_required { false };
};

// What a getUserMedia() call wants of its "video": which camera, and what to ask it for.
struct VideoRequest final {
    Optional<DeviceIdConstraint> device_id_constraint;
    Camera::StreamRequest stream_request;
    // What the constraints require of the track's settings, which the camera is only asked to come close to.
    decltype(MediaTrackConstraints::width) width;
    decltype(MediaTrackConstraints::height) height;
    decltype(MediaTrackConstraints::frame_rate) frame_rate;
};

// https://w3c.github.io/mediacapture-main/#mediadevices
class MediaDevices final : public DOM::EventTarget {
    WEB_WRAPPABLE(MediaDevices, DOM::EventTarget);
    GC_DECLARE_ALLOCATOR(MediaDevices);

public:
    // Ends every live track whose device the document may no longer use: the microphone's, the camera's, or both.
    void end_tracks_without_permission();
    void track_was_cloned(Badge<MediaStreamTrack>, MediaStreamTrack& clone);
    void track_source_ended(Badge<MediaStreamTrack>, MediaStreamTrack&);
    HTML::Window& window() const { return m_window; }
    // getUserMedia() waits while the page lacks system focus, and the Window sees no event when that changes.
    void system_focus_changed();

    [[nodiscard]] static GC::Ref<MediaDevices> create(HTML::Window&);
    void stop_all_sources();

    MediaTrackSupportedConstraints get_supported_constraints();
    void enumerate_devices(GC::Ref<WebIDL::Promise>);
    void get_user_media(Optional<MediaStreamConstraints> const& constraints, GC::Ref<WebIDL::Promise>);

    void set_ondevicechange(WebIDL::CallbackType* event_handler);
    WebIDL::CallbackType* ondevicechange();

private:
    struct StoredDevice final {
        Utf16String dom_device_id;
        Utf16FlyString kind;
        Utf16String label;
        Utf16String group_id;
        bool is_default { false };
    };

    struct PendingGetUserMediaRequest final {
        GC::Ref<WebIDL::Promise> promise;
        Optional<DeviceIdConstraint> device_id_constraint;
        bool audio_requested { true };
        Optional<VideoRequest> video_request;

        void visit_edges(GC::Cell::Visitor&) const;
    };

    struct PendingEnumerateDevicesRequest final {
        GC::Ref<WebIDL::Promise> promise;

        void visit_edges(GC::Cell::Visitor&) const;
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
    bool has_live_device_of_kind(Utf16FlyString const& kind) const;
    void set_device_information_exposure(bool audio_requested, bool video_requested, bool value);
    void queue_enumerate_devices_task(GC::Ref<WebIDL::Promise>);
    void queue_get_user_media_task(GC::Ref<WebIDL::Promise>, Optional<DeviceIdConstraint>, Optional<VideoRequest>);
    void start_get_user_media(GC::Ref<WebIDL::Promise>, bool audio_requested, Optional<DeviceIdConstraint>, Optional<VideoRequest>);
    // Resolves the promise with the stream, once it has the track of a camera if one was asked for.
    void finish_get_user_media(GC::Ref<WebIDL::Promise>, GC::Ref<MediaStream>, Optional<VideoRequest>);
    void acquire_video_track(GC::Ref<WebIDL::Promise>, GC::Ref<MediaStream>, VideoRequest);
    // Rejects the promise and stops any tracks acquired before the failure.
    void fail_get_user_media(WebIDL::Promise&, MediaStream&, GC::Ref<WebIDL::DOMException>);
    void process_pending_enumerate_devices_requests();
    void process_pending_get_user_media_requests();
    GC::RootVector<GC::Ref<MediaDeviceInfo>> create_list_of_device_info_objects(Vector<StoredDevice> const& device_list);
    void run_device_change_notification_steps(Vector<StoredDevice> const& device_list);
    static Vector<StoredDevice> current_device_snapshot();
    void did_observe_audio_device_cache_update();
    void did_observe_camera_device_list_update();
    enum class DeviceListAge : u8 {
        Any,
        // One the CameraServer sent after it was asked now, which a page that was just granted the camera needs: only
        // then does its process hear what the cameras are.
        Fresh,
    };
    // Runs the callback once the process has a list of the cameras, or at once where no list ever comes.
    void run_with_camera_device_list(DeviceListAge, GC::Ref<GC::Function<void()>>);
    // Runs the callback once the process has a device list fetched from now on, or at once where no list ever comes.
    void run_with_fresh_audio_device_list(GC::Ref<GC::Function<void()>>);
    void end_tracks_of_removed_devices();
    // Whether the document holds the permission for the device the track is of.
    bool may_capture_for(MediaStreamTrack const&) const;
    void end_tracks(GC::RootVector<GC::Ref<MediaStreamTrack>>);
    virtual void visit_edges(Cell::Visitor&) override;

    GC::Ref<HTML::Window> m_window;

    // https://w3c.github.io/mediacapture-main/#mediadevices
    // [[devicesLiveMap]]
    HashMap<Utf16String, bool> m_devices_live_map;
    // [[devicesAccessibleMap]]
    HashMap<Utf16String, bool> m_devices_accessible_map;
    // [[storedDeviceList]]
    Vector<StoredDevice> m_stored_device_list;
    // [[canExposeCameraInfo]]
    bool m_can_expose_camera_info { false };
    bool camera_information_can_be_exposed() const;
    // [[canExposeMicrophoneInfo]]
    bool m_can_expose_microphone_info { false };
    // [[mediaStreamTrackSources]]
    // FIXME: Replace the tracks with concrete source objects when MediaCapture source modeling lands.
    Vector<GC::Weak<MediaStreamTrack>> m_media_stream_track_sources;
    Vector<PendingEnumerateDevicesRequest> m_pending_enumerate_devices_requests;
    Vector<PendingGetUserMediaRequest> m_pending_get_user_media_requests;
    Vector<GC::Ref<GC::Function<void()>>> m_awaiting_fresh_audio_device_list;
    Vector<GC::Ref<GC::Function<void()>>> m_awaiting_camera_device_list;
    GC::Ptr<DOM::IDLEventListener> m_pending_request_state_change_listener;
    Optional<u64> m_audio_device_cache_listener_id;
    Optional<u64> m_camera_device_list_listener_id;
};

}

GC_UNTRACED_CAPTURE(Web::MediaCapture::DeviceIdConstraint);
GC_UNTRACED_CAPTURE(Web::MediaCapture::VideoRequest);
