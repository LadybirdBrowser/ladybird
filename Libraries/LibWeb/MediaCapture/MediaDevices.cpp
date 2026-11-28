/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <LibAudio/AudioDevices.h>
#include <LibCamera/Devices.h>
#include <LibGC/Heap.h>
#include <LibGC/Root.h>
#include <LibGC/RootVector.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/VM.h>
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
#include <LibWeb/MediaCapture/CameraVideoSource.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/PermissionsAPI/Permissions.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

void MediaDevices::PendingEnumerateDevicesRequest::visit_edges(GC::Cell::Visitor& visitor) const
{
    visitor.visit(promise);
}

void MediaDevices::PendingGetUserMediaRequest::visit_edges(GC::Cell::Visitor& visitor) const
{
    visitor.visit(promise);
}

static Utf16FlyString const& AUDIO_INPUT_KIND = *new Utf16FlyString("audioinput"_utf16_fly_string);
static Utf16FlyString const& AUDIO_OUTPUT_KIND = *new Utf16FlyString("audiooutput"_utf16_fly_string);
static Utf16FlyString const& VIDEO_INPUT_KIND = *new Utf16FlyString("videoinput"_utf16_fly_string);

using ConstrainDOMString = Variant<Utf16String, Vector<Utf16String>, ConstrainDOMStringParameters>;

static Optional<DeviceIdConstraint> extract_device_id_constraint(Optional<ConstrainDOMString> const& device_id_value);
static Camera::StreamRequest stream_request_from(MediaTrackConstraints const&);
static Optional<StringView> unsatisfied_required_video_constraint(VideoRequest const&, u32 width, u32 height, double frame_rate);

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
    m_audio_device_cache_listener_id = Audio::AudioDevices::the().add_devices_changed_listener([this] {
        did_observe_audio_device_cache_update();
    });

    // NB: Listening asks for nothing: the CameraServer is first asked for its cameras when a page looks for one.
    m_camera_device_list_listener_id = Camera::Devices::the().add_devices_changed_listener([this] {
        did_observe_camera_device_list_update();
    });

    did_observe_audio_device_cache_update();
}

void MediaDevices::initialize_pending_request_state_change_listener()
{
    auto& realm = m_window->principal_realm();
    auto pending_request_state_change_callback_function = JS::NativeFunction::create(realm, GC::create_function(GC::Heap::the(), GC::captures(*this), [](auto& media_devices, JS::VM&) -> JS::ThrowCompletionOr<JS::Value> {
        media_devices.process_pending_enumerate_devices_requests();
        media_devices.process_pending_get_user_media_requests();
        return JS::js_undefined(); }), 0, Utf16FlyString {}, &realm);

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
        Audio::AudioDevices::the().remove_devices_changed_listener(m_audio_device_cache_listener_id.release_value());
    if (m_camera_device_list_listener_id.has_value())
        Camera::Devices::the().remove_devices_changed_listener(m_camera_device_list_listener_id.release_value());
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
    visitor.visit(m_awaiting_fresh_audio_device_list);
    visitor.visit(m_awaiting_camera_device_list);
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

// https://w3c.github.io/mediacapture-main/#dfn-has-system-focus
void MediaDevices::system_focus_changed()
{
    // 11.7 While hasSystemFocus is false, the User Agent MUST wait to proceed to the next step until a task queued to set
    //      hasSystemFocus to the result of the has system focus algorithm, would set hasSystemFocus to true.
    HTML::queue_global_task(HTML::Task::Source::UserInteraction, HTML::relevant_global_object(*m_window),
        GC::create_function(heap(), GC::captures(GC::Ref(*this)), [](auto& media_devices) {
            media_devices->process_pending_enumerate_devices_requests();
            media_devices->process_pending_get_user_media_requests();
        }));
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

    // AD-HOC: The cameras are the CameraServer's to list, and it is first asked when a page wants to know.
    Camera::Devices::the().ensure_watching();
    if (!Audio::AudioDevices::the().has_device_list() || !Camera::Devices::the().has_device_list()) {
        m_pending_enumerate_devices_requests.append({ promise });
        return;
    }

    m_stored_device_list = current_device_snapshot();

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
        else if (device.kind == "videoinput"sv)
            kind = MediaDeviceKind::Videoinput;
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
        if (device.kind == VIDEO_INPUT_KIND && !camera_information_can_be_exposed()) {
            device_id = {};
            label = {};
            group_id = {};
        }

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

static GC::Ref<WebIDL::DOMException> device_access_failure(Error const& error)
{
    // AD-HOC: The Browser refuses a capture only once the permission, or the system's consent for the browser, is gone,
    //         which is a failure of permission rather than of the device.
    if (error.is_errno() && error.code() == EPERM)
        return WebIDL::NotAllowedError::create("Permission denied"_utf16);
    if (error.is_errno() && error.code() == EIO)
        return WebIDL::NotReadableError::create("The audio input device could not be opened"_utf16);
    return WebIDL::AbortError::create("The audio input device could not be accessed"_utf16);
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-getusermedia
void MediaDevices::queue_get_user_media_task(GC::Ref<WebIDL::Promise> promise, Optional<DeviceIdConstraint> device_id_constraint, Optional<VideoRequest> video_request)
{
    GC::Ref<MediaDevices> media_devices = *this;

    // NB: These are the steps for "audio". Those for "video" follow them in acquire_video_track(), which adds its track
    //     to the stream made here.
    auto audio_input_devices = Audio::AudioDevices::the().input_devices();

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(GC::Root(promise), media_devices, move(device_id_constraint), move(audio_input_devices), move(video_request)), [](auto& promise, auto& media_devices, auto& device_id_constraint, auto& audio_input_devices, auto& video_request) {
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
        Vector<Audio::AudioDeviceInfo> final_set;

        // 11.3 For each media type kind in requestedMediaTypes, run the following steps.
        // 11.3.1 For each possible configuration of each possible source device of media of type kind, conceive a candidate as a placeholder for an eventual MediaStreamTrack holding a source device and configured with a settings dictionary comprised of its specific settings.
        //      Call this set of candidates the candidateSet.
        Vector<Audio::AudioDeviceInfo> candidate_set = audio_input_devices;

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
        // AD-HOC: Until the tab may capture, this process only learns that devices exist, not their ids, so a required
        //         device can only be looked for once the permission is granted.
        auto device_ids_are_known = any_of(candidate_set, [](auto const& device) { return !device.dom_device_id.is_empty(); });
        if (device_id_constraint.has_value() && device_id_constraint->is_required && device_ids_are_known) {
            candidate_set.remove_all_matching([&](auto const& device) {
                return !device_id_constraint->device_ids.contains_slow(Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()));
            });

            if (candidate_set.is_empty()) {
                // FIXME: 11.14.3 Reject p with a new OverconstrainedError created by calling OverconstrainedError(constraint, message).
                reject_not_found("Requested audio input device not found"_utf16);
                return;
            }
        }

        auto microphone_permission = Web::Permissions::PermissionName::Microphone;

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
        auto& settings_object = HTML::relevant_settings_object(*media_devices->m_window);
        PermissionsAPI::request_permission_with_user_prompt(microphone_permission, settings_object, GC::create_function(GC::Heap::the(), GC::captures(promise, media_devices, device_id_constraint, stream, video_request), [](auto& promise, auto& media_devices, auto& device_id_constraint, auto& stream, auto& video_request, PermissionsAPI::PermissionState microphone_permission_result) {
            auto& realm = WebIDL::promise_realm(*promise);
            HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

            // AD-HOC: The prompt may outlive the document, and capture must not start for one the user has left.
            if (!media_devices->m_window->associated_document().is_fully_active()) {
                WebIDL::reject_promise(*promise, WebIDL::InvalidStateError::create("Document is not fully active"_utf16));
                return;
            }

            // 11.5.2 If the result of the request is "denied", jump to the step labeled Permission Failure below.
            if (microphone_permission_result == PermissionsAPI::PermissionState::Denied) {
                // 11.15 Permission Failure: Reject p with a new DOMException object whose name attribute has the value "NotAllowedError".
                WebIDL::reject_promise(*promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                return;
            }

            // AD-HOC: The input devices' names and ids reach this process only once the Browser has told its AudioServer
            // of the grant, so finalSet is gathered again from a list fetched after it.
            media_devices->run_with_fresh_audio_device_list(GC::create_function(GC::Heap::the(), GC::captures(promise, media_devices, device_id_constraint, stream, video_request), [](auto& promise, auto& media_devices, auto& device_id_constraint, auto& stream, auto& video_request) {
                auto& realm = WebIDL::promise_realm(*promise);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                // AD-HOC: The user may have taken the permission away while the device list was fetched, and no track
                //         is created for a document that may no longer capture.
                auto& document = media_devices->m_window->associated_document();
                if (!document.is_fully_active() || PermissionsAPI::permission_state(Web::Permissions::PermissionName::Microphone, HTML::relevant_settings_object(*media_devices->m_window)) != PermissionsAPI::PermissionState::Granted) {
                    WebIDL::reject_promise(*promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                    return;
                }

                auto final_set = Audio::AudioDevices::the().input_devices();
                if (device_id_constraint.has_value()) {
                    auto matching_devices = final_set;
                    matching_devices.remove_all_matching([&](auto const& device) {
                        return !device_id_constraint->device_ids.contains_slow(Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()));
                    });
                    if (!matching_devices.is_empty()) {
                        final_set = move(matching_devices);
                    } else if (device_id_constraint->is_required) {
                        // FIXME: Reject with an OverconstrainedError for the deviceId constraint.
                        WebIDL::reject_promise(*promise, WebIDL::NotFoundError::create("Requested audio input device not found"_utf16));
                        return;
                    }
                }

                // 11.8 Set the device information exposure on mediaDevices with requestedMediaTypes and true.
                media_devices->set_device_information_exposure(true, false, true);

                // 11.9 For each media type kind in requestedMediaTypes, run the following sub steps.
                // 11.9.1 Let finalCandidate be the provided media, which MUST be precisely one candidate of type kind from finalSet.
                Optional<Audio::AudioDeviceInfo> final_candidate;
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

                // 11.9.2 The result of the request is "granted". If a hardware error such as an OS/program/webpage lock
                //        prevents access, remove the corresponding candidate from finalSet. If finalSet has no candidates
                //        of type kind, reject p with a new DOMException object whose name attribute has the value
                //        "NotReadableError" and abort these steps. Otherwise, restart these sub steps with the updated
                //        finalSet.
                //        If device access fails for any reason other than those listed above, remove the corresponding
                //        candidate from finalSet. If finalSet has no candidates of type kind, reject p with a new
                //        DOMException object whose name attribute has the value "AbortError" and abort these steps.
                //        Otherwise, restart these sub steps with the updated finalSet.
                // AD-HOC: The device is accessed through the capture stream of the track it feeds, so the track is created
                //         first, and the page sees it only once access succeeds.
                // FIXME: Restart these sub steps with the rest of finalSet when the device fails.
                Audio::AudioDeviceInfo const& granted_device = final_candidate.value();
                auto granted_device_id = Utf16String::from_utf8_with_replacement_character(granted_device.dom_device_id.view());
                GC::Ref<MediaStreamTrack> track = MediaStreamTrack::create(
                    MediaStreamTrackKind::Audio,
                    Utf16String::from_utf8_with_replacement_character(granted_device.label.view()));

                MediaTrackSettings settings;
                settings.device_id = granted_device_id;
                settings.sample_rate = granted_device.sample_rate_hz;
                settings.channel_count = granted_device.channel_count;
                track->set_settings(move(settings));
                track->set_media_devices({}, *media_devices);

                track->start_audio_capture(GC::create_function(GC::Heap::the(), GC::captures(promise, media_devices, stream, track, move(granted_device_id), video_request), [](auto& promise, auto& media_devices, auto& stream, auto& track, auto& granted_device_id, auto& video_request, ErrorOr<void> access) {
                    auto& realm = WebIDL::promise_realm(*promise);
                    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                    if (access.is_error()) {
                        WebIDL::reject_promise(*promise, device_access_failure(access.error()));
                        return;
                    }

                    // AD-HOC: The user may have left the document while the device opened, and capture does not go on
                    //         for one they have left.
                    if (!media_devices->m_window->associated_document().is_fully_active()) {
                        track->stop();
                        WebIDL::reject_promise(*promise, WebIDL::InvalidStateError::create("Document is not fully active"_utf16));
                        return;
                    }

                    // AD-HOC: The user may have taken the permission away while the device opened, which ended no track of
                    //         this request since none is tied to mediaDevices yet. Another page can keep the process
                    //         allowed to capture, so the device opening says nothing of this document.
                    if (PermissionsAPI::permission_state(Web::Permissions::PermissionName::Microphone, HTML::relevant_settings_object(*media_devices->m_window)) != PermissionsAPI::PermissionState::Granted) {
                        track->stop();
                        WebIDL::reject_promise(*promise, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                        return;
                    }

                    // 11.9.3 Let grantedDevice be finalCandidate's source device.
                    // 11.9.4 Using grantedDevice's deviceId, deviceId, set mediaDevices.[[devicesLiveMap]][deviceId] to true, if it isn't already true, and set mediaDevices.[[devicesAccessibleMap]][deviceId] to true, if it isn't already true.
                    media_devices->m_devices_live_map.set(granted_device_id, true);
                    media_devices->m_devices_accessible_map.set(granted_device_id, true);

                    // 11.9.5 Let track be the result of creating a MediaStreamTrack with grantedDevice and mediaDevices. The source of the MediaStreamTrack MUST NOT change.
                    // NB: The track was created to access the device.

                    // 11.9.6 Add track to stream's track set.
                    stream->add_track(track);

                    // FIXME: 11.10 Run the ApplyConstraints algorithm on all tracks in stream with the appropriate constraints.

                    // 11.11 For each track in stream, tie track source to MediaDevices with track.[[Source]] and mediaDevices.
                    media_devices->m_media_stream_track_sources.append(track);

                    // 11.12 Resolve p with stream and abort these steps.
                    // NB: Once the stream has its camera track, if one was asked for.
                    media_devices->finish_get_user_media(*promise, stream, video_request);
                }));
            }));
        }));
    }));
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-enumeratedevices
void MediaDevices::queue_enumerate_devices_task(GC::Ref<WebIDL::Promise> promise)
{
    GC::Ref<MediaDevices> media_devices = *this;

    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), GC::captures(GC::Root(promise), media_devices), [](auto& promise, auto& media_devices) {
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
    if (!Audio::AudioDevices::the().has_device_list())
        return;
    // A request that waits for the cameras has them asked for now, as one made while it could not proceed hasn't.
    // NB: A server that can't be reached is reported as no cameras at once, to the listener that brings us back here,
    //     so they are asked for only while there is no list, which that report is.
    if (!m_pending_enumerate_devices_requests.is_empty() && !Camera::Devices::the().has_device_list()) {
        Camera::Devices::the().ensure_watching();
        if (!Camera::Devices::the().has_device_list())
            return;
    }

    run_device_change_notification_steps(current_device_snapshot());

    GC::RootVector pending_requests { move(m_pending_enumerate_devices_requests) };
    for (auto& request : pending_requests)
        queue_enumerate_devices_task(request.promise);
}

void MediaDevices::process_pending_get_user_media_requests()
{
    if (!get_user_media_can_proceed())
        return;

    GC::RootVector<PendingGetUserMediaRequest> ready_requests;
    m_pending_get_user_media_requests.remove_all_matching([&](auto& request) {
        if (request.audio_requested && !Audio::AudioDevices::the().has_device_list())
            return false;
        ready_requests.append(move(request));
        return true;
    });

    GC::RootVector<GC::Ref<WebIDL::Promise>> promises;
    for (auto const& request : ready_requests)
        promises.append(request.promise);
    for (auto& request : ready_requests)
        start_get_user_media(request.promise, request.audio_requested, move(request.device_id_constraint), move(request.video_request));
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
        GC::create_function(GC::Heap::the(), GC::captures(GC::Ref(*this), GC::Ref(relevant_global_object)), [](auto& media_devices, auto& relevant_global_object) {
            media_devices->dispatch_event(DOM::Event::create(HTML::EventNames::devicechange,
                HighResolutionTime::current_high_resolution_time(*relevant_global_object)));
        }));
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-getusermedia
void MediaDevices::get_user_media(Optional<MediaStreamConstraints> const& constraints, GC::Ref<WebIDL::Promise> promise)
{
    bool audio_requested = false;
    bool video_requested = false;
    Optional<DeviceIdConstraint> device_id_constraint;
    Optional<VideoRequest> video_request;

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
        device_id_constraint = extract_device_id_constraint(audio_constraints.device_id);
    }

    auto const& video_value = constraints_value.video;
    if (video_value.has<bool>()) {
        video_requested = video_value.get<bool>();
        if (video_requested)
            video_request = VideoRequest {};
    } else {
        video_requested = true;
        auto const& video_constraints = video_value.get<MediaTrackConstraints>();
        video_request = VideoRequest {
            .device_id_constraint = extract_device_id_constraint(video_constraints.device_id),
            .stream_request = stream_request_from(video_constraints),
            .width = video_constraints.width,
            .height = video_constraints.height,
            .frame_rate = video_constraints.frame_rate,
        };
    }

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

    // 11. Run the following steps in parallel.
    // AD-HOC: Keep the request queued until isInView and hasSystemFocus allow the deferred task
    // to continue without spinning in the event loop.
    if (!get_user_media_can_proceed() || (audio_requested && !Audio::AudioDevices::the().has_device_list())) {
        m_pending_get_user_media_requests.append({
            .promise = promise,
            .device_id_constraint = move(device_id_constraint),
            .audio_requested = audio_requested,
            .video_request = move(video_request),
        });
        return;
    }

    start_get_user_media(promise, audio_requested, move(device_id_constraint), move(video_request));
}

void MediaDevices::start_get_user_media(GC::Ref<WebIDL::Promise> promise, bool audio_requested, Optional<DeviceIdConstraint> device_id_constraint, Optional<VideoRequest> video_request)
{
    if (audio_requested) {
        queue_get_user_media_task(promise, move(device_id_constraint), move(video_request));
        return;
    }

    // 11.4 Let stream be a new and empty MediaStream object.
    finish_get_user_media(promise, MediaStream::create(), move(video_request));
}

void MediaDevices::finish_get_user_media(GC::Ref<WebIDL::Promise> promise, GC::Ref<MediaStream> stream, Optional<VideoRequest> video_request)
{
    if (video_request.has_value()) {
        acquire_video_track(promise, stream, video_request.release_value());
        return;
    }

    // 11.12 Resolve p with stream and abort these steps.
    auto& realm = WebIDL::promise_realm(promise);
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
    resolve_media_stream_promise(promise, stream);
}

void MediaDevices::fail_get_user_media(WebIDL::Promise& promise, MediaStream& stream, GC::Ref<WebIDL::DOMException> exception)
{
    auto& realm = WebIDL::promise_realm(promise);
    HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

    // AD-HOC: The page never sees the stream, so nothing else would stop a microphone it already has a track of.
    for (auto const& track : stream.get_tracks())
        track->stop();
    WebIDL::reject_promise(promise, exception);
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-getusermedia
void MediaDevices::acquire_video_track(GC::Ref<WebIDL::Promise> promise, GC::Ref<MediaStream> stream, VideoRequest video_request)
{
    // NB: These are the steps of getUserMedia() for the media type "video", from where the request may proceed.

    // AD-HOC: The cameras are the CameraServer's to list, and it is first asked when a page wants one.
    Camera::Devices::the().ensure_watching();
    run_with_camera_device_list(DeviceListAge::Any, GC::create_function(heap(), GC::captures(GC::Ref(*this), promise, stream, move(video_request)), [](auto& media_devices, auto& promise, auto& stream, auto& video_request) {
        auto const& device_id_constraint = video_request.device_id_constraint;

        // 11.3.1 For each possible configuration of each possible source device of media of type kind, conceive a
        //        candidate as a placeholder for an eventual MediaStreamTrack holding a source device and configured
        //        with a settings dictionary comprised of its specific settings. Call this set of candidates the
        //        candidateSet.
        //        If candidateSet is the empty set, jump to the step labeled NotFound Failure below.
        auto candidate_set = Camera::Devices::the().devices();
        if (candidate_set.is_empty()) {
            // 11.13.2 Reject p with a new DOMException object whose name attribute has the value "NotFoundError".
            media_devices->fail_get_user_media(promise, stream, WebIDL::NotFoundError::create("No cameras available"_utf16));
            return;
        }

        // 11.3.5 Run the SelectSettings algorithm on each candidate in candidateSet with CS as the constraint set.
        // AD-HOC: Until the tab may capture, this process only learns that cameras exist, not their ids, so a required
        //         camera can only be looked for once the permission is granted.
        // FIXME: Fail with an OverconstrainedError where no camera can do what a required constraint asks.
        auto device_ids_are_known = any_of(candidate_set, [](auto const& device) { return !device.dom_device_id.is_empty(); });
        if (device_id_constraint.has_value() && device_id_constraint->is_required && device_ids_are_known) {
            candidate_set.remove_all_matching([&](auto const& device) {
                return !device_id_constraint->device_ids.contains_slow(Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()));
            });
            if (candidate_set.is_empty()) {
                media_devices->fail_get_user_media(promise, stream, WebIDL::NotFoundError::create("Requested camera not found"_utf16));
                return;
            }
        }

        // 11.3.6 Read the current permission state for all candidate devices in candidateSet that are not attached to a
        //        live MediaStreamTrack in the current Document. Remove from candidateSet any candidate whose device's
        //        permission state is "denied".
        auto camera_permission = Web::Permissions::PermissionName::Camera;
        auto& settings_object = HTML::relevant_settings_object(*media_devices->m_window);
        if (PermissionsAPI::permission_state(camera_permission, settings_object) == PermissionsAPI::PermissionState::Denied) {
            media_devices->fail_get_user_media(promise, stream, WebIDL::NotAllowedError::create("Permission denied"_utf16));
            return;
        }

        // 11.5.1 Request permission to use a PermissionDescriptor with its name member set to the permission name
        //        associated with kind.
        // AD-HOC: A page that names no camera leaves the choice of one to the user, who is offered it with the prompt; the
        //         camera chosen is the default one of the list fetched once the permission is granted.
        auto device_is_named = device_id_constraint.has_value() ? PermissionsAPI::DeviceIsNamed::Yes : PermissionsAPI::DeviceIsNamed::No;
        auto on_camera_permission_decided = GC::create_function(GC::Heap::the(), GC::captures(media_devices, promise, stream, video_request), [](auto& media_devices, auto& promise, auto& stream, auto& video_request, PermissionsAPI::PermissionState camera_permission_result) {
            // AD-HOC: The prompt may outlive the document, and capture must not start for one the user has left.
            if (!media_devices->m_window->associated_document().is_fully_active()) {
                media_devices->fail_get_user_media(promise, stream, WebIDL::InvalidStateError::create("Document is not fully active"_utf16));
                return;
            }

            // 11.5.2 If the result of the request is "denied", jump to the step labeled Permission Failure below.
            if (camera_permission_result == PermissionsAPI::PermissionState::Denied) {
                // 11.15 Permission Failure: Reject p with a new DOMException object whose name attribute has the value
                //       "NotAllowedError".
                media_devices->fail_get_user_media(promise, stream, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                return;
            }

            // AD-HOC: The cameras' names and ids reach this process only once the Browser has told its CameraServer of
            //         the grant, so finalSet is gathered from a list fetched after it.
            media_devices->run_with_camera_device_list(DeviceListAge::Fresh, GC::create_function(GC::Heap::the(), GC::captures(media_devices, promise, stream, video_request), [](auto& media_devices, auto& promise, auto& stream, auto& video_request) {
                // AD-HOC: The user may have taken the permission away while the list was fetched, and no track is
                //         created for a document that may no longer capture.
                auto& document = media_devices->m_window->associated_document();
                auto& settings_object = HTML::relevant_settings_object(*media_devices->m_window);
                if (!document.is_fully_active() || PermissionsAPI::permission_state(Web::Permissions::PermissionName::Camera, settings_object) != PermissionsAPI::PermissionState::Granted) {
                    media_devices->fail_get_user_media(promise, stream, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                    return;
                }

                // 11.3.8 Add all candidates from candidateSet to finalSet.
                auto final_set = Camera::Devices::the().devices();
                if (auto const& device_id_constraint = video_request.device_id_constraint; device_id_constraint.has_value()) {
                    auto matching_devices = final_set;
                    matching_devices.remove_all_matching([&](auto const& device) {
                        return !device_id_constraint->device_ids.contains_slow(Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()));
                    });
                    if (!matching_devices.is_empty()) {
                        final_set = move(matching_devices);
                    } else if (device_id_constraint->is_required) {
                        // FIXME: Reject with an OverconstrainedError for the deviceId constraint.
                        media_devices->fail_get_user_media(promise, stream, WebIDL::NotFoundError::create("Requested camera not found"_utf16));
                        return;
                    }
                }

                // 11.8 Set the device information exposure on mediaDevices with requestedMediaTypes and true.
                media_devices->set_device_information_exposure(false, true, true);

                // 11.9.1 Let finalCandidate be the provided media, which MUST be precisely one candidate of type kind
                //        from finalSet.
                Optional<Camera::DeviceInfo> final_candidate;
                for (auto const& device : final_set) {
                    if (!final_candidate.has_value() || device.is_default)
                        final_candidate = device;
                    if (device.is_default)
                        break;
                }
                // NB: A camera with no id is one this process may not open, as before the permission.
                if (!final_candidate.has_value() || final_candidate->dom_device_id.is_empty()) {
                    media_devices->fail_get_user_media(promise, stream, WebIDL::NotReadableError::create("No readable cameras available"_utf16));
                    return;
                }

                // 11.9.2 The result of the request is "granted". If a hardware error such as an OS/program/webpage lock
                //        prevents access, remove the corresponding candidate from finalSet. If finalSet has no
                //        candidates of type kind, reject p with a new DOMException object whose name attribute has the
                //        value "NotReadableError" and abort these steps. Otherwise, restart these sub steps with the
                //        updated finalSet.
                // AD-HOC: The camera is opened through the source of the track it feeds, so the source is created
                //         first, and the page sees a track of it only once access succeeds.
                // FIXME: Restart these sub steps with the rest of finalSet when the camera fails.
                auto source = CameraVideoSource::create();
                // NB: The callback takes the candidate with it, so the id is copied out first.
                auto device_id_to_open = final_candidate->dom_device_id;
                source->open(media_devices->m_window->page(), device_id_to_open, video_request.stream_request,
                    [media_devices = GC::Root(media_devices), promise = GC::Root(promise), stream = GC::Root(stream), source, device = final_candidate.release_value(), video_request](ErrorOr<void> access) {
                        if (access.is_error()) {
                            // AD-HOC: The Browser refuses a capture only once the permission is gone, which is a
                            //         failure of permission rather than of the camera.
                            if (access.error().is_errno() && access.error().code() == EPERM)
                                media_devices->fail_get_user_media(*promise, *stream, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                            else
                                media_devices->fail_get_user_media(*promise, *stream, WebIDL::NotReadableError::create("The camera could not be opened"_utf16));
                            return;
                        }

                        // AD-HOC: The user may have left the document while the camera opened, or taken the permission
                        //         away; the source closes the camera as it goes, having no track.
                        auto& settings_object = HTML::relevant_settings_object(*media_devices->m_window);
                        if (!media_devices->m_window->associated_document().is_fully_active()) {
                            media_devices->fail_get_user_media(*promise, *stream, WebIDL::InvalidStateError::create("Document is not fully active"_utf16));
                            return;
                        }
                        if (PermissionsAPI::permission_state(Web::Permissions::PermissionName::Camera, settings_object) != PermissionsAPI::PermissionState::Granted) {
                            media_devices->fail_get_user_media(*promise, *stream, WebIDL::NotAllowedError::create("Permission denied"_utf16));
                            return;
                        }

                        auto& realm = WebIDL::promise_realm(*promise);
                        HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };

                        // 11.9.3 Let grantedDevice be finalCandidate's source device.
                        // 11.9.4 Using grantedDevice's deviceId, deviceId, set mediaDevices.[[devicesLiveMap]][deviceId]
                        //        to true, if it isn't already true, and set
                        //        mediaDevices.[[devicesAccessibleMap]][deviceId] to true, if it isn't already true.
                        auto granted_device_id = Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view());
                        media_devices->m_devices_live_map.set(granted_device_id, true);
                        media_devices->m_devices_accessible_map.set(granted_device_id, true);

                        // 11.9.5 Let track be the result of creating a MediaStreamTrack with grantedDevice and
                        //        mediaDevices. The source of the MediaStreamTrack MUST NOT change.
                        auto track = MediaStreamTrack::create(MediaStreamTrackKind::Video, Utf16String::from_utf8_with_replacement_character(device.label.view()));
                        // NB: Use the CameraServer's output settings after cropping and scaling to the request.
                        MediaTrackSettings settings;
                        settings.device_id = granted_device_id;
                        settings.width = source->width();
                        settings.height = source->height();
                        settings.frame_rate = source->frame_rate();
                        if (source->height() != 0)
                            settings.aspect_ratio = static_cast<double>(source->width()) / source->height();
                        track->set_settings(move(settings));
                        track->set_media_devices({}, *media_devices);
                        track->set_video_picture_source(source);

                        // 11.9.6 Add track to stream's track set.
                        stream->add_track(track);

                        // NB: The camera gives the closest it does to what was asked for, which a required constraint
                        //     may still rule out.
                        if (auto constraint = unsatisfied_required_video_constraint(video_request, source->width(), source->height(), source->frame_rate()); constraint.has_value()) {
                            // FIXME: Reject with an OverconstrainedError object, whose constraint attribute names the constraint.
                            media_devices->fail_get_user_media(*promise, *stream, WebIDL::DOMException::create("OverconstrainedError"_utf16_fly_string, Utf16String::formatted("The camera can't satisfy the {} constraint", *constraint)));
                            return;
                        }

                        // FIXME: 11.10 Run the ApplyConstraints algorithm on all tracks in stream with the appropriate
                        //        constraints.

                        // 11.11 For each track in stream, tie track source to MediaDevices with track.[[Source]] and
                        //       mediaDevices.
                        media_devices->m_media_stream_track_sources.append(track);

                        // 11.12 Resolve p with stream and abort these steps.
                        resolve_media_stream_promise(*promise, *stream);
                    });
            }));
        });
        PermissionsAPI::request_permission_with_user_prompt(camera_permission, settings_object, on_camera_permission_decided, device_is_named);
    }));
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

Vector<MediaDevices::StoredDevice> MediaDevices::current_device_snapshot()
{
    Vector<StoredDevice> stored_devices;
    auto input_devices = Audio::AudioDevices::the().input_devices();
    auto output_devices = Audio::AudioDevices::the().output_devices();
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

    // NB: Empty until some page of the process has looked for a camera.
    for (auto const& device : Camera::Devices::the().devices()) {
        stored_devices.append(StoredDevice {
            .dom_device_id = Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()),
            .kind = VIDEO_INPUT_KIND,
            .label = Utf16String::from_utf8_with_replacement_character(device.label.view()),
            .group_id = Utf16String::from_utf8_with_replacement_character(device.group_id.view()),
            .is_default = device.is_default,
        });
    }

    return stored_devices;
}

void MediaDevices::did_observe_audio_device_cache_update()
{
    end_tracks_of_removed_devices();
    process_pending_enumerate_devices_requests();
    process_pending_get_user_media_requests();

    GC::RootVector awaiting { move(m_awaiting_fresh_audio_device_list) };
    for (auto const& callback : awaiting)
        callback->function()();
}

void MediaDevices::did_observe_camera_device_list_update()
{
    end_tracks_of_removed_devices();
    process_pending_enumerate_devices_requests();

    GC::RootVector awaiting { move(m_awaiting_camera_device_list) };
    for (auto const& callback : awaiting)
        callback->function()();
}

void MediaDevices::run_with_camera_device_list(DeviceListAge age, GC::Ref<GC::Function<void()>> callback)
{
    auto& devices = Camera::Devices::the();
    auto must_wait = age == DeviceListAge::Fresh ? devices.request_fresh_device_list() : !devices.has_device_list();
    if (!must_wait) {
        callback->function()();
        return;
    }
    m_awaiting_camera_device_list.append(callback);
}

void MediaDevices::run_with_fresh_audio_device_list(GC::Ref<GC::Function<void()>> callback)
{
    if (!Audio::AudioDevices::the().request_fresh_device_list()) {
        callback->function()();
        return;
    }
    m_awaiting_fresh_audio_device_list.append(callback);
}

// https://w3c.github.io/mediacapture-main/#track-ended
void MediaDevices::end_tracks_of_removed_devices()
{
    Vector<Utf16String> present_microphone_ids;
    for (auto const& device : Audio::AudioDevices::the().input_devices())
        present_microphone_ids.append(Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()));
    Vector<Utf16String> present_camera_ids;
    for (auto const& device : Camera::Devices::the().devices())
        present_camera_ids.append(Utf16String::from_utf8_with_replacement_character(device.dom_device_id.view()));

    GC::RootVector<GC::Ref<MediaStreamTrack>> tracks_to_end;
    m_media_stream_track_sources.remove_all_matching([&](GC::Weak<MediaStreamTrack> const& weak_track) {
        auto track = weak_track.ptr();
        if (!track || track->track_ready_state() == MediaStreamTrackState::Ended)
            return true;
        auto device_id = track->device_id();
        auto const& present_device_ids = track->is_video() ? present_camera_ids : present_microphone_ids;
        if (!device_id.has_value() || present_device_ids.contains_slow(device_id.value()))
            return false;
        tracks_to_end.append(*track);
        return true;
    });
    end_tracks(move(tracks_to_end));
}

// https://w3c.github.io/mediacapture-main/#dfn-device-permission-revocation-algorithm
void MediaDevices::end_tracks_without_permission()
{
    // NB: A track's source is the microphone or a camera, so every live track ends once the document may no longer use
    //     the one it has.
    GC::RootVector<GC::Ref<MediaStreamTrack>> tracks_to_end;
    m_media_stream_track_sources.remove_all_matching([&](GC::Weak<MediaStreamTrack> const& weak_track) {
        auto track = weak_track.ptr();
        if (!track || track->track_ready_state() == MediaStreamTrackState::Ended)
            return true;
        if (may_capture_for(*track))
            return false;
        tracks_to_end.append(*track);
        return true;
    });
    end_tracks(move(tracks_to_end));
}

bool MediaDevices::may_capture_for(MediaStreamTrack const& track) const
{
    auto& settings = HTML::relevant_settings_object(*m_window);
    auto name = track.is_video() ? Web::Permissions::PermissionName::Camera : Web::Permissions::PermissionName::Microphone;
    return PermissionsAPI::permission_state(name, settings) == PermissionsAPI::PermissionState::Granted;
}

// A clone of a device's track captures from the device too, so it ends with the permission like its original. A clone
// made once the permission is gone (from an ended listener of another track, e.g.) ends at once.
void MediaDevices::track_was_cloned(Badge<MediaStreamTrack>, MediaStreamTrack& clone)
{
    if (!may_capture_for(clone)) {
        GC::RootVector<GC::Ref<MediaStreamTrack>> tracks_to_end;
        tracks_to_end.append(clone);
        end_tracks(move(tracks_to_end));
        return;
    }
    m_media_stream_track_sources.append(clone);
}

void MediaDevices::track_source_ended(Badge<MediaStreamTrack>, MediaStreamTrack& track)
{
    GC::RootVector<GC::Ref<MediaStreamTrack>> tracks_to_end;
    tracks_to_end.append(track);
    end_tracks(move(tracks_to_end));
}

void MediaDevices::end_tracks(GC::RootVector<GC::Ref<MediaStreamTrack>> tracks_to_end)
{
    if (tracks_to_end.is_empty())
        return;

    // When a MediaStreamTrack track ends for any reason other than the stop() method being invoked, the User Agent
    // MUST queue a task that runs the following steps:
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(*m_window),
        GC::create_function(heap(), GC::captures(move(tracks_to_end), m_window), [](auto& tracks, auto& window) {
            HTML::TemporaryExecutionContext execution_context { window->principal_realm(), HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            // 1. If the track's [[ReadyState]] slot has the value "ended", abort these steps.
            // 2. Set track's [[ReadyState]] slot to "ended".
            // 3. Notify track's source that track is ended so that the source may be stopped, unless other MediaStreamTrack objects depend on it.
            // 4. Fire an event named ended at the object.
            for (auto const& track : tracks)
                track->end();
        }));
}

// https://w3c.github.io/mediacapture-main/#dfn-stop-all-sources
void MediaDevices::stop_all_sources()
{
    // 1. For each MediaStreamTrack object track whose relevant global object is globalObject, set track's
    //    [[ReadyState]] to "ended".
    // 2. If globalObject is a Window, then for each source in globalObject's associated
    //    MediaDevices.[[mediaStreamTrackSources]], stop source.
    // NB: The tracks stand in for their sources, and stopping a track also ends it.
    // FIXME: End this global object's tracks that no MediaDevices made too.
    for (auto const& weak_track : exchange(m_media_stream_track_sources, {})) {
        if (auto track = weak_track.ptr())
            track->stop();
    }
}

// https://w3c.github.io/mediacapture-main/#dfn-fitness-distance
// NB: What a constraint would most like, which is as far as this goes: the camera is asked for it and gives the closest
//     it does. A bare value is an ideal one, and a range without one is taken for its upper end, or else its lower.
template<typename T, typename Range>
static Optional<T> preferred_value(Optional<Variant<T, Range>> const& constraint)
{
    if (!constraint.has_value())
        return {};
    if (constraint->template has<T>())
        return constraint->template get<T>();
    auto const& range = constraint->template get<Range>();
    if (range.exact.has_value())
        return range.exact;
    if (range.ideal.has_value())
        return range.ideal;
    if (range.max.has_value())
        return range.max;
    return range.min;
}

// https://w3c.github.io/mediacapture-main/#dfn-fitness-distance
// If the constraint is required (constraintValue either contains one or more members named 'min', 'max', or 'exact',
// or is itself a bare value and bare values are to be treated as 'exact'), and the settings dictionary's value for the
// constraint does not satisfy the constraint, the fitness distance is positive infinity.
// NB: A bare value is an ideal one here.
template<typename T, typename Range>
static bool satisfies_required(Optional<Variant<T, Range>> const& constraint, double value)
{
    if (!constraint.has_value() || !constraint->template has<Range>())
        return true;
    auto const& range = constraint->template get<Range>();
    if (range.exact.has_value() && value != static_cast<double>(*range.exact))
        return false;
    if (range.min.has_value() && value < static_cast<double>(*range.min))
        return false;
    if (range.max.has_value() && value > static_cast<double>(*range.max))
        return false;
    return true;
}

static Optional<StringView> unsatisfied_required_video_constraint(VideoRequest const& request, u32 width, u32 height, double frame_rate)
{
    if (!satisfies_required(request.width, width))
        return "width"sv;
    if (!satisfies_required(request.height, height))
        return "height"sv;
    if (!satisfies_required(request.frame_rate, frame_rate))
        return "frameRate"sv;
    return {};
}

static Camera::StreamRequest stream_request_from(MediaTrackConstraints const& constraints)
{
    // FIXME: Take the advanced constraint sets, aspectRatio, facingMode and resizeMode into account.
    Camera::StreamRequest request;
    request.width = min(preferred_value(constraints.width).value_or(0), Camera::MAXIMUM_PICTURE_DIMENSION);
    request.height = min(preferred_value(constraints.height).value_or(0), Camera::MAXIMUM_PICTURE_DIMENSION);
    if (auto frame_rate = preferred_value(constraints.frame_rate); frame_rate.has_value() && *frame_rate >= 1)
        request.frame_rate = static_cast<u32>(min(*frame_rate, 240.0));
    return request;
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

static Optional<DeviceIdConstraint> extract_device_id_constraint(Optional<ConstrainDOMString> const& device_id_value)
{
    if (!device_id_value.has_value())
        return {};

    // https://w3c.github.io/mediacapture-main/#dfn-required-constraints
    // A bare value is an ideal one, so only exact makes the device a requirement.
    auto is_required = device_id_value->has<ConstrainDOMStringParameters>() && device_id_value->get<ConstrainDOMStringParameters>().exact.has_value();
    auto device_ids = extract_dom_string_constraint_values(*device_id_value).value_or({});

    // NB: Device ids are never empty, so an exact constraint without any other id is one that no device satisfies, such
    //     as the empty ids that enumerateDevices() exposes before the page may capture.
    if (device_ids.is_empty() && !is_required)
        return {};
    return DeviceIdConstraint { .device_ids = move(device_ids), .is_required = is_required };
}

}
