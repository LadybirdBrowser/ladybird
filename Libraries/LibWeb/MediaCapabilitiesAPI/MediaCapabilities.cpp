/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/BooleanObject.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaCapabilitiesPrototype.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MediaCapabilitiesAPI/MediaCapabilities.h>
#include <LibWeb/Platform/EventLoopPlugin.h>

namespace Web::MediaCapabilitiesAPI {

// https://w3c.github.io/media-capabilities/#valid-mediaconfiguration
bool MediaConfiguration::is_valid_media_configuration() const
{
    //  For a MediaConfiguration to be a valid MediaConfiguration, all of the following conditions MUST be true:

    // 1. audio and/or video MUST exist.
    if (!audio.has_value() && !video.has_value())
        return false;

    // 2. audio MUST be a valid audio configuration if it exists.
    if (audio.has_value() && !audio.value().is_valid_audio_configuration())
        return false;

    // 3. video MUST be a valid video configuration if it exists.
    if (video.has_value() && !video.value().is_valid_video_configuration())
        return false;

    return true;
}

// https://w3c.github.io/media-capabilities/#valid-mediadecodingconfiguration
bool MediaDecodingConfiguration::is_valid_media_decoding_configuration() const
{
    // For a MediaDecodingConfiguration to be a valid MediaDecodingConfiguration, all of the following
    // conditions MUST be true:

    // 1. It MUST be a valid MediaConfiguration.
    if (!is_valid_media_configuration())
        return false;

    // 2. If keySystemConfiguration exists:
    // FIXME: Implement this.

    return true;
}

// https://w3c.github.io/media-capabilities/#valid-audio-mime-type
bool is_valid_audio_mime_type(StringView string)
{
    // A valid audio MIME type is a string that is a valid media MIME type and for which the type per [RFC9110] is
    // either audio or application.
    auto mime_type = MimeSniff::MimeType::parse(string);
    if (!mime_type.has_value())
        return false;
    return mime_type->type() == "audio"sv || mime_type->type() == "application"sv;
}

// https://w3c.github.io/media-capabilities/#valid-video-mime-type
bool is_valid_video_mime_type(StringView string)
{
    // A valid video MIME type is a string that is a valid media MIME type and for which the type per [RFC9110] is
    // either video or application.
    auto mime_type = MimeSniff::MimeType::parse(string);
    if (!mime_type.has_value())
        return false;
    return mime_type->type() == "video"sv || mime_type->type() == "application"sv;
}

// https://w3c.github.io/media-capabilities/#valid-video-configuration
bool VideoConfiguration::is_valid_video_configuration() const
{
    // To check if a VideoConfiguration configuration is a valid video configuration, the following steps MUST be
    // run:

    // 1. If configuration’s contentType is not a valid video MIME type, return false and abort these steps.
    if (!is_valid_video_mime_type(content_type))
        return false;

    // 2. If framerate is not finite or is not greater than 0, return false and abort these steps.
    if (!isfinite(framerate) || framerate <= 0)
        return false;

    // 3. If an optional member is specified for a MediaDecodingType or MediaEncodingType to which it’s not
    //    applicable, return false and abort these steps. See applicability rules in the member definitions below.
    // FIXME: Implement this.

    // 4. Return true.
    return true;
}

// https://w3c.github.io/media-capabilities/#valid-video-configuration
bool AudioConfiguration::is_valid_audio_configuration() const
{
    // To check if a AudioConfiguration configuration is a valid audio configuration, the following steps MUST be
    // run:

    // 1. If configuration’s contentType is not a valid audio MIME type, return false and abort these steps.
    if (!is_valid_audio_mime_type(content_type))
        return false;

    // 2. Return true.
    return true;
}

GC_DEFINE_ALLOCATOR(MediaCapabilities);

GC::Ref<MediaCapabilities> MediaCapabilities::create(JS::Realm& realm)
{
    return realm.create<MediaCapabilities>(realm);
}

MediaCapabilities::MediaCapabilities(JS::Realm& realm)
    : PlatformObject(realm)
{
}

void MediaCapabilities::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaCapabilities);
}

// https://w3c.github.io/media-capabilities/#queue-a-media-capabilities-task
void queue_a_media_capabilities_task(JS::VM& vm, Function<void()> steps)
{
    // When an algorithm queues a Media Capabilities task T, the user agent MUST queue a global task T on the
    // media capabilities task source using the global object of the the current realm record.
    queue_global_task(HTML::Task::Source::MediaCapabilities, vm.current_realm()->global_object(), GC::create_function(vm.current_realm()->heap(), move(steps)));
}

// https://w3c.github.io/media-capabilities/#dom-mediacapabilities-decodinginfo
GC::Ref<WebIDL::Promise> MediaCapabilities::decoding_info(MediaDecodingConfiguration const& configuration)
{
    auto& realm = this->realm();
    // The decodingInfo() method MUST run the following steps:

    // 1. If configuration is not a valid MediaDecodingConfiguration, return a Promise rejected with a newly created
    //    TypeError.
    if (!configuration.is_valid_media_decoding_configuration()) {
        return WebIDL::create_rejected_promise_from_exception(realm, vm().throw_completion<JS::TypeError>("The given configuration is not a valid MediaDecodingConfiguration"sv));
    }

    // 2. If configuration.keySystemConfiguration exists, run the following substeps:
    // FIXME: Implement this.

    // 3. Let p be a new Promise.
    auto p = WebIDL::create_promise(realm);

    // 4. Run the following steps in parallel:
    auto& vm = this->vm();
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&vm, &realm, p, configuration]() mutable {
        HTML::TemporaryExecutionContext context(realm);
        // 1. Run the Create a MediaCapabilitiesDecodingInfo algorithm with configuration.
        auto result = create_a_media_capabilities_decoding_info(configuration).to_object(realm);

        // Queue a Media Capabilities task to resolve p with its result.
        queue_a_media_capabilities_task(vm, [&realm, p, result] {
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            WebIDL::resolve_promise(realm, p, JS::Value(result));
        });
    }));

    // 5. Return p.
    return p;
}

// https://w3c.github.io/media-capabilities/#create-a-mediacapabilitiesdecodinginfo
MediaCapabilitiesDecodingInfo create_a_media_capabilities_decoding_info(MediaDecodingConfiguration configuration)
{
    // 1. Let info be a new MediaCapabilitiesDecodingInfo instance. Unless stated otherwise, reading and
    //    writing apply to info for the next steps.
    MediaCapabilitiesDecodingInfo info = {};

    // 2. Set configuration to be a new MediaDecodingConfiguration. For every property in configuration create
    //    a new property with the same name and value in configuration.
    info.configuration = { { configuration.video, configuration.audio }, configuration.type, configuration.key_system_configuration };

    // 3. If configuration.keySystemConfiguration exists:
    if (false) {
        // FIXME: Implement this.
    }
    // 4. Otherwise, run the following steps:
    else {
        // 1. Set keySystemAccess to null.
        // FIXME: Implement this.

        // 2. If the user agent is able to decode the media represented by configuration, set supported to true.
        // 3. Otherwise, set it to false.
        info.supported = is_able_to_decode_media(configuration);
    }

    // 5. If the user agent is able to decode the media represented by configuration at the indicated framerate without
    //    dropping frames, set smooth to true. Otherwise set it to false.
    // FIXME: Actually check this.
    info.smooth = false;

    // 6. If the user agent is able to decode the media represented by configuration in a power efficient manner, set
    //    powerEfficient to true. Otherwise set it to false.
    // FIXME: Actually check this... somehow.
    info.power_efficient = false;

    // 7. Return info.
    return info;
}

bool is_able_to_decode_media(MediaDecodingConfiguration configuration)
{
    if (configuration.type != Bindings::MediaDecodingType::MediaSource)
        return false;

    if (configuration.video.has_value()) {
        auto video_mime_type = MimeSniff::MimeType::parse(configuration.video.value().content_type);
        if (!video_mime_type.has_value() || !Web::HTML::HTMLMediaElement::supported_video_subtypes.contains_slow(video_mime_type->subtype()))
            return false;
    }

    if (configuration.audio.has_value()) {
        auto audio_mime_type = MimeSniff::MimeType::parse(configuration.audio.value().content_type);
        if (!audio_mime_type.has_value() || !Web::HTML::HTMLMediaElement::supported_audio_subtypes.contains_slow(audio_mime_type->subtype()))
            return false;
    }

    return true;
}

GC::Ref<JS::Object> MediaCapabilitiesDecodingInfo::to_object(JS::Realm& realm)
{
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());

    // FIXME: Also include configuration in this object.

    MUST(object->create_data_property("supported"_fly_string, JS::BooleanObject::create(realm, supported)));
    MUST(object->create_data_property("smooth"_fly_string, JS::BooleanObject::create(realm, smooth)));
    MUST(object->create_data_property("powerEfficent"_fly_string, JS::BooleanObject::create(realm, power_efficient)));

    return object;
}

}
