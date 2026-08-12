/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/VM.h>
#include <LibMedia/Audio/AudioDevices.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/DOM/IDLEventListener.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

static Utf16FlyString const& AUDIO_INPUT_KIND = *new Utf16FlyString("audioinput"_utf16_fly_string);
static Utf16FlyString const& AUDIO_OUTPUT_KIND = *new Utf16FlyString("audiooutput"_utf16_fly_string);
static Utf16FlyString const& VIDEO_INPUT_KIND = *new Utf16FlyString("videoinput"_utf16_fly_string);

using ConstrainDOMString = Variant<Utf16String, Vector<Utf16String>, ConstrainDOMStringParameters>;

static Optional<Vector<Utf16String>> extract_device_id_constraint(Optional<ConstrainDOMString> const& device_id_value);

GC_DEFINE_ALLOCATOR(MediaDevices);

static void resolve_media_stream_promise(WebIDL::Promise& promise, GC::Ref<MediaStream> stream)
{
    auto& realm = HTML::relevant_realm(*promise.promise());
    WebIDL::resolve_promise(promise, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, stream));
}

static void resolve_media_device_info_list_promise(WebIDL::Promise& promise, GC::RootVector<GC::Ref<MediaDeviceInfo>> const& result_list)
{
    auto& realm = HTML::relevant_realm(*promise.promise());
    GC::Ref<JS::Array> array = MUST(JS::Array::create(realm, result_list.size()));
    for (size_t index = 0; index < result_list.size(); ++index) {
        JS::PropertyKey property_index { index };
        if (array->create_data_property(property_index, Bindings::wrap(Bindings::host_defined_wrapper_world(realm), realm, result_list[index])).is_error()) {
            WebIDL::reject_promise(promise, WebIDL::OperationError::create(realm, "Failed to build enumerateDevices result"_utf16));
            return;
        }
    }
    WebIDL::resolve_promise(promise, array);
}

GC::Ref<MediaDevices> MediaDevices::create(HTML::Window& window)
{
    auto media_devices = GC::Heap::the().allocate<MediaDevices>(window);
    media_devices->initialize_pending_request_state_change_listener();
    return media_devices;
}

MediaDevices::MediaDevices(HTML::Window& window)
    : DOM::EventTarget()
    , m_window(window)
{
    m_audio_device_cache_listener_id = Media::AudioDevices::the().add_devices_changed_listener([this] {
        did_observe_audio_device_cache_update();
    });

    did_observe_audio_device_cache_update();
}

void MediaDevices::initialize_pending_request_state_change_listener()
{
    auto& realm = m_window->principal_realm();
    auto pending_request_state_change_callback_function = JS::NativeFunction::create(realm, [media_devices = GC::Ref(*this)](JS::VM&) {
        media_devices->process_pending_enumerate_devices_requests();
        media_devices->process_pending_get_user_media_requests();
        return JS::js_undefined(); }, 0, Utf16FlyString {}, &realm);

    auto pending_request_state_change_callback = GC::Heap::the().allocate<WebIDL::CallbackType>(*pending_request_state_change_callback_function, realm);
    m_pending_request_state_change_listener = DOM::IDLEventListener::create(pending_request_state_change_callback);

    m_window->associated_document().add_event_listener_without_options(HTML::EventNames::visibilitychange, *m_pending_request_state_change_listener);
    m_window->add_event_listener_without_options(HTML::EventNames::focus, *m_pending_request_state_change_listener);
    m_window->add_event_listener_without_options(HTML::EventNames::blur, *m_pending_request_state_change_listener);
}

void MediaDevices::finalize()
{
    if (m_pending_request_state_change_listener) {
        auto& window = *m_window;
        window.associated_document().remove_event_listener_without_options(HTML::EventNames::visibilitychange, *m_pending_request_state_change_listener);
        window.remove_event_listener_without_options(HTML::EventNames::focus, *m_pending_request_state_change_listener);
        window.remove_event_listener_without_options(HTML::EventNames::blur, *m_pending_request_state_change_listener);
    }

    Base::finalize();
    if (m_audio_device_cache_listener_id.has_value())
        Media::AudioDevices::the().remove_devices_changed_listener(m_audio_device_cache_listener_id.release_value());
}

void MediaDevices::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_window);
    for (auto const& request : m_pending_enumerate_devices_requests)
        visitor.visit(request.promise);
    for (auto const& request : m_pending_get_user_media_requests)
        visitor.visit(request.promise);
    visitor.visit(m_pending_request_state_change_listener);
}

MediaTrackSupportedConstraints MediaDevices::get_supported_constraints()
{
    // Returns a dictionary whose members are the constrainable properties known to the User Agent.
    return {};
}

// https://w3c.github.io/mediacapture-main/#device-information-exposure
bool MediaDevices::device_information_can_be_exposed()
{
    // 1. If camera information can be exposed on mediaDevices, return true.
    if (camera_information_can_be_exposed())
        return true;
    // 2. If microphone information can be exposed on mediaDevices, return true.
    if (microphone_information_can_be_exposed())
        return true;
    // 3. Return false.
    return false;
}

bool MediaDevices::microphone_information_can_be_exposed()
{
    // 1. If any of the local devices of kind "audioinput" are attached to a live MediaStreamTrack in
    // mediaDevices's relevant global object's associated Document, return true.
    if (has_live_device_of_kind(AUDIO_INPUT_KIND))
        return true;
    // 2. Return mediaDevices.[[canExposeMicrophoneInfo]].
    return m_can_expose_microphone_info;
}

bool MediaDevices::camera_information_can_be_exposed() const
{
    return has_live_device_of_kind(VIDEO_INPUT_KIND) || m_can_expose_camera_info;
}

bool MediaDevices::can_use_microphone_feature() const
{
    auto const& document = m_window->associated_document();
    return document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Microphone);
}

bool MediaDevices::can_use_camera_feature() const
{
    auto const& document = m_window->associated_document();
    return document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Camera);
}

bool MediaDevices::device_enumeration_can_proceed()
{
    // 1. The User Agent MAY return true if device information can be exposed on mediaDevices.
    if (device_information_can_be_exposed())
        return true;

    // 2. Return the result of is in view with mediaDevices.
    return is_in_view();
}

bool MediaDevices::get_user_media_can_proceed() const
{
    return is_in_view() && m_window->associated_document().has_focus();
}

bool MediaDevices::is_in_view() const
{
    auto const& document = m_window->associated_document();
    return document.is_fully_active() && document.visibility_state() == HTML::VisibilityState::Visible;
}

bool MediaDevices::has_live_device_of_kind(Utf16FlyString const& kind) const
{
    for (auto const& device : m_stored_device_list) {
        if (device.kind != kind)
            continue;

        auto live = m_devices_live_map.get(device.dom_device_id);
        if (live.value_or(false))
            return true;
    }

    return false;
}

// https://w3c.github.io/mediacapture-main/#set-device-information-exposure-0
void MediaDevices::set_device_information_exposure(bool audio_requested, bool video_requested, bool value)
{
    // 1. If "video" is in requestedTypes, run the following sub-steps.
    if (video_requested) {
        // 1.1 Set mediaDevices.[[canExposeCameraInfo]] to value.
        m_can_expose_camera_info = value;

        // FIXME: 1.2 If value is true and if device exposure can be extended with "microphone", set mediaDevices.[[canExposeMicrophoneInfo]] to true.
    }

    // 2. If "audio" is in requestedTypes, run the following sub-steps.
    if (audio_requested) {
        // 2.1 Set mediaDevices.[[canExposeMicrophoneInfo]] to value.
        m_can_expose_microphone_info = value;

        // FIXME: 2.2 If value is true and if device exposure can be extended with "camera", set mediaDevices.[[canExposeCameraInfo]] to true.
    }
    process_pending_enumerate_devices_requests();
    process_pending_get_user_media_requests();
}

void MediaDevices::enumerate_devices(GC::Ref<WebIDL::Promise> promise)
{
    // 2. Let proceed be the result of device enumeration can proceed with this.
    if (!device_enumeration_can_proceed()) {
        m_pending_enumerate_devices_requests.append({ promise });
        return;
    }

    if (!HTML::Window::in_test_mode() && !Media::AudioDevices::the().has_completed_refresh()) {
        m_pending_enumerate_devices_requests.append({ promise });
        Media::AudioDevices::the().refresh();
        return;
    }

    m_stored_device_list = current_audio_device_snapshot();

    // 3. Let mediaDevices be this.
    // 4. Run the following steps in parallel.
    queue_enumerate_devices_task(promise);
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-enumeratedevices
GC::RootVector<GC::Ref<MediaDeviceInfo>> MediaDevices::create_list_of_device_info_objects(Vector<StoredDevice> const& device_list)
{
    // To perform creating a list of device info objects, given mediaDevices and deviceList, run the following steps:

    // 1. Let resultList be an empty list.
    GC::RootVector<GC::Ref<MediaDeviceInfo>> result_list;

    // 2. Let microphoneList, cameraList and otherDeviceList be empty lists.
    GC::RootVector<GC::Ref<MediaDeviceInfo>> microphone_list;
    GC::RootVector<GC::Ref<MediaDeviceInfo>> camera_list;
    GC::RootVector<GC::Ref<MediaDeviceInfo>> other_device_list;

    // 3. Let document be mediaDevices's relevant global object's associated Document.
    auto const& document = m_window->associated_document();
    (void)document;

    auto create_device_info_object = [&](StoredDevice const& device) -> Optional<GC::Ref<MediaDeviceInfo>> {
        Optional<MediaDeviceKind> kind;
        if (device.kind == "audioinput"sv)
            kind = MediaDeviceKind::Audioinput;
        else if (device.kind == "audiooutput"sv)
            kind = MediaDeviceKind::Audiooutput;
        else
            return {};

        auto device_id = device.dom_device_id;
        auto label = device.label;
        auto group_id = device.group_id;

        if (device.kind == AUDIO_INPUT_KIND && !microphone_information_can_be_exposed()) {
            device_id = {};
            label = {};
            group_id = {};
        }
        // FIXME: Implement videoinput creation when camera enumeration is available.

        return MediaDeviceInfo::create(move(device_id), *kind, move(label), move(group_id));
    };

    // 4. Run the following sub steps for each discovered device in deviceList, device.
    for (auto const& device : device_list) {
        // 4.1 If device is not a microphone, or document is not allowed to use the feature identified by microphone, abort these sub steps and continue with the next device.
        if (device.kind != AUDIO_INPUT_KIND)
            continue;
        if (!can_use_microphone_feature())
            continue;

        // 4.2 Let deviceInfo be the result of creating a device info object to represent device, with mediaDevices.
        Optional<GC::Ref<MediaDeviceInfo>> device_info = create_device_info_object(device);
        if (!device_info.has_value())
            continue;

        // 4.3 If device is the system default microphone, prepend deviceInfo to microphoneList. Otherwise, append deviceInfo to microphoneList.
        if (device.is_default)
            microphone_list.insert(0, *device_info);
        else
            microphone_list.append(*device_info);
    }

    // 5. Run the following sub steps for each discovered device in deviceList, device.
    for (auto const& device : device_list) {
        // 5.1 If device is not a camera, or document is not allowed to use the feature identified by camera, abort these sub steps and continue with the next device.
        if (device.kind != VIDEO_INPUT_KIND)
            continue;
        if (!can_use_camera_feature())
            continue;

        // 5.2 Let deviceInfo be the result of creating a device info object to represent device, with mediaDevices.
        Optional<GC::Ref<MediaDeviceInfo>> device_info = create_device_info_object(device);
        if (!device_info.has_value())
            continue;

        // 5.3 If device is the system default camera, prepend deviceInfo to cameraList. Otherwise, append deviceInfo to cameraList.
        if (device.is_default)
            camera_list.insert(0, *device_info);
        else
            camera_list.append(*device_info);
    }

    // 6. If microphone information can be exposed on mediaDevices is false, truncate microphoneList to its first item.
    if (!microphone_information_can_be_exposed()) {
        while (microphone_list.size() > 1)
            microphone_list.take_last();
    }

    // 7. If camera information can be exposed on mediaDevices is false, truncate cameraList to its first item.
    if (!camera_information_can_be_exposed()) {
        while (camera_list.size() > 1)
            camera_list.take_last();
    }

    // 8. Run the following sub steps for each discovered device in deviceList, device.
    for (auto const& device : device_list) {
        // 8.1 If device is a microphone or device is a camera, abort these sub steps and continue with the next device.
        if (device.kind == AUDIO_INPUT_KIND || device.kind == VIDEO_INPUT_KIND)
            continue;

        // 8.2 Run the exposure decision algorithm for devices other than camera and microphone.
        bool expose_other_device = device_information_can_be_exposed();
        // FIXME: Implement the exact exposure decision algorithm from the relevant output-device spec.
        if (!expose_other_device)
            continue;

        // 8.3 Let deviceInfo be the result of creating a device info object to represent device, with mediaDevices.
        Optional<GC::Ref<MediaDeviceInfo>> device_info = create_device_info_object(device);
        if (!device_info.has_value())
            continue;

        // 8.4 Append deviceInfo to otherDeviceList.
        // 8.5 If device is the system default audio output, prepend it instead.
        if (device.kind == AUDIO_OUTPUT_KIND && device.is_default) {
            // AD-HOC: 8.5.x Label & id in the LibMedia audio device cache are only mutated in response
            // to OS notifications. The device manager updates the DOM id & label in the event of changes,
            // and these will be broadcast to the LibMedia cache. We don't set that state here.
            other_device_list.insert(0, *device_info);
        } else {
            other_device_list.append(*device_info);
        }
    }

    // 9. Append to resultList all devices of microphoneList in order.
    result_list.extend(microphone_list);

    // 10. Append to resultList all devices of cameraList in order.
    result_list.extend(camera_list);

    // 11. Append to resultList all devices of otherDeviceList in order.
    result_list.extend(other_device_list);

    // 12. Return resultList.
    return result_list;
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-getusermedia
void MediaDevices::queue_get_user_media_task(GC::Ref<WebIDL::Promise> promise, Optional<Vector<Utf16String>> requested_device_ids)
{
    GC::Ref<MediaDevices> media_devices = *this;

    // FIXME: Add camera/video
    Vector<Media::AudioDeviceInfo> audio_input_devices = Media::AudioDevices::the().input_devices();

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [promise = GC::Root(promise), media_devices, requested_device_ids = move(requested_device_ids), audio_input_devices = move(audio_input_devices)] mutable {
        auto& realm = WebIDL::promise_realm(*promise);
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        auto reject_not_found = [&](Utf16String const& message) {
            WebIDL::reject_promise(*promise, WebIDL::NotFoundError::create(message));
        };

        auto reject_permission_failure = [&] {
            WebIDL::reject_promise(*promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
        };

        // 11. Run the following steps in parallel.
        // 11.1 While isInView is false, the User Agent MUST wait to proceed to the next step until a task queued to set isInView to the result of the is in view algorithm, would set isInView to true.
        // 11.7 While hasSystemFocus is false, the User Agent MUST wait to proceed to the next step until a task queued to set hasSystemFocus to the result of the has system focus algorithm, would set hasSystemFocus to true.
        // AD-HOC: Requests stay pending until both conditions are true, then continue here.

        // 11.2 Let finalSet be an (initially) empty set.
        Vector<Media::AudioDeviceInfo> final_set;

        // 11.3 For each media type kind in requestedMediaTypes, run the following steps.
        // 11.3.1 For each possible configuration of each possible source device of media of type kind, conceive a candidate as a placeholder for an eventual MediaStreamTrack holding a source device and configured with a settings dictionary comprised of its specific settings.
        //      Call this set of candidates the candidateSet.
        Vector<Media::AudioDeviceInfo> candidate_set = audio_input_devices;

        // 11.3.1 If candidateSet is the empty set, jump to the step labeled NotFound Failure below.
        if (candidate_set.is_empty()) {
            // 11.13.2 Reject p with a new DOMException object whose name attribute has the value "NotFoundError".
            reject_not_found("No audio input devices available"_utf16);
            return;
        }

        // 11.3.2 If the value of the kind entry of constraints is true, set CS to the empty constraint set (no constraint). Otherwise, continue with CS set to the value of the kind entry of constraints.
        // 11.3.3 Remove any constrainable property inside of CS that are not defined for MediaStreamTrack objects of type kind.
        // 11.3.4 If CS contains a member that is a required constraint and whose name is not in the list of allowed required constraints for device selection, then reject p with a TypeError, and abort these steps.
        // 11.3.5 Run the SelectSettings algorithm on each candidate in candidateSet with CS as the constraint set.
        if (requested_device_ids.has_value() && !requested_device_ids->is_empty()) {
            Vector<Media::AudioDeviceInfo> filtered_candidates;
            for (auto const& device : candidate_set) {
                auto device_id = Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view());
                for (auto const& requested_id : *requested_device_ids) {
                    if (device_id == requested_id) {
                        filtered_candidates.append(device);
                        break;
                    }
                }
            }
            candidate_set = move(filtered_candidates);

            if (candidate_set.is_empty()) {
                // FIXME: 11.14.3 Reject p with a new OverconstrainedError created by calling OverconstrainedError(constraint, message).
                reject_not_found("Requested audio input device not found"_utf16);
                return;
            }
        }

        PermissionsAPI::PermissionDescriptor microphone_permission { "microphone"_utf16 };

        // 11.3.6 Read the current permission state for all candidate devices in candidateSet that are not attached to a live MediaStreamTrack in the current Document. Remove from candidateSet any candidate whose device's permission state is "denied".
        if (PermissionsAPI::permission_state(microphone_permission) == PermissionsAPI::PermissionState::Denied) {
            reject_permission_failure();
            return;
        }
        // FIXME: 11.3.7 Optionally, e.g., based on a previously-established user preference, for security reasons, or due to platform limitations, jump to the step labeled Permission Failure below.

        // 11.3.8 Add all candidates from candidateSet to finalSet.
        final_set = move(candidate_set);

        // 11.4 Let stream be a new and empty MediaStream object.
        GC::Ref<MediaStream> stream = MediaStream::create();

        // 11.5 For each media type kind in requestedMediaTypes, run the following sub steps, preferably at the same time.

        // 11.5.1 Request permission to use a PermissionDescriptor with its name member set to the permission name associated with kind.
        auto microphone_permission_result = PermissionsAPI::permission_state(microphone_permission);
        // FIXME: Wire this up to a real microphone permission prompt. Until then, do not turn
        //        a prompt state into a persisted denied state without a user decision.
        if (HTML::Window::in_test_mode() && microphone_permission_result == PermissionsAPI::PermissionState::Prompt)
            microphone_permission_result = PermissionsAPI::request_permission(microphone_permission);
        // 11.5.2 If the result of the request is "denied", jump to the step labeled Permission Failure below.
        if (microphone_permission_result == PermissionsAPI::PermissionState::Denied) {
            reject_permission_failure();
            return;
        }

        // 11.8 Set the device information exposure on mediaDevices with requestedMediaTypes and true.
        media_devices->set_device_information_exposure(true, false, true);

        // 11.9 For each media type kind in requestedMediaTypes, run the following sub steps.
        // 11.9.1 Let finalCandidate be the provided media, which MUST be precisely one candidate of type kind from finalSet.
        Optional<Media::AudioDeviceInfo> final_candidate;
        for (auto const& device : final_set) {
            if (!final_candidate.has_value())
                final_candidate = device;
            if (!device.is_default)
                continue;
            final_candidate = device;
            break;
        }
        if (!final_candidate.has_value()) {
            WebIDL::reject_promise(*promise, WebIDL::NotReadableError::create("No readable audio input devices available"_utf16));
            return;
        }

        // 11.9.2 The result of the request is "granted".
        // 11.9.3 Let grantedDevice be finalCandidate's source device.
        Media::AudioDeviceInfo const& granted_device = final_candidate.value();

        // 11.9.4 Using grantedDevice's deviceId, deviceId, set mediaDevices.[[devicesLiveMap]][deviceId] to true, if it isn't already true, and set mediaDevices.[[devicesAccessibleMap]][deviceId] to true, if it isn't already true.
        auto granted_device_id = Utf16String::from_utf8_with_replacement_character(granted_device.dom_device_id.view());
        media_devices->m_devices_live_map.set(granted_device_id, true);
        media_devices->m_devices_accessible_map.set(granted_device_id, true);

        // 11.9.5 Let track be the result of creating a MediaStreamTrack with grantedDevice and mediaDevices. The source of the MediaStreamTrack MUST NOT change.
        GC::Ref<MediaStreamTrack> track = MediaStreamTrack::create(
            MediaStreamTrackKind::Audio,
            Utf16String::from_utf8_with_replacement_character(granted_device.label.view()));

        MediaTrackSettings settings;
        settings.device_id = granted_device_id;
        settings.sample_rate = granted_device.sample_rate_hz;
        settings.channel_count = granted_device.channel_count;
        track->set_settings(move(settings));

        // 11.9.6 Add track to stream's track set.
        stream->append_track(track);

        // FIXME: 11.10 Run the ApplyConstraints algorithm on all tracks in stream with the appropriate constraints.

        // 11.11 For each track in stream, tie track source to MediaDevices with track.[[Source]] and mediaDevices.
        media_devices->m_media_stream_track_sources.set(track->provider_id());

        // 11.12 Resolve p with stream and abort these steps.
        resolve_media_stream_promise(*promise, stream);

        // 11.15 Permission Failure: Reject p with a new DOMException object whose name attribute has the value "NotAllowedError".
    }));
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-enumeratedevices
void MediaDevices::queue_enumerate_devices_task(GC::Ref<WebIDL::Promise> promise)
{
    GC::Ref<MediaDevices> media_devices = *this;

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [promise = GC::Root(promise), media_devices] {
        auto& realm = WebIDL::promise_realm(*promise);
        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

        auto result_list = media_devices->create_list_of_device_info_objects(media_devices->m_stored_device_list);
        resolve_media_device_info_list_promise(*promise, result_list);
    }));
}

void MediaDevices::process_pending_enumerate_devices_requests()
{
    if (!device_enumeration_can_proceed())
        return;
    if (!HTML::Window::in_test_mode() && !Media::AudioDevices::the().has_completed_refresh()) {
        Media::AudioDevices::the().refresh();
        return;
    }

    run_device_change_notification_steps(current_audio_device_snapshot());

    auto pending_requests = move(m_pending_enumerate_devices_requests);
    for (auto& request : pending_requests)
        queue_enumerate_devices_task(request.promise);
}

void MediaDevices::process_pending_get_user_media_requests()
{
    if (!get_user_media_can_proceed())
        return;
    if (!HTML::Window::in_test_mode() && !Media::AudioDevices::the().has_completed_refresh()) {
        Media::AudioDevices::the().refresh();
        return;
    }

    auto pending_requests = move(m_pending_get_user_media_requests);
    for (auto& request : pending_requests)
        queue_get_user_media_task(request.promise, move(request.requested_device_ids));
}

// https://w3c.github.io/mediacapture-main/#dfn-device-change-notification-steps
void MediaDevices::run_device_change_notification_steps(Vector<StoredDevice> const& device_list)
{
    // When new media input and/or output devices are made available to the User Agent, or any available
    // input and/or output device becomes unavailable, or the system default for input and/or output
    // devices of a MediaDeviceKind changed, the User Agent MUST run the following device change
    // notification steps for each MediaDevices object, mediaDevices, for which device enumeration can
    // proceed is true, but for no other MediaDevices object:

    if (!device_enumeration_can_proceed())
        return;

    // 1. Let lastExposedDevices be the result of creating a list of device info objects with mediaDevices and mediaDevices.[[storedDeviceList]].
    GC::RootVector<GC::Ref<MediaDeviceInfo>> last_exposed_devices = create_list_of_device_info_objects(m_stored_device_list);

    // 2. Let deviceList be the list of all media input and/or output devices available to the User Agent.
    // device_list is the caller-provided snapshot.

    // 3. Let newExposedDevices be the result of creating a list of device info objects with mediaDevices and deviceList.
    GC::RootVector<GC::Ref<MediaDeviceInfo>> new_exposed_devices = create_list_of_device_info_objects(device_list);

    // 4. If the MediaDeviceInfo objects in newExposedDevices match those in lastExposedDevices and have
    //    the same order, then abort these steps.
    if (new_exposed_devices.size() == last_exposed_devices.size()) {
        bool exposed_devices_match = true;
        for (size_t i = 0; i < new_exposed_devices.size(); ++i) {
            if (new_exposed_devices[i]->kind() != last_exposed_devices[i]->kind()
                || new_exposed_devices[i]->device_id() != last_exposed_devices[i]->device_id()
                || new_exposed_devices[i]->label() != last_exposed_devices[i]->label()
                || new_exposed_devices[i]->group_id() != last_exposed_devices[i]->group_id()) {
                exposed_devices_match = false;
                break;
            }
        }
        if (exposed_devices_match)
            return;
    }

    // 5. Set mediaDevices.[[storedDeviceList]] to deviceList.
    m_stored_device_list = device_list;

    // 6. Queue a task that fires an event named devicechange, using the DeviceChangeEvent constructor
    //    with devices initialized to newExposedDevices, at mediaDevices.
    // FIXME: Dispatch DeviceChangeEvent with devices once DeviceChangeEvent is implemented in LibWeb.
    auto& relevant_global_object = HTML::relevant_global_object(*m_window);
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation,
        relevant_global_object,
        GC::create_function(GC::Heap::the(),
            [media_devices = GC::Ref(*this), relevant_global_object = GC::Ref(relevant_global_object)] {
                media_devices->dispatch_event(DOM::Event::create(HTML::EventNames::devicechange,
                    HighResolutionTime::current_high_resolution_time(*relevant_global_object)));
            }));
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-getusermedia
void MediaDevices::get_user_media(Optional<MediaStreamConstraints> const& constraints, GC::Ref<WebIDL::Promise> promise)
{
    bool audio_requested = false;
    bool video_requested = false;
    Optional<Vector<Utf16String>> requested_device_ids;

    // 1. Let constraints be the method's first argument.
    if (!constraints.has_value()) {
        auto& realm = WebIDL::promise_realm(promise);
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "getUserMedia requires constraints"sv));
        return;
    }

    // 2. Let requestedMediaTypes be the set of media types in constraints with either a dictionary value or a value of true.
    auto const& constraints_value = constraints.value();
    auto const& audio_value = constraints_value.audio;
    if (audio_value.has<bool>()) {
        audio_requested = audio_value.get<bool>();
    } else {
        audio_requested = true;
        auto const& audio_constraints = audio_value.get<MediaTrackConstraints>();
        requested_device_ids = extract_device_id_constraint(audio_constraints.device_id);
    }

    auto const& video_value = constraints_value.video;
    if (video_value.has<bool>())
        video_requested = video_value.get<bool>();
    else
        video_requested = true;

    // 3. If requestedMediaTypes is the empty set, return a promise rejected with a TypeError.
    if (!audio_requested && !video_requested) {
        auto& realm = WebIDL::promise_realm(promise);
        WebIDL::reject_promise(promise, JS::TypeError::create(realm, "No media types requested"sv));
        return;
    }

    // 4. Let document be the relevant global object's associated Document.
    auto const& document = m_window->associated_document();

    // 5. If document is NOT fully active, return a promise rejected with an InvalidStateError.
    if (!document.is_fully_active()) {
        WebIDL::reject_promise(promise, WebIDL::InvalidStateError::create("Document is not fully active"_utf16));
        return;
    }

    // 6. If requestedMediaTypes contains "audio" and document is not allowed to use the feature identified by the "microphone" permission name, jump to Permission Failure.
    if (audio_requested && !can_use_microphone_feature()) {
        WebIDL::reject_promise(promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
        return;
    }

    // 7. If requestedMediaTypes contains "video" and document is not allowed to use the feature identified by the "camera" permission name, jump to Permission Failure.
    if (video_requested && !can_use_camera_feature()) {
        WebIDL::reject_promise(promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
        return;
    }

    // 8. Let mediaDevices be this.
    // 9. Let isInView be the result of the is in view algorithm.

    // 10. Let p be a new promise.
    if (video_requested) {
        WebIDL::reject_promise(promise, WebIDL::NotSupportedError::create("Video capture is not supported"_utf16));
        return;
    }

    // 11. Run the following steps in parallel.
    // AD-HOC: Keep the request queued until isInView and hasSystemFocus allow the deferred task
    // to continue without spinning in the event loop.
    if (!get_user_media_can_proceed()) {
        m_pending_get_user_media_requests.append({ .promise = promise, .requested_device_ids = move(requested_device_ids) });
        return;
    }

    if (!HTML::Window::in_test_mode() && !Media::AudioDevices::the().has_completed_refresh()) {
        Media::AudioDevices::the().refresh();
        m_pending_get_user_media_requests.append({ .promise = promise, .requested_device_ids = move(requested_device_ids) });
        return;
    }

    queue_get_user_media_task(promise, move(requested_device_ids));
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-ondevicechange
WebIDL::CallbackType* MediaDevices::ondevicechange()
{
    return event_handler_attribute(HTML::EventNames::devicechange);
}

void MediaDevices::set_ondevicechange(WebIDL::CallbackType* event_handler)
{
    set_event_handler_attribute(HTML::EventNames::devicechange, event_handler);
}

Vector<MediaDevices::StoredDevice> MediaDevices::current_audio_device_snapshot()
{
    Vector<StoredDevice> stored_devices;
    auto input_devices = Media::AudioDevices::the().input_devices();
    auto output_devices = Media::AudioDevices::the().output_devices();
    stored_devices.ensure_capacity(input_devices.size() + output_devices.size());

    for (auto const& device : input_devices) {
        stored_devices.append(StoredDevice {
            .dom_device_id = Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()),
            .kind = AUDIO_INPUT_KIND,
            .label = Utf16String::from_utf8_with_replacement_character(device.label.view()),
            .group_id = Utf16String::from_utf8_with_replacement_character(device.group_id.view()),
            .is_default = device.is_default,
        });
    }

    for (auto const& device : output_devices) {
        stored_devices.append(StoredDevice {
            .dom_device_id = Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()),
            .kind = AUDIO_OUTPUT_KIND,
            .label = Utf16String::from_utf8_with_replacement_character(device.label.view()),
            .group_id = Utf16String::from_utf8_with_replacement_character(device.group_id.view()),
            .is_default = device.is_default,
        });
    }

    return stored_devices;
}

void MediaDevices::did_observe_audio_device_cache_update()
{
    process_pending_enumerate_devices_requests();
    process_pending_get_user_media_requests();
}

static Optional<Vector<Utf16String>> dom_string_values_from_variant(Variant<Utf16String, Vector<Utf16String>> const& value)
{
    Vector<Utf16String> values;
    if (value.has<Utf16String>()) {
        auto const& string_value = value.get<Utf16String>();
        if (!string_value.is_empty())
            values.append(string_value);
    } else {
        for (auto const& entry : value.get<Vector<Utf16String>>()) {
            if (!entry.is_empty())
                values.append(entry);
        }
    }
    return values.is_empty() ? Optional<Vector<Utf16String>> {} : Optional<Vector<Utf16String>> { move(values) };
}

static Optional<Vector<Utf16String>> extract_dom_string_constraint_values(ConstrainDOMString const& constraint)
{
    // https://w3c.github.io/mediacapture-main/#dom-constraindomstring
    // ConstrainDOMString is (DOMString or sequence<DOMString> or ConstrainDOMStringParameters).
    // https://w3c.github.io/mediacapture-main/#dom-constraindomstringparameters
    // ConstrainDOMStringParameters members:
    // - exact: (DOMString or sequence<DOMString>) The exact required value for this property.
    // - ideal: (DOMString or sequence<DOMString>) The ideal (target) value for this property.
    // https://w3c.github.io/mediacapture-main/#constraint-types
    // List values MUST be interpreted as disjunctions.
    if (constraint.has<ConstrainDOMStringParameters>()) {
        auto const& parameters = constraint.get<ConstrainDOMStringParameters>();
        if (parameters.exact.has_value())
            return dom_string_values_from_variant(*parameters.exact);
        if (parameters.ideal.has_value())
            return dom_string_values_from_variant(*parameters.ideal);
        return {};
    }

    if (constraint.has<Utf16String>())
        return dom_string_values_from_variant(Variant<Utf16String, Vector<Utf16String>> { constraint.get<Utf16String>() });

    return dom_string_values_from_variant(Variant<Utf16String, Vector<Utf16String>> { constraint.get<Vector<Utf16String>>() });
}

static Optional<Vector<Utf16String>> extract_device_id_constraint(Optional<ConstrainDOMString> const& device_id_value)
{
    if (!device_id_value.has_value())
        return {};

    return extract_dom_string_constraint_values(*device_id_value);
}

}
