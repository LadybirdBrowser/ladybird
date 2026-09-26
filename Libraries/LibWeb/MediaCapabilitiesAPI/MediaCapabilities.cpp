/*
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/Runtime/Object.h>
#include <LibMedia/CodecParameters.h>
#include <LibMedia/MediaSupport.h>
#include <LibMediaClient/Client.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/MediaCapabilitiesAPI/MediaCapabilities.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebIDL/Promise.h>

namespace Web::MediaCapabilitiesAPI {

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
static bool check_mime_type_validity(MimeSniff::MimeType const& mime_type, Media::TrackType media)
{
    // 1. If the type of mimeType per [RFC9110] is neither media nor application, return false.
    auto media_name = media == Media::TrackType::Audio ? "audio"sv : "video"sv;
    if (mime_type.type() != media_name && mime_type.type() != "application"sv)
        return false;

    auto container = Media::container_mime_type_from_mime_type(mime_type.type(), mime_type.subtype());
    if (!container.has_value())
        return true;

    auto implied_codec = Media::codec_implied_by_file_container(*container);

    // 2. If the combined type and subtype members of mimeType allow a single media codec and the parameters
    //    member of mimeType is not empty, return false.
    if (implied_codec.has_value() && !mime_type.parameters().is_empty())
        return false;

    // 3. If the combined type and subtype members of mimeType allow multiple media codecs, run the following
    //    steps:
    if (!implied_codec.has_value()) {
        //     1. If the parameters member of mimeType does not contain a single key named "codecs", return false.
        if (mime_type.parameters().size() != 1)
            return false;
        auto codecs = mime_type.parameters().find("codecs"sv);
        if (codecs == mime_type.parameters().end())
            return false;

        //     2. If the value of mimeType.parameters["codecs"] does not describe a single media codec, return
        //        false.
        auto codec_strings = codecs->value.bytes_as_string_view().split_view(',', SplitBehavior::KeepEmpty);
        if (codec_strings.size() != 1)
            return false;
        auto codec = Media::parse_codec_parameters_string(codec_strings[0].trim_whitespace());
        if (!codec.has_value())
            return false;

        // AD-HOC: The algorithm takes media but consults it only in step 1, so nothing rejects a codec of the
        //         wrong kind. See https://github.com/w3c/media-capabilities/issues/261.
        if (Media::track_type_from_codec_id(codec->codec_id()) != media)
            return false;
    }

    // 4. Return true.
    return true;
}

// https://w3c.github.io/media-capabilities/#valid-audio-mime-type
bool is_valid_audio_mime_type(Utf16View string)
{
    // 1. Let mimeType be the result of running parse a MIME type with configuration's contentType.
    auto mime_type = MimeSniff::MimeType::parse(string);

    // 2. If mimeType is failure, return false.
    if (!mime_type.has_value())
        return false;

    // 3. Return the result of running check MIME type validity with mimeType and audio.
    return check_mime_type_validity(*mime_type, Media::TrackType::Audio);
}

// https://w3c.github.io/media-capabilities/#valid-video-mime-type
bool is_valid_video_mime_type(Utf16View string)
{
    // 1. Let mimeType be the result of running parse a MIME type with configuration's contentType.
    auto mime_type = MimeSniff::MimeType::parse(string);

    // 2. If mimeType is failure, return false.
    if (!mime_type.has_value())
        return false;

    // 3. Return the result of running check MIME type validity with mimeType and video.
    return check_mime_type_validity(*mime_type, Media::TrackType::Video);
}

// https://w3c.github.io/media-capabilities/#valid-video-configuration
bool is_valid_video_configuration(VideoConfiguration const& configuration)
{
    // To check if a VideoConfiguration configuration is a valid video configuration, the following steps MUST be
    // run:

    // 1. If configuration’s contentType is not a valid video MIME type, return false and abort these steps.
    if (!is_valid_video_mime_type(configuration.content_type))
        return false;

    // 2. If framerate is not finite or is not greater than 0, return false and abort these steps.
    if (!isfinite(configuration.framerate) || configuration.framerate <= 0)
        return false;

    // 3. If an optional member is specified for a MediaDecodingType or MediaEncodingType to which it’s not
    //    applicable, return false and abort these steps. See applicability rules in the member definitions below.
    // FIXME: Implement this.

    // 4. Return true.
    return true;
}

// https://w3c.github.io/media-capabilities/#valid-video-configuration
bool is_valid_audio_configuration(AudioConfiguration const& configuration)
{
    // To check if a AudioConfiguration configuration is a valid audio configuration, the following steps MUST be
    // run:

    // 1. If configuration’s contentType is not a valid audio MIME type, return false and abort these steps.
    if (!is_valid_audio_mime_type(configuration.content_type))
        return false;

    // 2. Return true.
    return true;
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
    auto resolve_with_capabilities = [promise = GC::Root(promise), configuration](Optional<Media::DecoderCapabilities> const& capabilities) {
        // 1. Run the Create a MediaCapabilitiesDecodingInfo algorithm with configuration.
        // 2. Queue a Media Capabilities task to resolve p with its result.
        queue_a_media_capabilities_task(WebIDL::promise_realm(*promise).global_object(), [promise, configuration, capabilities] {
            auto& realm = WebIDL::promise_realm(*promise);
            HTML::TemporaryExecutionContext context(realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes);
            auto result = Bindings::media_capabilities_decoding_info_to_value(realm, create_a_media_capabilities_decoding_info(configuration, capabilities));
            WebIDL::resolve_promise(*promise, result);
        });
    };
    request_media_decoding_capabilities(configuration)
        ->when_resolved([resolve_with_capabilities](Optional<Media::DecoderCapabilities>& capabilities) {
            resolve_with_capabilities(capabilities);
        })
        .when_rejected([resolve_with_capabilities](Error&) {
            resolve_with_capabilities({});
        });
}

// https://w3c.github.io/media-capabilities/#create-media-capabilities-decoding-info
MediaCapabilitiesDecodingInfo create_a_media_capabilities_decoding_info(MediaDecodingConfiguration const& configuration, Optional<Media::DecoderCapabilities> const& capabilities)
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

    // NB: Steps 3. and 4. run in request_media_decoding_capabilities().

    // 5. Set supported to true.
    // NB: Step 4's substep 6 sets it to false instead when a content type is unsupported, which an empty result
    //     stands for.
    info.supported = capabilities.has_value();

    // 6. If the user agent is able to decode the media represented by configuration at the indicated framerate without
    //    dropping frames, set smooth to true. Otherwise set it to false.
    info.smooth = capabilities.has_value() && capabilities->smooth;

    // 7. If the user agent is able to decode the media represented by configuration in a power efficient manner, set
    //    powerEfficient to true. Otherwise set it to false.
    info.power_efficient = capabilities.has_value() && capabilities->power_efficient;

    // 8. Return info.
    return info;
}

static NonnullRefPtr<DecoderCapabilitiesPromise> unsupported()
{
    return DecoderCapabilitiesPromise::resolved(Optional<Media::DecoderCapabilities> {});
}

static NonnullRefPtr<DecoderCapabilitiesPromise> request_file_decoding_capabilities(MimeSniff::MimeType const& mime_type)
{
    auto media_client = MediaClient::Client::acquire();
    if (media_client.is_error())
        return unsupported();
    return media_client.value()->request_file_media_support(mime_type.type(), mime_type.subtype(), mime_type.parameters().get("codecs"sv))->map<Optional<Media::DecoderCapabilities>>([](Media::MediaSupportInfo& support) -> Optional<Media::DecoderCapabilities> {
        // AD-HOC: An inexact answer is treated as unsupported, as Chromium does.
        if (support.support != Media::MediaSupport::Probably)
            return {};
        return support.capabilities;
    });
}

static NonnullRefPtr<DecoderCapabilitiesPromise> request_media_source_decoding_capabilities(MimeSniff::MimeType const& mime_type)
{
    if (!MediaSourceExtensions::MediaSource::mime_type_is_supported_in_a_byte_stream(mime_type))
        return unsupported();

    auto media_client = MediaClient::Client::acquire();
    if (media_client.is_error())
        return unsupported();
    return media_client.value()->request_decoder_capabilities(mime_type.parameters().get("codecs"sv)->bytes_as_string_view());
}

// https://w3c.github.io/media-capabilities/#check-mime-type-support
// AD-HOC: The spec answers only supported or unsupported, leaving steps 6 and 7 of "Create a
//         MediaCapabilitiesDecodingInfo" to determine smoothness and power efficiency by unspecified means.
//         LibMedia answers all three from one query, so the capabilities are returned here instead.
static NonnullRefPtr<DecoderCapabilitiesPromise> check_mime_type_support(Utf16View content_type, MediaDecodingType decoding_type)
{
    // 1. If encodingOrDecodingType is webrtc and mimeType is not one that is used with RTP [...], return
    //    unsupported.
    // FIXME: No RTP payload formats are supported, so every webrtc configuration is rejected here.
    if (decoding_type == MediaDecodingType::Webrtc)
        return unsupported();

    // FIXME: 2. If colorGamut is present and is not valid for mimeType, return unsupported.
    // FIXME: 3. If transferFunction is present and is not valid for mimeType, return unsupported.

    // 4. If mimeType is not supported by the user agent, return unsupported.
    // AD-HOC: A media-source content type is supported exactly when MediaSource.isTypeSupported() accepts it,
    //         and a file content type when canPlayType() answers "probably" for it.
    // 5. Return supported.
    auto mime_type = MimeSniff::MimeType::parse(content_type);
    if (!mime_type.has_value())
        return unsupported();
    if (decoding_type == MediaDecodingType::MediaSource)
        return request_media_source_decoding_capabilities(*mime_type);
    return request_file_decoding_capabilities(*mime_type);
}

static NonnullRefPtr<DecoderCapabilitiesPromise> combine_capability_answers(NonnullRefPtr<DecoderCapabilitiesPromise> const& video_capabilities, NonnullRefPtr<DecoderCapabilitiesPromise> const& audio_capabilities)
{
    struct Answers : public RefCounted<Answers> {
        Optional<Optional<Media::DecoderCapabilities>> video;
        Optional<Optional<Media::DecoderCapabilities>> audio;
    };
    auto answers = make_ref_counted<Answers>();
    auto result = DecoderCapabilitiesPromise::construct();

    auto resolve_if_complete = [answers, result] {
        if (!answers->video.has_value() || !answers->audio.has_value())
            return;
        auto const& video = *answers->video;
        auto const& audio = *answers->audio;
        if (!video.has_value() || !audio.has_value()) {
            result->resolve(Optional<Media::DecoderCapabilities> {});
            return;
        }
        result->resolve(Optional<Media::DecoderCapabilities> { Media::DecoderCapabilities {
            .smooth = video->smooth && audio->smooth,
            .power_efficient = video->power_efficient && audio->power_efficient,
        } });
    };
    video_capabilities
        ->when_resolved([answers, resolve_if_complete](Optional<Media::DecoderCapabilities>& capabilities) {
            answers->video = capabilities;
            resolve_if_complete();
        })
        .when_rejected([answers, resolve_if_complete](Error&) {
            answers->video = Optional<Media::DecoderCapabilities> {};
            resolve_if_complete();
        });
    audio_capabilities
        ->when_resolved([answers, resolve_if_complete](Optional<Media::DecoderCapabilities>& capabilities) {
            answers->audio = capabilities;
            resolve_if_complete();
        })
        .when_rejected([answers, resolve_if_complete](Error&) {
            answers->audio = Optional<Media::DecoderCapabilities> {};
            resolve_if_complete();
        });
    return result;
}

// https://w3c.github.io/media-capabilities/#create-a-mediacapabilitiesdecodinginfo
NonnullRefPtr<DecoderCapabilitiesPromise> request_media_decoding_capabilities(MediaDecodingConfiguration const& configuration)
{
    // FIXME: 3. If configuration.keySystemConfiguration exists:
    if (false) {
        // 1. Set keySystemAccess to the result of running the Check Encrypted Decoding Support algorithm with
        //    configuration.
        // 2. If keySystemAccess is null, set supported to false, smooth to false, powerEfficient to false, and return
        //    info.
        // 3. Otherwise, set supported to true and continue with step 6.
    }
    // Otherwise, run the following steps:
    else {
        // NB: An absent content type places no constraint, which the all-true capabilities stand for below.
        auto unconstrained = [] { return DecoderCapabilitiesPromise::resolved(Optional<Media::DecoderCapabilities> { Media::DecoderCapabilities { .smooth = true, .power_efficient = true } }); };

        // 2. Let videoSupported be unknown.
        auto video_capabilities = unconstrained();

        // 3. If video is present in configuration, [...] set videoSupported to the result of running check MIME
        //   type support with videoMimeType, configuration's type, colorGamut, and transferFunction.
        if (configuration.video.has_value())
            video_capabilities = check_mime_type_support(configuration.video->content_type, configuration.type);

        // 4. Let audioSupported be unknown.
        auto audio_capabilities = unconstrained();

        // 5. If audio is present in configuration, [...] set audioSupported to the result of running check MIME type
        //    support with audioMimeType and configuration's type.
        if (configuration.audio.has_value())
            audio_capabilities = check_mime_type_support(configuration.audio->content_type, configuration.type);

        // 6. If videoSupported or audioSupported is unsupported, set supported to false, smooth to false,
        //    powerEfficient to false, and return info.
        return combine_capability_answers(video_capabilities, audio_capabilities);
    }
}

}
