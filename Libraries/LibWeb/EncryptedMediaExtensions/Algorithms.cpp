/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/EncryptedMediaExtensions/Algorithms.h>
#include <LibWeb/MimeSniff/MimeType.h>

namespace Web::EncryptedMediaExtensions {

bool supports_container([[maybe_unused]] Utf16String const& container)
{
    // FIXME: Check FFmpeg?
    return true;
}

// https://w3c.github.io/encrypted-media/#get-supported-capabilities-for-audio-video-type
Optional<Vector<Bindings::MediaKeySystemMediaCapability>> get_supported_capabilities_for_audio_video_type(KeySystem const& implementation, CapabilitiesType type, Vector<Bindings::MediaKeySystemMediaCapability> requested_capabilities, Bindings::MediaKeySystemConfiguration config, MediaKeyRestrictions restrictions)
{
    // 1. Let local accumulated configuration be a local copy of accumulated configuration.
    Bindings::MediaKeySystemConfiguration accumulated_configuration = config;

    // 2. Let supported media capabilities be an empty sequence of MediaKeySystemMediaCapability dictionaries.
    Vector<Bindings::MediaKeySystemMediaCapability> supported_media_capabilities;

    // 3. For each requested media capability in requested media capabilities:
    for (auto& capability : requested_capabilities) {
        // 1. Let content type be requested media capability's contentType member.
        auto const& content_type = capability.content_type;

        // 2. Let encryption scheme be requested media capability’s encryptionScheme member.
        auto const& encryption_scheme = capability.encryption_scheme;

        // 3. Let robustness be requested media capability's robustness member.
        auto const& robustness = capability.robustness;

        // 4. If content type is the empty string, return null.
        if (content_type.is_empty())
            return {};

        // 5. Let mimeType be the result of running parse a MIME type with content type.
        auto mime_type = MimeSniff::MimeType::parse(content_type.to_utf8());

        // 6. If mimeType is failure or is unrecognized, continue to the next iteration.
        if (!mime_type.has_value())
            continue;

        // 7. Let container be the container type specified by mimeType.
        auto const& container = Utf16String::from_utf8(mime_type->essence());

        // 8. If the user agent does not support container, continue to the next iteration.
        //    The case-sensitivity of string comparisons is determined by the appropriate RFC.
        if (!supports_container(container))
            continue;

        // 9. Let parameters be the "codecs" and "profiles" RFC 6381 [RFC6381] parameters, if any, of mimeType.
        auto parameters = mime_type->parameters();

        // FIXME: 10. If the user agent does not recognize one or more parameters, or
        //            if any parameters are not valid per the relevant specification, continue to the next iteration.

        // 11. Let media types be the set of codecs and codec constraints specified by parameters.
        //     The case-sensitivity of string comparisons is determined by the appropriate RFC or other specification.
        auto media_types = Utf16String::from_utf8(parameters.get("codecs"sv).value_or({}));

        // 12. If media types is empty:
        if (media_types.is_empty()) {
            // FIXME: If container normatively implies a specific set of codecs and codec constraints:
            if (false) {
                // Let parameters be that set.
            }
            // Otherwise:
            else {
                // Continue to the next iteration.
                continue;
            }
        }

        // 13. If mimeType is not strictly an audio/video type, continue to the next iteration.
        if (!mime_type->is_audio_or_video())
            continue;

        // 14. If encryption scheme is non-null and is not recognized or not supported by implementation,
        //     continue to the next iteration.
        if (encryption_scheme.has_value() && !implementation.supports_encryption_scheme(*encryption_scheme))
            continue;

        // 15. If robustness is not the empty string and contains an unrecognized value or a value not supported
        //     by implementation, continue to the next iteration. String comparison is case-sensitive.
        if (!robustness.is_empty() && !implementation.supports_robustness(robustness))
            continue;

        // 16. If the user agent and implementation definitely support playback of encrypted media data for
        //     the combination of container, media types, encryption scheme, robustness and
        //     local accumulated configuration in combination with restrictions:
        if (implementation.definitely_supports_playback(container, media_types, encryption_scheme, robustness, accumulated_configuration, restrictions)) {
            // 1. Add requested media capability to supported media capabilities.
            supported_media_capabilities.append(move(capability));

            // 2. If audio/video type is Video:
            if (type == CapabilitiesType::Video) {
                // Add requested media capability to the videoCapabilities member of local accumulated configuration.
                accumulated_configuration.video_capabilities.append(capability);
            }

            // If audio/video type is Audio:
            if (type == CapabilitiesType::Audio) {
                // Add requested media capability to the audioCapabilities member of local accumulated configuration.
                accumulated_configuration.audio_capabilities.append(capability);
            }
        }
    }

    // 4. If supported media capabilities is empty, return null.
    if (supported_media_capabilities.is_empty())
        return {};

    // 5. Return supported media capabilities.
    return supported_media_capabilities;
}

}
