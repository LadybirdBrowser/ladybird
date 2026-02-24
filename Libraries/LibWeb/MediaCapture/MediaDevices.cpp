/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/RefCounted.h>
#include <LibAudioServer/SessionClientOfAudioServer.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibMedia/Audio/AudioDevices.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaDeviceInfoPrototype.h>
#include <LibWeb/Bindings/MediaDevicesPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamConstraints.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

static Optional<Vector<String>> extract_device_id_constraint(Optional<ConstrainDOMString> const& device_id_value);
static String const AUDIO_INPUT_KIND = "audioinput"_string;
static String const AUDIO_OUTPUT_KIND = "audiooutput"_string;
static String const VIDEO_INPUT_KIND = "videoinput"_string;

GC_DEFINE_ALLOCATOR(MediaDevices);

GC::Ref<MediaDevices> MediaDevices::create(JS::Realm& realm)
{
    return realm.create<MediaDevices>(realm);
}

MediaDevices::MediaDevices(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
    m_audio_device_cache_listener_id = Media::AudioDevices::the().add_devices_changed_listener([this] {
        did_observe_audio_device_cache_update();
    });

    did_observe_audio_device_cache_update();
}

MediaDevices::~MediaDevices()
{
    if (m_audio_device_cache_listener_id.has_value())
        Media::AudioDevices::the().remove_devices_changed_listener(m_audio_device_cache_listener_id.release_value());
}

void MediaDevices::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaDevices);
    Base::initialize(realm);
}

// https://w3c.github.io/mediacapture-main/#device-information-exposure
bool MediaDevices::microphone_information_can_be_exposed()
{
    // 1. If any of the local devices of kind "audioinput" are attached to a live MediaStreamTrack in
    // mediaDevices's relevant global object's associated Document, return true.
    for (auto const& entry : m_devices_live_map) {
        if (entry.value)
            return true;
    }

    // 2. Return mediaDevices.[[canExposeMicrophoneInfo]].
    return can_expose_microphone_info();
}

bool MediaDevices::can_expose_camera_info() const
{
    auto camera_accessible = m_kinds_accessible_map.get(VIDEO_INPUT_KIND);
    // AD-HOC: Default to permissive exposure until camera permission plumbing lands.
    return can_use_camera_feature() && camera_accessible.value_or(true);
}

bool MediaDevices::can_expose_microphone_info() const
{
    auto microphone_accessible = m_kinds_accessible_map.get(AUDIO_INPUT_KIND);
    // AD-HOC: Default to permissive exposure until microphone permission plumbing lands.
    return can_use_microphone_feature() && microphone_accessible.value_or(true);
}

bool MediaDevices::can_use_microphone_feature() const
{
    auto const& document = as<HTML::Window>(realm().global_object()).associated_document();
    return document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Microphone);
}

bool MediaDevices::can_use_camera_feature() const
{
    auto const& document = as<HTML::Window>(realm().global_object()).associated_document();
    return document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Camera);
}

// https://w3c.github.io/mediacapture-main/#device-information-exposure
bool MediaDevices::device_information_can_be_exposed()
{
    // 1. If camera information can be exposed on mediaDevices, return true.
    if (can_expose_camera_info())
        return true;

    // 2. If microphone information can be exposed on mediaDevices, return true.
    if (microphone_information_can_be_exposed())
        return true;

    // 3. Return false.
    return false;
}

// https://w3c.github.io/mediacapture-main/#device-information-exposure
bool MediaDevices::device_enumeration_can_proceed()
{
    // 1. The User Agent MAY return true if device information can be exposed on mediaDevices.
    if (device_information_can_be_exposed())
        return true;

    // 2. Return the result of is in view with mediaDevices.
    auto const& document = as<HTML::Window>(realm().global_object()).associated_document();
    return document.is_fully_active() && document.visibility_state() == HTML::VisibilityState::Visible;
}

// https://w3c.github.io/mediacapture-main/#set-device-information-exposure-0
void MediaDevices::set_device_information_exposure(bool audio_requested, bool video_requested, bool value)
{
    // 1. If "video" is in requestedTypes, run the following sub-steps.
    if (video_requested) {
        // 1.1 Set mediaDevices.[[canExposeCameraInfo]] to value.
        m_kinds_accessible_map.set(VIDEO_INPUT_KIND, value);

        // 1.2 If value is true and if device exposure can be extended with "microphone", set mediaDevices.[[canExposeMicrophoneInfo]] to true.
        // FIXME: Implement device exposure can be extended checks.
    }

    // 2. If "audio" is in requestedTypes, run the following sub-steps.
    if (audio_requested) {
        // 2.1 Set mediaDevices.[[canExposeMicrophoneInfo]] to value.
        m_kinds_accessible_map.set(AUDIO_INPUT_KIND, value);

        // 2.2 If value is true and if device exposure can be extended with "camera", set mediaDevices.[[canExposeCameraInfo]] to true.
        // FIXME: Implement device exposure can be extended checks.
    }
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-enumeratedevices
Vector<GC::Ref<MediaDeviceInfo>> MediaDevices::create_list_of_device_info_objects(Vector<StoredDevice> const& device_list)
{
    JS::Realm& realm = this->realm();

    // To perform creating a list of device info objects, given mediaDevices and deviceList, run the following steps:

    // 1. Let resultList be an empty list.
    Vector<GC::Ref<MediaDeviceInfo>> result_list;

    // 2. Let microphoneList, cameraList and otherDeviceList be empty lists.
    Vector<GC::Ref<MediaDeviceInfo>> microphone_list;
    Vector<GC::Ref<MediaDeviceInfo>> camera_list;
    Vector<GC::Ref<MediaDeviceInfo>> other_device_list;

    // 3. Let document be mediaDevices's relevant global object's associated Document.
    auto const& document = as<HTML::Window>(realm.global_object()).associated_document();
    (void)document;

    auto create_device_info_object = [&](StoredDevice const& device) -> Optional<GC::Ref<MediaDeviceInfo>> {
        Optional<Bindings::MediaDeviceKind> kind;
        if (device.kind == "audioinput"sv)
            kind = Bindings::MediaDeviceKind::Audioinput;
        else if (device.kind == "audiooutput"sv)
            kind = Bindings::MediaDeviceKind::Audiooutput;
        else
            return {};

        String device_id = device.dom_device_id;
        String label = device.label;
        String group_id = device.group_id;

        if (device.kind == "audioinput"sv && !microphone_information_can_be_exposed()) {
            device_id = String {};
            label = String {};
            group_id = String {};
        }
        // FIXME: Implement videoinput creation when camera enumeration is available.

        return MediaDeviceInfo::create(realm, move(device_id), *kind, move(label), move(group_id));
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
    if (!can_expose_camera_info()) {
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
        if (device.kind == "audiooutput"sv && device.is_default) {
            // 8.5.2 Set defaultAudioOutputInfo's deviceId to default.
            // FIXME: MediaDeviceInfo has no mutable deviceId; synthesize this object once mutable construction is supported.

            // 8.5.3 The user agent SHOULD update defaultAudioOutputInfo's label to make it explicit that this is the system default audio output.
            // FIXME: Add default-audio-output label decoration once output-device UX is implemented.
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

static bool media_device_info_lists_match(Vector<GC::Ref<MediaDeviceInfo>> const& a, Vector<GC::Ref<MediaDeviceInfo>> const& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i]->kind() != b[i]->kind())
            return false;
        if (a[i]->device_id() != b[i]->device_id())
            return false;
        if (a[i]->label() != b[i]->label())
            return false;
        if (a[i]->group_id() != b[i]->group_id())
            return false;
    }

    return true;
}

// https://w3c.github.io/mediacapture-main/#mediadevices - When new media input and/or output devices...
void MediaDevices::run_device_change_notification_steps(Vector<StoredDevice> const& device_list)
{
    // FIXME: AudioServer must detect and notify us when an audio device changes, so wire it into this.
    // No clue about cameras yet.

    // When new media input and/or output devices are made available to the User Agent, or any available
    // input and/or output device becomes unavailable, or the system default for input and/or output
    // devices of a MediaDeviceKind changed, the User Agent MUST run the following device change
    // notification steps for each MediaDevices object, mediaDevices, for which device enumeration can
    // proceed is true, but for no other MediaDevices object:

    // 1. Let lastExposedDevices be the result of creating a list of device info objects with mediaDevices and mediaDevices.[[storedDeviceList]].
    Vector<GC::Ref<MediaDeviceInfo>> last_exposed_devices = create_list_of_device_info_objects(m_stored_device_list);

    // 2. Let deviceList be the list of all media input and/or output devices available to the User Agent.
    // device_list is the caller-provided snapshot.

    // 3. Let newExposedDevices be the result of creating a list of device info objects with mediaDevices and deviceList.
    Vector<GC::Ref<MediaDeviceInfo>> new_exposed_devices = create_list_of_device_info_objects(device_list);

    // 4. If the MediaDeviceInfo objects in newExposedDevices match those in lastExposedDevices and have
    //    the same order, then abort these steps.
    if (media_device_info_lists_match(new_exposed_devices, last_exposed_devices))
        return;

    // 5. Set mediaDevices.[[storedDeviceList]] to deviceList.
    m_stored_device_list = device_list;

    // 6. Queue a task that fires an event named devicechange, using the DeviceChangeEvent constructor
    //    with devices initialized to newExposedDevices, at mediaDevices.
    // FIXME: Dispatch DeviceChangeEvent with devices once DeviceChangeEvent is implemented in LibWeb.
    HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(*this), GC::create_function(heap(), [media_devices = GC::Ref(*this)] {
        media_devices->dispatch_event(DOM::Event::create(media_devices->realm(), HTML::EventNames::devicechange));
    }));
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-enumeratedevices
GC::Ref<WebIDL::Promise> MediaDevices::enumerate_devices()
{
    JS::Realm& realm = this->realm();

    // 1. Let p be a new promise.
    GC::Ref<WebIDL::Promise> promise = WebIDL::create_promise(realm);

    // 2. Let proceed be the result of device enumeration can proceed with this.
    bool proceed = device_enumeration_can_proceed();

    // 3. Let mediaDevices be this.
    GC::Root<MediaDevices> media_devices = GC::make_root(this);

    struct EnumerateDevicesState final : public RefCounted<EnumerateDevicesState> {
        explicit EnumerateDevicesState(GC::Root<WebIDL::Promise> promise)
            : promise(move(promise))
        {
        }

        GC::Root<WebIDL::Promise> promise;
        Vector<Media::AudioDeviceInfo> input_devices;
        Vector<Media::AudioDeviceInfo> output_devices;
        bool settled { false };
    };

    RefPtr<EnumerateDevicesState> state = adopt_ref(*new EnumerateDevicesState(GC::make_root(promise)));

    auto maybe_resolve = [state, media_devices, proceed]() mutable {
        if (state->settled)
            return;

        // Keep the current snapshot of available devices in storedDeviceList.
        media_devices->m_stored_device_list.clear();

        for (auto const& device : state->input_devices) {
            String device_id = String::from_utf8_with_replacement_character(device.dom_device_id.view());
            String label = String::from_utf8_with_replacement_character(device.label.view());

            MediaDevices::StoredDevice stored_device {
                .dom_device_id = device_id,
                .kind = "audioinput"_string,
                .label = label,
                .group_id = String {},
                .is_default = device.is_default,
            };
            media_devices->m_stored_device_list.append(move(stored_device));
        }

        for (auto const& device : state->output_devices) {
            String device_id = String::from_utf8_with_replacement_character(device.dom_device_id.view());
            String label = String::from_utf8_with_replacement_character(device.label.view());

            MediaDevices::StoredDevice stored_device {
                .dom_device_id = device_id,
                .kind = "audiooutput"_string,
                .label = label,
                .group_id = String {},
                .is_default = device.is_default,
            };
            media_devices->m_stored_device_list.append(move(stored_device));
        }

        // 4. Run the following steps in parallel.
        // 4.1 While proceed is false, the User Agent MUST wait to proceed to the next step until a task
        //     queued to set proceed to the result of device enumeration can proceed with mediaDevices, would
        //     set proceed to true.
        if (!proceed) {
            // FIXME: Wait asynchronously for device enumeration can proceed to become true.
        }

        // 4.2 Let resultList be the result of creating a list of device info objects with mediaDevices and mediaDevices.[[storedDeviceList]].
        Vector<GC::Ref<MediaDeviceInfo>> result_list = media_devices->create_list_of_device_info_objects(media_devices->m_stored_device_list);

        state->settled = true;

        // 4.3 resolve p with resultList.
        HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(*media_devices), GC::create_function(media_devices->heap(), [state, media_devices, result_list = move(result_list)] {
            JS::Realm& realm = HTML::relevant_realm(*state->promise->promise());
            HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            GC::Ref<JS::Array> array = MUST(JS::Array::create(realm, result_list.size()));
            for (size_t index = 0; index < result_list.size(); ++index) {
                JS::PropertyKey property_index { index };
                if (array->create_data_property(property_index, result_list[index]).is_error()) {
                    WebIDL::reject_promise(realm, *state->promise, WebIDL::OperationError::create(realm, "Failed to build enumerateDevices result"_utf16));
                    return;
                }
            }

            WebIDL::resolve_promise(realm, *state->promise, array);
        }));
    };

    state->input_devices = Media::AudioDevices::the().input_devices();
    state->output_devices = Media::AudioDevices::the().output_devices();
    maybe_resolve();

    // 5. Return p.
    return promise;
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-getsupportedconstraints
MediaTrackSupportedConstraints MediaDevices::get_supported_constraints()
{
    // Returns a dictionary whose members are the constrainable properties known to the User Agent.
    MediaTrackSupportedConstraints supported_constraints;
    supported_constraints.device_id = true;
    return supported_constraints;
}

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-getusermedia
GC::Ref<WebIDL::Promise> MediaDevices::get_user_media(Optional<MediaStreamConstraints> const& constraints)
{
    // FIXME: Permission, visibility, and device selection. Honor "run these steps in parallel".
    JS::Realm& realm = this->realm();
    JS::VM& vm = realm.vm();

    bool audio_requested = false;
    bool video_requested = false;
    Optional<Vector<String>> requested_device_ids;

    // 1. Let constraints be the method's first argument.
    if (!constraints.has_value())
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::TypeError>("getUserMedia requires constraints"sv));

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
    if (!audio_requested && !video_requested)
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::TypeError>("No media types requested"sv));

    // 4. Let document be the relevant global object's associated Document.
    auto const& document = as<HTML::Window>(realm.global_object()).associated_document();

    // 5. If document is NOT fully active, return a promise rejected with an InvalidStateError.
    if (!document.is_fully_active())
        return WebIDL::create_rejected_promise(realm, WebIDL::InvalidStateError::create(realm, "Document is not fully active"_utf16));

    // 6. If requestedMediaTypes contains "audio" and document is not allowed to use the feature identified by the "microphone" permission name, jump to Permission Failure.
    // FIXME: Do microphone permission policy checks once PolicyControlledFeature includes microphone.
    // 7. If requestedMediaTypes contains "video" and document is not allowed to use the feature identified by the "camera" permission name, jump to Permission Failure.
    // FIXME: Do camera permission policy checks once PolicyControlledFeature includes camera.

    // 8. Let mediaDevices be this.
    // 9. Let isInView be the result of the is in view algorithm.
    bool is_in_view = document.visibility_state() == HTML::VisibilityState::Visible;

    // 10. Let p be a new promise.
    if (video_requested)
        return WebIDL::create_rejected_promise(realm, WebIDL::NotSupportedError::create(realm, "Video capture is not supported"_utf16));

    auto promise = WebIDL::create_promise(realm);

    auto promise_root = GC::make_root(promise);
    auto media_devices = GC::make_root(this);

    // 11.1 While isInView is false, the User Agent MUST wait to proceed to the next step until a task queued to set isInView to the result of the is in view algorithm, would set isInView to true.
    if (!is_in_view) {
        // FIXME: Wait to proceed until is_in_view becomes true.
    }

    // 11.2 Let finalSet be an (initially) empty set.
    Vector<Media::AudioDeviceInfo> final_set;

    // 11.3 For each media type kind in requestedMediaTypes, run the following steps.
    {
        auto& realm = HTML::relevant_realm(*promise->promise());
        auto const& document = as<HTML::Window>(realm.global_object()).associated_document();

        Vector<Media::AudioDeviceInfo> devices;
        for (auto const& device : Media::AudioDevices::the().input_devices()) {
            if (device.type == AudioServer::DeviceInfo::Type::Input)
                devices.append(device);
        }

        // 11.3.1 For each possible configuration of each possible source device of media of type kind, conceive a candidate as a placeholder for an eventual MediaStreamTrack holding a source device and configured with a settings dictionary comprised of its specific settings.
        //     Call this set of candidates the candidateSet.
        Vector<Media::AudioDeviceInfo> candidate_set = move(devices);

        // 11.13 NotFound Failure.
        // 11.13.2 Reject p with a new DOMException object whose name attribute has the value "NotFoundError".
        if (candidate_set.is_empty()) {
            WebIDL::reject_promise(realm, *promise, WebIDL::NotFoundError::create(realm, "No audio input devices available"_utf16));
            return promise;
        }

        // 11.3.2 If the value of the kind entry of constraints is true, set CS to the empty constraint set (no constraint). Otherwise, continue with CS set to the value of the kind entry of constraints.
        // 11.3.3 Remove any constrainable property inside of CS that are not defined for MediaStreamTrack objects of type kind.
        // 11.3.4 If CS contains a member that is a required constraint and whose name is not in the list of allowed required constraints for device selection, then reject p with a TypeError, and abort these steps.
        // 11.3.5 Run the SelectSettings algorithm on each candidate in candidateSet with CS as the constraint set.
        if (requested_device_ids.has_value() && !requested_device_ids->is_empty()) {
            Vector<Media::AudioDeviceInfo> filtered_candidates;
            for (auto const& device : candidate_set) {
                String device_id = String::from_utf8_with_replacement_character(device.dom_device_id.view());
                for (auto const& requested_id : *requested_device_ids) {
                    if (device_id == requested_id) {
                        filtered_candidates.append(device);
                        break;
                    }
                }
            }
            candidate_set = move(filtered_candidates);

            if (candidate_set.is_empty()) {
                // 11.14 Constraint Failure.
                // 11.14.3 Reject p with a new OverconstrainedError created by calling OverconstrainedError(constraint, message).
                // FIXME: Add OverconstrainedError plumbing and reject with it when constraints cannot be satisfied.
                WebIDL::reject_promise(realm, *promise, WebIDL::NotFoundError::create(realm, "Requested audio input device not found"_utf16));
                return promise;
            }
        }

        // 11.3.6 Read the current permission state for all candidate devices in candidateSet that are not attached to a live MediaStreamTrack in the current Document. Remove from candidateSet any candidate whose device's permission state is "denied".
        // FIXME: Integrate permission state checks and remove denied candidates.
        // 11.3.7 Optionally, e.g., based on a previously-established user preference, for security reasons, or due to platform limitations, jump to the step labeled Permission Failure below.
        // FIXME: Integrate optional UA-level permission failure policy hooks.

        // 11.3.8 Add all candidates from candidateSet to finalSet.
        final_set = move(candidate_set);

        // 11.4 Let stream be a new and empty MediaStream object.
        GC::Ref<MediaStream> stream = MediaStream::create(realm);

        // 11.5 For each media type kind in requestedMediaTypes, run the following sub steps, preferably at the same time.
        // 11.5.1 Request permission to use a PermissionDescriptor with its name member set to the permission name associated with kind.
        // FIXME: Integrate explicit permission prompting for getUserMedia.
        // 11.5.2 If the result of the request is "denied", jump to the step labeled Permission Failure below.
        // FIXME: On denied permission, reject with NotAllowedError.

        // 11.6 Let hasSystemFocus be false.
        bool has_system_focus = false;
        // 11.7 While hasSystemFocus is false, the User Agent MUST wait to proceed to the next step until a task queued to set hasSystemFocus to the result of the has system focus algorithm, would set hasSystemFocus to true.
        has_system_focus = document.has_focus();
        if (!has_system_focus) {
            // FIXME: Wait to proceed until has_system_focus becomes true.
        }

        // 11.8 Set the device information exposure on mediaDevices with requestedMediaTypes and true.
        media_devices->set_device_information_exposure(audio_requested, video_requested, true);

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

        // 11.9.2 The result of the request is "granted".
        if (!final_candidate.has_value()) {
            WebIDL::reject_promise(realm, *promise, WebIDL::NotReadableError::create(realm, "No readable audio input devices available"_utf16));
            return promise;
        }

        // 11.9.3 Let grantedDevice be finalCandidate's source device.
        Media::AudioDeviceInfo const& granted_device = final_candidate.value();

        // 11.9.4 Using grantedDevice's deviceId, deviceId, set mediaDevices.[[devicesLiveMap]][deviceId] to true, if it isn't already true, and set mediaDevices.[[devicesAccessibleMap]][deviceId] to true, if it isn't already true.
        String granted_device_id = String::from_utf8_with_replacement_character(granted_device.dom_device_id.view());
        media_devices->m_devices_live_map.set(granted_device_id, true);
        media_devices->m_devices_accessible_map.set(granted_device_id, true);

        // 11.9.5 Let track be the result of creating a MediaStreamTrack with grantedDevice and mediaDevices. The source of the MediaStreamTrack MUST NOT change.
        GC::Ref<MediaStreamTrack> track = MediaStreamTrack::create_audio_input_track(realm,
            granted_device,
            String::from_utf8_with_replacement_character(granted_device.label.view()));

        // 11.9.6 Add track to stream's track set.
        stream->add_track(track);

        // 11.10 Run the ApplyConstraints algorithm on all tracks in stream with the appropriate constraints.
        // FIXME: Run ApplyConstraints for selected tracks and surface Constraint Failure when needed.

        // 11.11 For each track in stream, tie track source to MediaDevices with track.[[Source]] and mediaDevices.
        media_devices->m_media_stream_track_sources.set(track->provider_id());

        // 11.12 Resolve p with stream and abort these steps.
        auto stream_root = GC::make_root(stream);
        HTML::queue_global_task(HTML::Task::Source::DOMManipulation, HTML::relevant_global_object(*media_devices), GC::create_function(media_devices->heap(), [promise, stream_root = move(stream_root)] {
            auto& realm = HTML::relevant_realm(*promise->promise());
            HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
            WebIDL::resolve_promise(realm, *promise, stream_root);
        }));
    }

    if (final_set.is_empty()) {
        // Final set is checked in the algorithm above; this preserves previous failure behavior.
    }

    // 11.15 Permission Failure: Reject p with a new DOMException object whose name attribute has the value "NotAllowedError".
    // FIXME: Route permission-denied paths through NotAllowedError once permission prompting is implemented.

    // Return p.
    return promise;
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

void MediaDevices::did_observe_audio_device_cache_update()
{
    auto input_devices = Media::AudioDevices::the().input_devices();
    auto output_devices = Media::AudioDevices::the().output_devices();

    size_t input_count = input_devices.size();
    size_t output_count = output_devices.size();

    Vector<StoredDevice> updated_device_list;
    updated_device_list.ensure_capacity(input_count + output_count);

    for (auto const& device : input_devices) {
        updated_device_list.append(StoredDevice {
            .dom_device_id = String::from_utf8_with_replacement_character(device.dom_device_id.view()),
            .kind = AUDIO_INPUT_KIND,
            .label = String::from_utf8_with_replacement_character(device.label.view()),
            .group_id = String::from_utf8_with_replacement_character(device.group_id.view()),
            .is_default = device.is_default,
        });
    }

    for (auto const& device : output_devices) {
        updated_device_list.append(StoredDevice {
            .dom_device_id = String::from_utf8_with_replacement_character(device.dom_device_id.view()),
            .kind = AUDIO_OUTPUT_KIND,
            .label = String::from_utf8_with_replacement_character(device.label.view()),
            .group_id = String::from_utf8_with_replacement_character(device.group_id.view()),
            .is_default = device.is_default,
        });
    }

    run_device_change_notification_steps(updated_device_list);
}

static Optional<Vector<String>> dom_string_values_from_variant(Variant<String, Vector<String>> const& value)
{
    Vector<String> values;
    if (value.has<String>()) {
        auto const& string_value = value.get<String>();
        if (!string_value.is_empty())
            values.append(string_value);
    } else {
        for (auto const& entry : value.get<Vector<String>>()) {
            if (!entry.is_empty())
                values.append(entry);
        }
    }
    return values.is_empty() ? Optional<Vector<String>> {} : Optional<Vector<String>> { move(values) };
}

static Optional<Vector<String>> extract_dom_string_constraint_values(ConstrainDOMString const& constraint)
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

    if (constraint.has<String>())
        return dom_string_values_from_variant(Variant<String, Vector<String>> { constraint.get<String>() });

    return dom_string_values_from_variant(Variant<String, Vector<String>> { constraint.get<Vector<String>>() });
}

static Optional<Vector<String>> extract_device_id_constraint(Optional<ConstrainDOMString> const& device_id_value)
{
    if (!device_id_value.has_value())
        return {};

    return extract_dom_string_constraint_values(*device_id_value);
}

}
