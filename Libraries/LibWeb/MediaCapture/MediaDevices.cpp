/*
 * Copyright (c) 2025, Mehran Kamal <me@mehrankamal.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Function.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaDevicesPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/Platform/MediaDevice.h>
#include <LibWeb/WebIDL/Promise.h>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_camera.h>
#include <SDL3/SDL_error.h>

namespace Web::MediaCapture {

constexpr static Platform::MediaDevice create_media_device(Platform::MediaDeviceKind kind, char const* label, bool is_default)
{
    return {
        .kind = kind,
        .label = String::formatted("{}", label).release_value(),
        .is_default = is_default
    };
}

static Vector<Platform::MediaDevice> sdl_cameras()
{
    i32 cameras_count = 0;
    auto* sdl_cameras = SDL_GetCameras(&cameras_count);
    if (sdl_cameras == NULL) {
        dbgln("SDL failed to fetch camera devices: {}", SDL_GetError());
        return {};
    }

    Vector<Platform::MediaDevice> cameras;
    cameras.ensure_capacity(cameras_count);
    for (i32 i = 0; i < cameras_count; i++) {
        auto const* device_name = SDL_GetCameraName(*(sdl_cameras + i));
        cameras.append(create_media_device(Platform::MediaDeviceKind::VideoInput,
            device_name,
            i == 0));
    }

    SDL_free(sdl_cameras);

    return cameras;
}

static Vector<Platform::MediaDevice> sdl_microphones()
{
    i32 microphones_count = 0;
    auto* sdl_microphones = SDL_GetAudioRecordingDevices(&microphones_count);
    if (sdl_microphones == NULL) {
        dbgln("SDL failed to fetch microphone devices: {}", SDL_GetError());
        return {};
    }

    Vector<Platform::MediaDevice> microphones;
    microphones.ensure_capacity(microphones_count);
    for (i32 i = 0; i < microphones_count; i++) {
        auto const* device_name = SDL_GetAudioDeviceName(*(sdl_microphones + i));
        microphones.append(create_media_device(Platform::MediaDeviceKind::AudioInput,
            device_name,
            *(sdl_microphones + i) == SDL_AUDIO_DEVICE_DEFAULT_RECORDING));
    }

    SDL_free(sdl_microphones);

    return microphones;
}

static Vector<Platform::MediaDevice> sdl_speakers()
{
    i32 speakers_count = 0;
    auto* sdl_speakers = SDL_GetAudioPlaybackDevices(&speakers_count);
    if (sdl_speakers == NULL) {
        dbgln("SDL failed to fetch speaker devices: {}", SDL_GetError());
        return {};
    }

    Vector<Platform::MediaDevice> speakers;
    for (i32 i = 0; i < speakers_count; i++) {
        auto const* device_name = SDL_GetAudioDeviceName(*(sdl_speakers + i));
        speakers.append(create_media_device(Platform::MediaDeviceKind::AudioOutput,
            device_name,
            *(sdl_speakers + i) == SDL_AUDIO_DEVICE_DEFAULT_RECORDING));
    }

    SDL_free(sdl_speakers);

    return speakers;
}

static Vector<Platform::MediaDevice> sdl_media_devices()
{
    Vector<Platform::MediaDevice> media_devices;

    media_devices.extend(sdl_microphones());
    media_devices.extend(sdl_speakers());
    media_devices.extend(sdl_cameras());

    return media_devices;
}

GC_DEFINE_ALLOCATOR(MediaDevices);

// https://www.w3.org/TR/mediacapture-streams/#dfn-create-a-mediadevices
GC::Ref<MediaDevices> MediaDevices::create(JS::Realm& realm)
{
    // 1. Let mediaDevices be a new MediaDevices object in realm, initalized
    //    with the following internal slots:
    //      [[devicesLiveMap]], initialized to an empty map.
    //      [[devicesAccessibleMap]], initialized to an empty map.
    //      [[kindsAccessibleMap]], initialized to an empty map.
    //      [[storedDeviceList]], initialized to a list of all media input and
    //          output devices available to the User Agent.
    //      [[canExposeCameraInfo]], initialized to false.
    //      [[canExposeMicrophoneInfo]], initialized to false.
    //      [[mediaStreamTrackSources]], initialized to an empty set.
    auto media_devices = realm.create<MediaDevices>(realm);
    media_devices->m_stored_devices = sdl_media_devices();

    // FIXME: 2. Let settings be mediaDevices's relevant settings object.
    // FIXME: 3. For each kind of device, kind, that MediaDevices.getUserMedia()
    //           exposes, run the following step:
    //      FIXME: 1. Set mediaDevices.[[kindsAccessibleMap]][kind] to either
    //           true if the permission state of the permission associated with
    //           kind (e.g. "camera", "microphone") for settings is "granted",
    //           or to false otherwise.
    // FIXME: 4. For each individual device that MediaDevices.getUserMedia()
    //           exposes, using the device's deviceId, deviceId, run the
    //           following step:
    //      FIXME: 1. Set mediaDevices.[[devicesLiveMap]][deviceId] to false,
    //                and set mediaDevices.[[devicesAccessibleMap]][deviceId]
    //                to either true if the permission state of the permission
    //                associated with the device’s kind and deviceId for
    //                settings, is "granted", or to false otherwise.

    // 5. Return mediaDevices.
    return media_devices;
}

// https://www.w3.org/TR/mediacapture-streams/#dom-mediadevices-enumeratedevices
GC::Ref<WebIDL::Promise> MediaDevices::enumerate_devices()
{

    // 1. Let p be a new promise.
    auto promise = WebIDL::create_promise(realm());

    // 2. Let proceed be the result of device enumeration can proceed with this.
    auto proceed = device_enumeration_can_proceed();

    // 3. Let mediaDevices be this.
    // 4. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm().heap(), [this, promise, proceed]() mutable {
        //      1. While proceed is false, the User Agent MUST wait to proceed
        //         to the next step until a task queued to set proceed to the
        //         result of device enumeration can proceed with mediaDevices,
        //         would set proceed to true.
        Platform::EventLoopPlugin::the().spin_until(GC::create_function(realm().heap(), [proceed, this]() mutable {
            proceed = device_enumeration_can_proceed();
            return proceed;
        }));
        //      2. Let resultList be the result of creating a list of device
        //         info objects with mediaDevices and mediaDevices.[[storedDeviceList]].
        auto device_infos = create_list_of_device_info_objects();
        GC::RootVector<JS::Value> results(realm().heap());
        for (auto& device_info : device_infos) {
            results.append(JS::Value(device_info.ptr()));
        }

        auto result = JS::Array::create_from(realm(), results);

        HTML::TemporaryExecutionContext execution_context { realm() };
        // 5. resolve p with resultList.
        WebIDL::resolve_promise(realm(), promise, result);
    }));

    // 6. Return p.
    return promise;
}

// https://www.w3.org/TR/mediacapture-streams/#device-enumeration-can-proceed
bool MediaDevices::device_enumeration_can_proceed()
{
    // 1. The User Agent MAY return true if device information can be exposed
    //    on mediaDevices.
    if (device_information_can_be_exposed())
        return true;

    // 2. Return the result of is in view with mediaDevices.
    return is_in_view();
}

// https://www.w3.org/TR/mediacapture-streams/#device-information-can-be-exposed
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

// https://www.w3.org/TR/mediacapture-streams/#camera-information-can-be-exposed
bool MediaDevices::camera_information_can_be_exposed()
{
    // FIXME: 1. If any of the local devices of kind "videoinput" are attached
    //           to a live MediaStreamTrack in mediaDevices's relevant global
    //           object's associated Document, return true.
    // 2. Return mediaDevices.[[canExposeCameraInfo]].
    return m_can_expose_camera_info;
}

// https://www.w3.org/TR/mediacapture-streams/#microphone-information-can-be-exposed
bool MediaDevices::microphone_information_can_be_exposed()
{
    // FIXME: 1. If any of the local devices of kind "audioinput" are attached
    //           to a live MediaStreamTrack in the relevant global object's
    //           associated Document, return true.
    // 2. Return mediaDevices.[[canExposeMicrophoneInfo]].
    return m_can_expose_microphone_info;
}

// https://www.w3.org/TR/mediacapture-streams/#dfn-is-in-view
bool MediaDevices::is_in_view()
{
    // 1. If mediaDevices's relevant global object's associated Document is
    //    fully active and its visibility state is "visible", return true.
    //    Otherwise, return false.
    auto associated_document = as<HTML::Window>(HTML::relevant_global_object(*this)).document();
    if (associated_document->is_fully_active() && associated_document->visibility_state() == "visible"sv)
        return true;

    return false;
}

// https://www.w3.org/TR/mediacapture-streams/#creating-a-list-of-device-info-objects
Vector<GC::Ref<MediaDeviceInfo>> MediaDevices::create_list_of_device_info_objects()
{
    // To perform creating a list of device info objects,
    // given mediaDevices and deviceList, run the following steps:
    //
    // Ad-Hoc: deviceList is [[storedDeviceList]] in referenced cases, hence not
    //         passed as parameters
    auto const& devices = m_stored_devices;

    // 1. Let resultList be an empty list.
    Vector<GC::Ref<MediaDeviceInfo>> results;
    // 2. Let microphoneList, cameraList and otherDeviceList be empty lists.
    Vector<GC::Ref<MediaDeviceInfo>> microphones;
    Vector<GC::Ref<MediaDeviceInfo>> cameras;
    Vector<GC::Ref<MediaDeviceInfo>> other_devices;

    // 3. Let document be mediaDevices's relevant global object's associated Document.
    auto& document = as<HTML::Window>(HTML::relevant_global_object(*this)).associated_document();

    // 4. Run the following sub steps for each discovered device in deviceList, device:
    for (auto const& device : devices) {
        //      1. If device is not a microphone, or document is not allowed
        //         to use the feature identified by "microphone", abort these
        //         sub steps and continue with the next device (if any).
        if (device.kind != Platform::MediaDeviceKind::AudioInput || !document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Microphone)) {
            continue;
        }
        //      2. Let deviceInfo be the result of creating a device info object
        //         to represent device, with mediaDevices.
        auto device_info = MediaDeviceInfo::create(realm(), device, *this);
        //      3. If device is the system default microphone, prepend
        //         deviceInfo to microphoneList. Otherwise, append deviceInfo
        //         to microphoneList.
        if (device.is_default) {
            microphones.prepend(device_info);
        } else {
            microphones.append(device_info);
        }
    }

    // 5. Run the following sub steps for each discovered device in deviceList, device:
    for (auto const& device : devices) {
        //      1. If device is not a camera, or document is not allowed to use
        //         the feature identified by "camera", abort these sub steps and
        //         continue with the next device (if any).
        if (device.kind != Platform::MediaDeviceKind::VideoInput || !document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Camera)) {
            continue;
        }
        //      2. Let deviceInfo be the result of creating a device info object
        //         to represent device, with mediaDevices.
        auto device_info = MediaDeviceInfo::create(realm(), device, *this);
        //      3. If device is the system default camera, prepend
        //         deviceInfo to cameraList. Otherwise, append deviceInfo to
        //         cameraList.
        if (device.is_default) {
            cameras.prepend(device_info);
        } else {
            cameras.append(device_info);
        }
    }

    // 6. If microphone information can be exposed on mediaDevices is false,
    //    truncate microphoneList to its first item.
    if (!microphone_information_can_be_exposed() && microphones.size() > 1) {
        microphones.remove(1, microphones.size() - 1);
    }

    // 7. If camera information can be exposed on mediaDevices is false,
    //    truncate cameraList to its first item.
    if (!camera_information_can_be_exposed() && cameras.size() > 1) {
        cameras.remove(1, cameras.size() - 1);
    }

    // 8. Run the following sub steps for each discovered device in deviceList, device:
    for (auto const& device : devices) {

        //      1. If device is a microphone or device is a camera, abort these
        //         sub steps and continue with the next device (if any).
        if (device.kind == Platform::MediaDeviceKind::AudioInput || device.kind == Platform::MediaDeviceKind::VideoInput) {
            continue;
        }
        //      2. Run the exposure decision algorithm for devices other than
        //         camera and microphone, with device, microphoneList, cameraList
        //         and mediaDevices as input. If the result of this algorithm is false,
        //         abort these sub steps and continue with the next device (if any).
        if (!do_expose_non_camera_and_non_microphone_device(device, microphones, cameras)) {
            continue;
        }
        //      3. Let deviceInfo be the result of creating a device info object
        //         to represent device, with mediaDevices.
        auto device_info = MediaDeviceInfo::create(realm(), device, *this);
        //      4. If device is the system default audio output, prepend
        //         deviceInfo to otherDeviceList. Otherwise, append deviceInfo
        //         to otherDeviceList.
        if (device.is_default && device.kind == Platform::MediaDeviceKind::AudioOutput) {
            other_devices.prepend(device_info);
        } else {
            other_devices.append(device_info);
        }
    }

    // 9. Append to resultList all devices of microphoneList in order.
    results.extend(microphones);
    // 10. Append to resultList all devices of cameraList in order.
    results.extend(cameras);
    // 11. Append to resultList all devices of otherDeviceList in order.
    results.extend(other_devices);

    // 12. Return resultList.
    return results;
}

// https://www.w3.org/TR/mediacapture-streams/#device-exposure-decision-non-camera-microphone
bool MediaDevices::do_expose_non_camera_and_non_microphone_device(Platform::MediaDevice const&, Vector<GC::Ref<MediaDeviceInfo>> const&, Vector<GC::Ref<MediaDeviceInfo>> const&)
{
    // The exposure decision algorithm for devices other than camera and microphone
    // takes a device, microphoneList, cameraList and mediaDevices as input and
    // returns a boolean to decide whether to expose information about device to the
    // web page or not.

    // By default, it returns false.

    // Other specifications can define the algorithm for specific device types.
    return false;
}

MediaDevices::MediaDevices(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

void MediaDevices::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaDevices);
    Base::initialize(realm);
}

void MediaDevices::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
}

}
