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
    Bindings::MediaKeySystemConfiguration accumulated_configuration = move(config);

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

// https://w3c.github.io/encrypted-media/#dfn-is-persistent-session-type
bool is_persistent_session_type(Utf16String const& session_type)
{
    // 1. Let the session type be the specified MediaKeySessionType value.

    // 2. Follow the steps for the value of session type from the following list:

    // * "temporary"
    if (session_type == "temporary"sv) {
        // Return false.
        return false;
    }

    // * "persistent-license"
    if (session_type == "persistent-license"sv) {
        // Return true.
        return true;
    }

    VERIFY_NOT_REACHED();
}

// https://w3c.github.io/encrypted-media/#get-consent-status
ConsentStatus get_consent_status(Bindings::MediaKeySystemConfiguration const& accumulated_configuration, MediaKeyRestrictions& restrictions, URL::Origin const& origin)
{
    // FIXME: Implement this
    (void)accumulated_configuration;
    (void)restrictions;
    (void)origin;

    dbgln("get_consent_status: Not implemented, returning Allowed by default");

    return ConsentStatus::Allowed;
}

// https://w3c.github.io/encrypted-media/#get-supported-configuration-and-consent
Optional<ConsentConfiguration> get_supported_configuration_and_consent(KeySystem const& implementation, Bindings::MediaKeySystemConfiguration const& candidate_configuration, MediaKeyRestrictions& restrictions, URL::Origin const& origin)
{
    // 1. Let accumulated configuration be a new MediaKeySystemConfiguration dictionary.
    Bindings::MediaKeySystemConfiguration accumulated_configuration;

    // 2. Set the label member of accumulated configuration to equal the label member of candidate configuration.
    accumulated_configuration.label = candidate_configuration.label;

    // 3. If the initDataTypes member of candidate configuration is non-empty, run the following steps:
    if (!candidate_configuration.init_data_types.is_empty()) {
        // 1. Let supported types be an empty sequence of DOMStrings.
        Vector<Utf16String> supported_types;

        // 2. For each value in candidate configuration's initDataTypes member:
        for (auto const& init_data_type : candidate_configuration.init_data_types) {
            // 1. Let initDataType be the value.

            // 2. If the implementation supports generating requests based on initDataType, add initDataType to supported types.
            // String comparison is case-sensitive. The empty string is never supported.
            if (implementation.supports_init_data_type(init_data_type))
                supported_types.append(init_data_type);
        }

        // 3. If supported types is empty, return NotSupported.
        if (supported_types.is_empty())
            return {};

        // 4. Set the initDataTypes member of accumulated configuration to supported types.
        accumulated_configuration.init_data_types = move(supported_types);
    }

    // 4. Let distinctive identifier requirement be the value of candidate configuration's distinctiveIdentifier member.
    auto distinctive_identifier_requirement = candidate_configuration.distinctive_identifier;

    // 5. If distinctive identifier requirement is "optional" and Distinctive Identifiers are not allowed according to restrictions,
    //    set distinctive identifier requirement to "not-allowed".
    if (distinctive_identifier_requirement == Bindings::MediaKeysRequirement::Optional && !restrictions.distinctive_identifiers)
        distinctive_identifier_requirement = Bindings::MediaKeysRequirement::NotAllowed;

    // 6. Follow the steps for distinctive identifier requirement from the following list:
    switch (distinctive_identifier_requirement) {
    case Bindings::MediaKeysRequirement::Required:
        // FIXME: If the implementation does not support use of Distinctive Identifier(s) in combination
        //        with accumulated configuration and restrictions, return NotSupported.
        break;
    case Bindings::MediaKeysRequirement::Optional:
        // Continue with the following steps.
        break;
    case Bindings::MediaKeysRequirement::NotAllowed:
        // FIXME: If the implementation requires use of Distinctive Identifier(s) or Distinctive Permanent Identifier(s)
        //        in combination with accumulated configuration and restrictions, return NotSupported.
        break;
    }

    // 7. Set the distinctiveIdentifier member of accumulated configuration to equal distinctive identifier requirement.
    accumulated_configuration.distinctive_identifier = distinctive_identifier_requirement;

    // 8. Let persistent state requirement be equal to the value of candidate configuration's persistentState member.
    auto persistent_state_requirement = candidate_configuration.persistent_state;

    // 9. If persistent state requirement is "optional" and persisting state is not allowed according to restrictions,
    //    set persistent state requirement to "not-allowed".
    if (persistent_state_requirement == Bindings::MediaKeysRequirement::Optional && !restrictions.persist_state)
        persistent_state_requirement = Bindings::MediaKeysRequirement::NotAllowed;

    // 10. Follow the steps for persistent state requirement from the following list:
    switch (persistent_state_requirement) {
    case Bindings::MediaKeysRequirement::Required:
        // FIXME: If the implementation does not support persisting state in combination with accumulated configuration
        //        and restrictions, return NotSupported.
        break;
    case Bindings::MediaKeysRequirement::Optional:
        // Continue with the following steps.
        break;
    case Bindings::MediaKeysRequirement::NotAllowed:
        // FIXME: If the implementation requires persisting state in combination with accumulated configuration
        //        and restrictions, return NotSupported.
        break;
    }

    // 12. Set the persistentState member of accumulated configuration to equal the value of persistent state requirement.
    accumulated_configuration.persistent_state = persistent_state_requirement;

    Vector<Utf16String> session_types;
    // 1. Follow the steps for the first matching condition from the following list:

    // * If the sessionTypes member is present in candidate configuration
    if (candidate_configuration.session_types.has_value()) {
        // Let session types be candidate configuration's sessionTypes member.
        session_types = *candidate_configuration.session_types;
    }
    // * Otherwise
    else {
        // Let session types be [ "temporary" ].
        session_types.append("temporary"_utf16);
    }

    // 13. For each value in session types:
    for (auto const& session_type : session_types) {
        // 1. Let session type be the value.

        // 2. If accumulated configuration's persistentState value is "not-allowed" and the Is persistent session type? algorithm
        //    returns true for session type return NotSupported.
        if (accumulated_configuration.persistent_state == Bindings::MediaKeysRequirement::NotAllowed && is_persistent_session_type(session_type))
            return {};

        // 3. FIXME: If the implementation does not support session type in combination with accumulated configuration and restrictions for other reasons, return NotSupported.

        // 4. If accumulated configuration's persistentState value is "optional" and the result of running the Is persistent session type? algorithm
        //    on session type is true, change accumulated configuration's persistentState value to "required".
        if (accumulated_configuration.persistent_state == Bindings::MediaKeysRequirement::Optional && is_persistent_session_type(session_type))
            accumulated_configuration.persistent_state = Bindings::MediaKeysRequirement::Required;
    }

    // 14. Set the sessionTypes member of accumulated configuration to session types.
    accumulated_configuration.session_types = move(session_types);

    // 15. If the videoCapabilities and audioCapabilities members in candidate configuration are both empty, return NotSupported.
    if (candidate_configuration.video_capabilities.is_empty() && candidate_configuration.audio_capabilities.is_empty())
        return {};

    // 16. If the videoCapabilities member in candidate configuration is non-empty:
    if (!candidate_configuration.video_capabilities.is_empty()) {
        // 1. Let video capabilities be the result of executing the Get Supported Capabilities for Audio/Video Type algorithm
        //    on Video, candidate configuration's videoCapabilities member, accumulated configuration, and restrictions.
        auto video_capabilities = get_supported_capabilities_for_audio_video_type(implementation, CapabilitiesType::Video, candidate_configuration.video_capabilities, accumulated_configuration, restrictions);

        // 2. If video capabilities is null, return NotSupported.
        if (!video_capabilities.has_value())
            return {};

        // 3. Set the videoCapabilities member of accumulated configuration to video capabilities.
        accumulated_configuration.video_capabilities = *video_capabilities;
    }
    // Otherwise:
    else {
        // 1. Set the videoCapabilities member of accumulated configuration to an empty sequence.
        accumulated_configuration.video_capabilities = Vector<Bindings::MediaKeySystemMediaCapability> {};
    }

    // 1. If the audioCapabilities member in candidate configuration is non-empty:
    if (!candidate_configuration.audio_capabilities.is_empty()) {
        // 1. Let audio capabilities be the result of executing the Get Supported Capabilities for Audio/Video Type algorithm
        //    on Audio, candidate configuration's audioCapabilities member, accumulated configuration, and restrictions.
        auto audio_capabilities = get_supported_capabilities_for_audio_video_type(implementation, CapabilitiesType::Audio, candidate_configuration.audio_capabilities, accumulated_configuration, restrictions);

        // 2. If audio capabilities is null, return NotSupported.
        if (!audio_capabilities.has_value())
            return {};

        // 3. Set the audioCapabilities member of accumulated configuration to audio capabilities.
        accumulated_configuration.audio_capabilities = *audio_capabilities;
    }
    // Otherwise:
    else {
        // 1. Set the audioCapabilities member of accumulated configuration to an empty sequence.
        accumulated_configuration.audio_capabilities = Vector<Bindings::MediaKeySystemMediaCapability> {};
    }

    // 18. If accumulated configuration's distinctiveIdentifier value is "optional", follow the steps for the first matching condition from the following list:
    if (accumulated_configuration.distinctive_identifier == Bindings::MediaKeysRequirement::Optional) {
        // FIXME: 1. If the implementation requires use of Distinctive Identifier(s) or Distinctive Permanent Identifier(s) for any of the combinations in accumulated configuration:
        if (false) {
            // 1. Change accumulated configuration's distinctiveIdentifier value to "required".
            accumulated_configuration.distinctive_identifier = Bindings::MediaKeysRequirement::Required;
        }
        // Otherwise
        else {
            // 1. Change accumulated configuration's distinctiveIdentifier value to "not-allowed".
            accumulated_configuration.distinctive_identifier = Bindings::MediaKeysRequirement::NotAllowed;
        }
    }

    // 19. If accumulated configuration's persistentState value is "optional", follow the steps for the first matching condition from the following list:
    if (accumulated_configuration.persistent_state == Bindings::MediaKeysRequirement::Optional) {
        // FIXME: 1. If the implementation requires persisting state for any of the combinations in accumulated configuration
        if (false) {
            // 1. Change accumulated configuration's persistentState value to "required".
            accumulated_configuration.persistent_state = Bindings::MediaKeysRequirement::Required;
        }
        // Otherwise
        else {
            // 1. Change accumulated configuration's persistentState value to "not-allowed".
            accumulated_configuration.persistent_state = Bindings::MediaKeysRequirement::NotAllowed;
        }
    }

    // FIXME: 20. If implementation in the configuration specified by the combination of the values in accumulated configuration
    //            is not supported or not allowed in the origin, return NotSupported.
    // FIXME: 21. If accumulated configuration's distinctiveIdentifier value is "required" and the Distinctive Identifier(s)
    //            associated with accumulated configuration are not unique per origin and profile and clearable:
    // FIXME: 1. Update restrictions to reflect that all configurations described by accumulated configuration do not have user consent.
    // FIXME: 2. Return ConsentDenied and restrictions.

    // 22. Let consent status and updated restrictions be the result of running the Get Consent Status algorithm
    //     on accumulated configuration, restrictions and origin and follow the steps for the value of consent status from the following list:
    auto consent_status = get_consent_status(accumulated_configuration, restrictions, origin);
    switch (consent_status) {
    case ConsentStatus::ConsentDenied:
        // Return ConsentDenied and updated restrictions.
        return {};
    case ConsentStatus::InformUser:
        // FIXME: Inform the user that accumulated configuration is in use in the origin including, specifically,
        //        the information that Distinctive Identifier(s) and/or Distinctive Permanent Identifier(s) as
        //        appropriate will be used if the distinctiveIdentifier member of accumulated configuration is "required".
        //        Continue to the next step.
        break;
    case ConsentStatus::Allowed:
        // Continue to the next step.
        break;
    }

    // 23. Return accumulated configuration.
    return ConsentConfiguration { consent_status, accumulated_configuration };
}

// https://w3c.github.io/encrypted-media/#get-supported-configuration
Optional<ConsentConfiguration> get_supported_configuration(KeySystem const& implementation, Bindings::MediaKeySystemConfiguration const& candidate_configuration, URL::Origin const& origin)
{
    // 1. Let supported configuration be ConsentDenied.
    Optional<ConsentConfiguration> supported_configuration = ConsentConfiguration { ConsentStatus::ConsentDenied, {} };

    // 2. Initialize restrictions to indicate that no configurations have had user consent denied.
    MediaKeyRestrictions restrictions;

    size_t loop_count = 0;

    // 3. Repeat the following step while supported configuration is ConsentDenied:
    while (supported_configuration.has_value() && supported_configuration->status == ConsentStatus::ConsentDenied) {
        // 1. Let supported configuration and, if provided, restrictions be the result of executing
        //    the Get Supported Configuration and Consent algorithm with implementation, candidate configuration, restrictions and origin.
        supported_configuration = get_supported_configuration_and_consent(implementation, candidate_configuration, restrictions, origin);

        // AD-HOC: While this is being implemented, we use this to avoid a possible infinite loop
        if (loop_count++ > 5) {
            break;
        }
    }

    // 4. Return supported configuration.
    return supported_configuration;
}

// https://w3c.github.io/encrypted-media/#dfn-common-key-systems
bool is_supported_key_system(Utf16String const& key_system)
{
    constexpr Array<Utf16View, 1> supported_key_systems = {
        // https://w3c.github.io/encrypted-media/#clear-key
        "org.w3.clearkey"sv,
    };

    return supported_key_systems.contains_slow(key_system);
}

NonnullOwnPtr<KeySystem> key_system_from_string(Utf16String const& key_system)
{
    if (key_system == "org.w3.clearkey"_utf16) {
        return adopt_own(*new ClearKeySystem());
    }

    VERIFY_NOT_REACHED();
}

}
