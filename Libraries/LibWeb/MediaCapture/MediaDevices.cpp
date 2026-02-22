/*
 * Copyright (c) 2026, The Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibMedia/MediaCapture/AudioInputDevices.h>
#include <LibMedia/MediaCapture/AudioOutputDevices.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaDeviceInfoPrototype.h>
#include <LibWeb/Bindings/MediaDevicesPrototype.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventNames.h>
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

// https://w3c.github.io/mediacapture-main/#dom-mediadevices-enumeratedevices
GC::Ref<WebIDL::Promise> MediaDevices::enumerate_devices()
{
    // FIXME: Apply access control and visibility checks. Honor "run these steps in parallel".
    JS::Realm& realm = this->realm();

    // 1. Let p be a new promise.
    // 2. Let proceed be the result of device enumeration can proceed with this.
    // 3. Let mediaDevices be this.
    auto const& document = as<HTML::Window>(realm.global_object()).associated_document();
    bool proceed = document.is_fully_active() && document.visibility_state() == HTML::VisibilityState::Visible;
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
        String device_id = device.persistent_id.is_empty()
            ? String::number(device.device_id)
            : String::from_utf8_with_replacement_character(device.persistent_id.view());
        String label = String::from_utf8_with_replacement_character(device.label.view());
        append_device(move(device_id), Bindings::MediaDeviceKind::Audioinput, move(label));
    }

    for (auto const& device : output_devices) {
        String device_id = device.persistent_id.is_empty()
            ? String::number(device.device_id)
            : String::from_utf8_with_replacement_character(device.persistent_id.view());
        String label = String::from_utf8_with_replacement_character(device.label.view());
        append_device(move(device_id), Bindings::MediaDeviceKind::Audiooutput, move(label));
    }

    // 4.3. [resolve] p with resultList.
    return WebIDL::create_resolved_promise(realm, array);
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
    if (requested_device_ids.has_value() && !requested_device_ids->is_empty()) {
        for (auto const& device : devices) {
            String device_id = device.persistent_id.is_empty()
                ? String::number(device.device_id)
                : String::from_utf8_with_replacement_character(device.persistent_id.view());
            for (auto const& requested_id : *requested_device_ids) {
                if (device_id == requested_id) {
                    selected_device = device;
                    break;
                }
            }
            if (selected_device.has_value())
                break;
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
        String::from_utf8_with_replacement_character(selected_device->label.view()));

    GC::Ref<MediaStream> stream = MediaStream::create(realm);
    stream->add_track(track);

    return WebIDL::create_resolved_promise(realm, stream);
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
