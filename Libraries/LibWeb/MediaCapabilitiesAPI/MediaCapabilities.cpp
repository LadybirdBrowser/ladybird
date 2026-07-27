/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/BooleanObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibMedia/CodecID.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MediaCapabilitiesAPI/MediaCapabilities.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapabilitiesAPI {

static GC::Ref<JS::Object> media_capabilities_decoding_info_to_object(JS::Object& global_object, MediaCapabilitiesDecodingInfo const& info)
{
    auto& realm = global_object.shape().realm();
    auto object = JS::Object::create(realm, realm.intrinsics().object_prototype());

    // FIXME: Also include configuration in this object.

    MUST(object->create_data_property("supported"_utf16_fly_string, JS::BooleanObject::create(realm, info.supported)));
    MUST(object->create_data_property("smooth"_utf16_fly_string, JS::BooleanObject::create(realm, info.smooth)));
    MUST(object->create_data_property("powerEfficient"_utf16_fly_string, JS::BooleanObject::create(realm, info.power_efficient)));

    return object;
}

// https://w3c.github.io/media-capabilities/#queue-a-media-capabilities-task
static void queue_a_media_capabilities_task(JS::Object& global_object, Function<void()> steps)
{
    // When an algorithm queues a Media Capabilities task T, the user agent MUST queue a global task T on the
    // media capabilities task source using the global object of the the current realm record.
    HTML::queue_global_task(HTML::Task::Source::MediaCapabilities, global_object, GC::create_function(GC::Heap::the(), move(steps)));
}

// https://w3c.github.io/media-capabilities/#valid-mediaconfiguration
static bool is_valid_media_configuration(MediaDecodingConfiguration const& configuration)
{
    //  For a MediaConfiguration to be a valid MediaConfiguration, all of the following conditions MUST be true:

    // 1. audio and/or video MUST exist.
    if (!configuration.audio.has_value() && !configuration.video.has_value())
        return false;

    // 2. audio MUST be a valid audio configuration if it exists.
    if (configuration.audio.has_value() && !is_valid_audio_configuration(configuration.audio.value()))
        return false;

    // 3. video MUST be a valid video configuration if it exists.
    if (configuration.video.has_value() && !is_valid_video_configuration(configuration.video.value()))
        return false;

    return true;
}

// https://w3c.github.io/media-capabilities/#valid-mediadecodingconfiguration
bool is_valid_media_decoding_configuration(MediaDecodingConfiguration const& configuration)
{
    // For a MediaDecodingConfiguration to be a valid MediaDecodingConfiguration, all of the following
    // conditions MUST be true:

    // 1. It MUST be a valid MediaConfiguration.
    if (!is_valid_media_configuration(configuration))
        return false;

    // 2. If keySystemConfiguration exists:
    // FIXME: Implement this.

    return true;
}

// https://w3c.github.io/media-capabilities/#check-mime-type-validity
static bool check_mime_type_validity(MimeSniff::MimeType const& mime_type, StringView media)
{
    // To check MIME type validity given a MIME type record mimeType and a string media, run the following steps:

    // 1. If the type of mimeType per [RFC9110] is neither media nor application, return false.
    if (mime_type.type() != media && mime_type.type() != "application"sv)
        return false;

    // AD-HOC: The spec doesn't define which combined type and subtype members allow a single media codec, and which
    //         instead allow multiple media codecs. So this is based on the equivalent lists used by other engines.
    bool allows_multiple_media_codecs = mime_type.essence().is_one_of(
        "audio/mp4"sv, "audio/ogg"sv, "audio/webm"sv,
        "application/mp4"sv, "application/ogg"sv,
        "video/mp4"sv, "video/ogg"sv, "video/webm"sv);

    // 2. If the combined type and subtype members of mimeType allow a single media codec and the parameters member of
    //    mimeType is not empty, return false.
    if (!allows_multiple_media_codecs && !mime_type.parameters().is_empty())
        return false;

    // 3. If the combined type and subtype members of mimeType allow multiple media codecs, run the following steps:
    if (allows_multiple_media_codecs) {
        // 1. If the parameters member of mimeType does not contain a single key named "codecs", return false.
        auto codecs_iter = mime_type.parameters().find("codecs"sv);
        if (mime_type.parameters().size() != 1 || codecs_iter == mime_type.parameters().end())
            return false;

        // 2. If the value of mimeType.parameters["codecs"] does not describe a single media codec, return false.
        auto codec_string = codecs_iter->value.bytes_as_string_view().trim_whitespace();
        if (codec_string.is_empty() || codec_string.contains(','))
            return false;

        // AD-HOC: Reject a video config whose contentType gives an audio codec, and reject an audio config whose
        //         contentType describes a video codec — as WPT expects, even though the spec doesn't require it (yet).
        //         https://github.com/w3c/media-capabilities/issues/261
        auto track_type = Media::track_type_from_codec_id(Media::codec_id_from_rfc6381_codec_string(codec_string));
        if (media == "audio"sv && track_type == Media::TrackType::Video)
            return false;
        if (media == "video"sv && track_type == Media::TrackType::Audio)
            return false;
    }

    // 4. Return true.
    return true;
}

// https://w3c.github.io/media-capabilities/#valid-video-configuration
bool is_valid_video_configuration(VideoConfiguration const& configuration)
{
    // To check if a VideoConfiguration configuration is a valid video configuration, the following steps MUST be
    // run:

    // 1. If framerate is not finite or is not greater than 0, return false and abort these steps.
    if (!isfinite(configuration.framerate) || configuration.framerate <= 0)
        return false;

    // 2. If an optional member is specified for a MediaDecodingType or MediaEncodingType to which it’s not
    //    applicable, return false and abort these steps. See applicability rules in the member definitions below.
    // FIXME: Implement this.

    // 3. Let mimeType be the result of running parse a MIME type with configuration’s contentType.
    auto mime_type = MimeSniff::MimeType::parse(configuration.content_type);

    // 4. If mimeType is failure, return false.
    if (!mime_type.has_value())
        return false;

    // AD-HOC: Also return false if config's contentType is not a valid MIME-type string, such as "video/webm;". The
    //         spec’s "Parse a MIME type" accepts some strings that don't match the media-type production; but Blink
    //         instead parses the content type strictly here — and rejects a trailing semicolon.
    if (!MimeSniff::is_valid_mime_type_string(configuration.content_type))
        return false;

    // 5. Return the result of running check MIME type validity with mimeType and video.
    return check_mime_type_validity(*mime_type, "video"sv);
}

// https://w3c.github.io/media-capabilities/#valid-audio-configuration
bool is_valid_audio_configuration(AudioConfiguration const& configuration)
{
    // To check if a AudioConfiguration configuration is a valid audio configuration, the following steps MUST be
    // run:

    // 1. Let mimeType be the result of running parse a MIME type with configuration’s contentType.
    auto mime_type = MimeSniff::MimeType::parse(configuration.content_type);

    // 2. If mimeType is failure, return false.
    if (!mime_type.has_value())
        return false;

    // AD-HOC: Also return false if config's contentType isn't a valid MIME-type string, such as "audio/mpeg;". The
    //         spec’s "Parse a MIME type" accepts some strings that don't match the media-type production; but Blink
    //         instead parses the content type strictly here — and rejects a trailing semicolon.
    if (!MimeSniff::is_valid_mime_type_string(configuration.content_type))
        return false;

    // 3. Return the result of running check MIME type validity with mimeType and audio.
    return check_mime_type_validity(*mime_type, "audio"sv);
}

GC_DEFINE_ALLOCATOR(MediaCapabilities);

GC::Ref<MediaCapabilities> MediaCapabilities::create()
{
    return GC::Heap::the().allocate<MediaCapabilities>();
}

MediaCapabilities::MediaCapabilities()
{
}

// https://w3c.github.io/media-capabilities/#dom-mediacapabilities-decodinginfo
void MediaCapabilities::decoding_info(MediaDecodingConfiguration const& configuration, GC::Ref<WebIDL::Promise> promise)
{
    // The decodingInfo() method MUST run the following steps:

    // 1. If configuration is not a valid MediaDecodingConfiguration, return a Promise rejected with a newly created
    //    TypeError.
    if (!is_valid_media_decoding_configuration(configuration)) {
        WebIDL::reject_promise(promise, JS::TypeError::create(WebIDL::promise_realm(promise), "The given configuration is not a valid MediaDecodingConfiguration"sv));
        return;
    }

    // 2. If configuration.keySystemConfiguration exists, run the following substeps:
    // FIXME: Implement this.

    // 4. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(GC::Heap::the(), [promise, configuration]() mutable {
        auto& realm = WebIDL::promise_realm(promise);
        HTML::TemporaryExecutionContext context(realm);
        // 1. Run the Create a MediaCapabilitiesDecodingInfo algorithm with configuration.
        auto result = media_capabilities_decoding_info_to_object(realm.global_object(), create_a_media_capabilities_decoding_info(configuration));

        // Queue a Media Capabilities task to resolve p with its result.
        queue_a_media_capabilities_task(realm.global_object(), [promise, result] {
            auto& realm = WebIDL::promise_realm(promise);
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            WebIDL::resolve_promise(promise, JS::Value(result));
        });
    }));
}

// https://w3c.github.io/media-capabilities/#create-a-mediacapabilitiesdecodinginfo
MediaCapabilitiesDecodingInfo create_a_media_capabilities_decoding_info(MediaDecodingConfiguration configuration)
{
    // 1. Let info be a new MediaCapabilitiesDecodingInfo instance. Unless stated otherwise, reading and
    //    writing apply to info for the next steps.
    MediaCapabilitiesDecodingInfo info = {};

    // 2. Set configuration to be a new MediaDecodingConfiguration. For every property in configuration create
    //    a new property with the same name and value in configuration.
    MediaDecodingConfiguration info_configuration {};
    info_configuration.audio = configuration.audio;
    info_configuration.video = configuration.video;
    info_configuration.type = configuration.type;
    info_configuration.key_system_configuration = configuration.key_system_configuration;
    info.configuration = move(info_configuration);

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

bool is_able_to_decode_media(MediaDecodingConfiguration const& configuration)
{
    // FIXME: This only checks the MIME subtype — so a codec unrecognized by LibMedia on an otherwise supported subtype
    //        is still reported as supported.
    if (configuration.type != MediaDecodingType::MediaSource)
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

}
