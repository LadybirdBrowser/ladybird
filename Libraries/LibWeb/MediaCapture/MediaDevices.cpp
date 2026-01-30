/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16FlyString.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibJS/Runtime/Value.h>
#include <LibMedia/MediaCapture/AudioInputDevices.h>
#include <LibMedia/MediaCapture/AudioOutputDevices.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaDeviceInfoPrototype.h>
#include <LibWeb/Bindings/MediaDevicesPrototype.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MediaCapture/MediaDeviceInfo.h>
#include <LibWeb/MediaCapture/MediaDevices.h>
#include <LibWeb/MediaCapture/MediaStream.h>
#include <LibWeb/MediaCapture/MediaStreamTrack.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapture {

GC_DEFINE_ALLOCATOR(MediaDevices);

GC::Ref<MediaDevices> MediaDevices::create(JS::Realm& realm)
{
    return realm.create<MediaDevices>(realm);
}

MediaDevices::MediaDevices(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
}

MediaDevices::~MediaDevices() = default;

void MediaDevices::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaDevices);
    Base::initialize(realm);
}

static String device_id_for(ByteString const& persistent_id, u64 device_id)
{
    if (!persistent_id.is_empty())
        return String::from_utf8_with_replacement_character(persistent_id.view());
    return String::number(device_id);
}

static String label_for(ByteString const& label)
{
    if (label.is_empty())
        return {};
    return String::from_utf8_with_replacement_character(label.view());
}

static Optional<String> extract_exact_device_id(JS::VM& vm, JS::Object& device_id_object)
{
    JS::PropertyKey exact_key { "exact"_utf16_fly_string };
    JS::ThrowCompletionOr<JS::Value> exact_value_or_error = device_id_object.get(exact_key);
    if (exact_value_or_error.is_error())
        return {};

    JS::Value exact_value = exact_value_or_error.release_value();
    if (exact_value.is_undefined() || exact_value.is_null())
        return {};

    if (!exact_value.is_string()) {
        JS::ThrowCompletionOr<String> exact_string_or_error = exact_value.to_string(vm);
        if (exact_string_or_error.is_error())
            return {};
        return exact_string_or_error.release_value();
    }

    return exact_value.as_string().utf8_string();
}

static Optional<String> extract_device_id_constraint(JS::VM& vm, JS::Value device_id_value)
{
    if (device_id_value.is_undefined() || device_id_value.is_null())
        return {};

    if (device_id_value.is_string())
        return device_id_value.as_string().utf8_string();

    if (!device_id_value.is_object()) {
        JS::ThrowCompletionOr<String> string_or_error = device_id_value.to_string(vm);
        if (string_or_error.is_error())
            return {};
        return string_or_error.release_value();
    }

    JS::Object& device_id_object = device_id_value.as_object();
    return extract_exact_device_id(vm, device_id_object);
}

GC::Ref<WebIDL::Promise> MediaDevices::enumerate_devices()
{
    // https://w3c.github.io/mediacapture-main/#dom-mediadevices-enumeratedevices
    // FIXME: Apply access control and visibility checks. Honor "run these steps in parallel".
    JS::Realm& realm = this->realm();

    // 1. Let p be a new promise.
    // 2. Let proceed be the result of device enumeration can proceed with this.
    // 3. Let mediaDevices be this.
    auto const& document = as<HTML::Window>(realm.global_object()).associated_document();
    bool proceed = document.is_fully_active() && document.visibility_state() == "visible"sv;
    if (!proceed) {
        // FIXME: If proceed is false, wait to proceed until it becomes true.
    }
    // 4. Run the following steps in parallel.
    // 5. Return p.
    // AD-HOC: We enumerate synchronously and resolve immediately.
    ErrorOr<Vector<Media::Capture::AudioInputDeviceInfo>> input_devices_or_error = Media::Capture::AudioInputDevices::enumerate();
    if (input_devices_or_error.is_error())
        return WebIDL::create_rejected_promise(realm, WebIDL::OperationError::create(realm, "Failed to enumerate audio input devices"_utf16));

    ErrorOr<Vector<Media::Capture::AudioOutputDeviceInfo>> output_devices_or_error = Media::Capture::AudioOutputDevices::enumerate();
    if (output_devices_or_error.is_error())
        return WebIDL::create_rejected_promise(realm, WebIDL::OperationError::create(realm, "Failed to enumerate audio output devices"_utf16));

    Vector<Media::Capture::AudioInputDeviceInfo> input_devices = input_devices_or_error.release_value();
    Vector<Media::Capture::AudioOutputDeviceInfo> output_devices = output_devices_or_error.release_value();

    // 4.2. Let resultList be the result of creating a list of device info objects with mediaDevices and mediaDevices.[[storedDeviceList]].
    GC::Ref<JS::Array> array = MUST(JS::Array::create(realm, 0));
    size_t index = 0;

    auto append_device = [&](String device_id, Bindings::MediaDeviceKind kind, String label) {
        GC::Ref<MediaDeviceInfo> device_info = MediaDeviceInfo::create(realm, move(device_id), kind, move(label), String {});
        JS::PropertyKey property_index { index++ };
        MUST(array->create_data_property(property_index, device_info));
    };

    for (auto const& device : input_devices) {
        append_device(device_id_for(device.persistent_id, device.device_id), Bindings::MediaDeviceKind::Audioinput, label_for(device.label));
    }

    for (auto const& device : output_devices) {
        append_device(device_id_for(device.persistent_id, device.device_id), Bindings::MediaDeviceKind::Audiooutput, label_for(device.label));
    }

    // 4.3. [resolve] p with resultList.
    return WebIDL::create_resolved_promise(realm, array);
}

GC::Ref<WebIDL::Promise> MediaDevices::get_user_media(Optional<GC::Root<JS::Object>> const& constraints)
{
    // https://w3c.github.io/mediacapture-main/#dom-mediadevices-getusermedia
    // FIXME: Permission, visibility, and device selection. Honor "run these steps in parallel".
    JS::Realm& realm = this->realm();
    JS::VM& vm = realm.vm();

    bool audio_requested = false;
    bool video_requested = false;
    Optional<String> requested_device_id;

    // 1. Let constraints be the method's first argument.
    if (!constraints.has_value())
        return WebIDL::create_rejected_promise_from_exception(realm, vm.throw_completion<JS::TypeError>("getUserMedia requires constraints"sv));

    // 2. Let requestedMediaTypes be the set of media types in constraints with either a dictionary value or a value of true.
    JS::Object* constraints_object = constraints->ptr();
    JS::PropertyKey audio_key { "audio"_utf16_fly_string };
    JS::ThrowCompletionOr<JS::Value> audio_value_or_error = constraints_object->get(audio_key);
    if (audio_value_or_error.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, audio_value_or_error.release_error());

    JS::Value audio_value = audio_value_or_error.release_value();
    if (!audio_value.is_undefined()) {
        if (audio_value.is_boolean()) {
            audio_requested = audio_value.as_bool();
        } else if (audio_value.is_object()) {
            audio_requested = true;
            JS::Object& audio_object = audio_value.as_object();
            JS::PropertyKey device_id_key { "deviceId"_utf16_fly_string };
            JS::ThrowCompletionOr<JS::Value> device_id_value_or_error = audio_object.get(device_id_key);
            if (device_id_value_or_error.is_error())
                return WebIDL::create_rejected_promise_from_exception(realm, device_id_value_or_error.release_error());

            requested_device_id = extract_device_id_constraint(vm, device_id_value_or_error.release_value());
        } else {
            audio_requested = true;
        }
    }

    JS::PropertyKey video_key { "video"_utf16_fly_string };
    JS::ThrowCompletionOr<JS::Value> video_value_or_error = constraints_object->get(video_key);
    if (video_value_or_error.is_error())
        return WebIDL::create_rejected_promise_from_exception(realm, video_value_or_error.release_error());

    JS::Value video_value = video_value_or_error.release_value();
    if (!video_value.is_undefined()) {
        if (video_value.is_boolean())
            video_requested = video_value.as_bool();
        else
            video_requested = true;
    }

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
    bool is_in_view = document.visibility_state() == "visible"sv;
    if (!is_in_view) {
        // FIXME: If is_in_view is false, wait to proceed until it becomes true.
    }

    // 10. Let p be a new promise.
    if (video_requested)
        return WebIDL::create_rejected_promise(realm, WebIDL::NotSupportedError::create(realm, "Video capture is not supported"_utf16));

    // 11. Run the following steps in parallel.
    // 12. Let finalSet be the candidates that satisfy constraints and permissions.
    // 13. Select a final candidate and create tracks, then resolve p with the stream.
    ErrorOr<Vector<Media::Capture::AudioInputDeviceInfo>> devices_or_error = Media::Capture::AudioInputDevices::enumerate();
    if (devices_or_error.is_error())
        return WebIDL::create_rejected_promise(realm, WebIDL::OperationError::create(realm, "Failed to enumerate audio input devices"_utf16));

    Vector<Media::Capture::AudioInputDeviceInfo> devices = devices_or_error.release_value();
    if (devices.is_empty())
        return WebIDL::create_rejected_promise(realm, WebIDL::NotFoundError::create(realm, "No audio input devices available"_utf16));

    Optional<Media::Capture::AudioInputDeviceInfo> selected_device;
    if (requested_device_id.has_value() && !requested_device_id->is_empty()) {
        for (auto const& device : devices) {
            String device_id = device_id_for(device.persistent_id, device.device_id);
            if (device_id == requested_device_id.value()) {
                selected_device = device;
                break;
            }
        }

        if (!selected_device.has_value())
            return WebIDL::create_rejected_promise(realm, WebIDL::NotFoundError::create(realm, "Requested audio input device not found"_utf16));
    } else {
        for (auto const& device : devices) {
            if (device.is_default) {
                selected_device = device;
                break;
            }
        }
        if (!selected_device.has_value())
            selected_device = devices.first();
    }

    GC::Ref<MediaStreamTrack> track = MediaStreamTrack::create_audio_input_track(realm,
        selected_device->device_id,
        selected_device->sample_rate_hz,
        selected_device->channel_count,
        label_for(selected_device->label));

    GC::Ref<MediaStream> stream = MediaStream::create(realm);
    stream->add_track(track);

    return WebIDL::create_resolved_promise(realm, stream);
}

void MediaDevices::set_ondevicechange(WebIDL::CallbackType* event_handler)
{
    // https://w3c.github.io/mediacapture-main/#dom-mediadevices-ondevicechange
    // The event type of this event handler is devicechange.
    set_event_handler_attribute(HTML::EventNames::devicechange, event_handler);
}

WebIDL::CallbackType* MediaDevices::ondevicechange()
{
    // https://w3c.github.io/mediacapture-main/#dom-mediadevices-ondevicechange
    return event_handler_attribute(HTML::EventNames::devicechange);
}

}
