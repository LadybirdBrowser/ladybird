/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use liburl_rust::origin::Origin;
use libwebcommon_rust::mime_sniff::mime_type::MimeType;

use super::encrypted_media_extensions::{CapabilitiesType, ConsentConfiguration, ConsentStatus, MediaKeyRestrictions};
use super::key_system::{ClearKeySystem, KeySystem};
use super::media_key_system_access::{
    MediaKeySessionType, MediaKeySystemConfiguration, MediaKeySystemMediaCapability, MediaKeysRequirement,
};

pub(crate) fn supports_container(_container: &str) -> bool {
    // FIXME: Check FFmpeg?
    true
}

// https://w3c.github.io/encrypted-media/#get-supported-capabilities-for-audio-video-type
pub(crate) fn get_supported_capabilities_for_audio_video_type(
    implementation: &dyn KeySystem,
    r#type: CapabilitiesType,
    requested_capabilities: Vec<MediaKeySystemMediaCapability>,
    config: MediaKeySystemConfiguration,
    restrictions: &MediaKeyRestrictions,
) -> Option<Vec<MediaKeySystemMediaCapability>> {
    // 1. Let local accumulated configuration be a local copy of accumulated configuration.
    let mut accumulated_configuration = config;

    // 2. Let supported media capabilities be an empty sequence of MediaKeySystemMediaCapability dictionaries.
    let mut supported_media_capabilities = Vec::new();

    // 3. For each requested media capability in requested media capabilities:
    for capability in requested_capabilities {
        // 1. Let content type be requested media capability's contentType member.
        let content_type = &capability.content_type;

        // 2. Let encryption scheme be requested media capability’s encryptionScheme member.
        let encryption_scheme = capability.encryption_scheme.as_ref();

        // 3. Let robustness be requested media capability's robustness member.
        let robustness = &capability.robustness;

        // 4. If content type is the empty string, return null.
        if content_type.is_empty() {
            return None;
        }

        // 5. Let mimeType be the result of running parse a MIME type with content type.
        let content_type = char::decode_utf16(content_type.to_utf16().iter().copied())
            .map(|code_point| code_point.unwrap_or(char::REPLACEMENT_CHARACTER))
            .collect::<Vec<_>>();
        let mime_type = MimeType::parse(&content_type);

        // 6. If mimeType is failure or is unrecognized, continue to the next iteration.
        let Some(mime_type) = mime_type else {
            continue;
        };

        // 7. Let container be the container type specified by mimeType.
        let essence = mime_type.essence();
        let container = format!("{}/{}", essence.type_, essence.subtype);

        // 8. If the user agent does not support container, continue to the next iteration.
        //    The case-sensitivity of string comparisons is determined by the appropriate RFC.
        if !supports_container(&container) {
            continue;
        }

        // 9. Let parameters be the "codecs" and "profiles" RFC 6381 [RFC6381] parameters, if any, of mimeType.
        let mut parameters = mime_type.parameters();

        // FIXME: 10. If the user agent does not recognize one or more parameters, or
        //            if any parameters are not valid per the relevant specification, continue to the next iteration.

        // 11. Let media types be the set of codecs and codec constraints specified by parameters.
        //     The case-sensitivity of string comparisons is determined by the appropriate RFC or other specification.
        let media_types = parameters
            .find(|(name, _)| *name == "codecs")
            .map_or("", |(_, value)| value);

        // 12. If media types is empty:
        if media_types.is_empty() {
            // FIXME: If container normatively implies a specific set of codecs and codec constraints:
            if false {
                // Let parameters be that set.
            }
            // Otherwise:
            else {
                // Continue to the next iteration.
                continue;
            }
        }

        // 13. If mimeType is not strictly an audio/video type, continue to the next iteration.
        if !essence.is_audio_or_video() {
            continue;
        }

        // 14. If encryption scheme is non-null and is not recognized or not supported by implementation,
        //     continue to the next iteration.
        if encryption_scheme
            .is_some_and(|encryption_scheme| !implementation.supports_encryption_scheme(encryption_scheme))
        {
            continue;
        }

        // 15. If robustness is not the empty string and contains an unrecognized value or a value not supported
        //     by implementation, continue to the next iteration. String comparison is case-sensitive.
        if !robustness.is_empty() && !implementation.supports_robustness(robustness) {
            continue;
        }

        // 16. If the user agent and implementation definitely support playback of encrypted media data for
        //     the combination of container, media types, encryption scheme, robustness and
        //     local accumulated configuration in combination with restrictions:
        if implementation.definitely_supports_playback(
            &container,
            media_types,
            encryption_scheme,
            robustness,
            &accumulated_configuration,
            restrictions,
        ) {
            // 1. Add requested media capability to supported media capabilities.
            supported_media_capabilities.push(capability.clone());

            // 2. If audio/video type is Video:
            if r#type == CapabilitiesType::Video {
                // Add requested media capability to the videoCapabilities member of local accumulated configuration.
                accumulated_configuration.video_capabilities.push(capability.clone());
            }

            // If audio/video type is Audio:
            if r#type == CapabilitiesType::Audio {
                // Add requested media capability to the audioCapabilities member of local accumulated configuration.
                accumulated_configuration.audio_capabilities.push(capability.clone());
            }
        }
    }

    // 4. If supported media capabilities is empty, return null.
    if supported_media_capabilities.is_empty() {
        return None;
    }

    // 5. Return supported media capabilities.
    Some(supported_media_capabilities)
}

// https://w3c.github.io/encrypted-media/#dfn-is-persistent-session-type
pub(crate) fn is_persistent_session_type(session_type: MediaKeySessionType) -> bool {
    // 1. Let the session type be the specified MediaKeySessionType value.

    // 2. Follow the steps for the value of session type from the following list:
    match session_type {
        // * "temporary"
        MediaKeySessionType::Temporary => {
            // Return false.
            false
        }
        // * "persistent-license"
        MediaKeySessionType::PersistentLicense => {
            // Return true.
            true
        }
    }
}

// https://w3c.github.io/encrypted-media/#get-consent-status
pub(crate) fn get_consent_status(
    _accumulated_configuration: &MediaKeySystemConfiguration,
    _restrictions: &mut MediaKeyRestrictions,
    _origin: &Origin,
) -> ConsentStatus {
    // FIXME: Implement this
    eprintln!("get_consent_status: Not implemented, returning Allowed by default");

    ConsentStatus::Allowed
}

// https://w3c.github.io/encrypted-media/#get-supported-configuration-and-consent
pub(crate) fn get_supported_configuration_and_consent(
    implementation: &dyn KeySystem,
    candidate_configuration: &MediaKeySystemConfiguration,
    restrictions: &mut MediaKeyRestrictions,
    origin: &Origin,
) -> Option<ConsentConfiguration> {
    // 1. Let accumulated configuration be a new MediaKeySystemConfiguration dictionary.
    // 2. Set the label member of accumulated configuration to equal the label member of candidate configuration.
    let mut accumulated_configuration = MediaKeySystemConfiguration {
        label: candidate_configuration.label.clone(),
        ..Default::default()
    };

    // 3. If the initDataTypes member of candidate configuration is non-empty, run the following steps:
    if !candidate_configuration.init_data_types.is_empty() {
        // 1. Let supported types be an empty sequence of DOMStrings.
        let mut supported_types = Vec::new();

        // 2. For each value in candidate configuration's initDataTypes member:
        for init_data_type in &candidate_configuration.init_data_types {
            // 1. Let initDataType be the value.

            // 2. If the implementation supports generating requests based on initDataType, add initDataType to supported types.
            // String comparison is case-sensitive. The empty string is never supported.
            if implementation.supports_init_data_type(init_data_type) {
                supported_types.push(init_data_type.clone());
            }
        }

        // 3. If supported types is empty, return NotSupported.
        if supported_types.is_empty() {
            return None;
        }

        // 4. Set the initDataTypes member of accumulated configuration to supported types.
        accumulated_configuration.init_data_types = supported_types;
    }

    // 4. Let distinctive identifier requirement be the value of candidate configuration's distinctiveIdentifier member.
    let mut distinctive_identifier_requirement = candidate_configuration.distinctive_identifier;

    // 5. If distinctive identifier requirement is "optional" and Distinctive Identifiers are not allowed according to restrictions,
    //    set distinctive identifier requirement to "not-allowed".
    if distinctive_identifier_requirement == MediaKeysRequirement::Optional && !restrictions.distinctive_identifiers {
        distinctive_identifier_requirement = MediaKeysRequirement::NotAllowed;
    }

    // 6. Follow the steps for distinctive identifier requirement from the following list:
    match distinctive_identifier_requirement {
        MediaKeysRequirement::Required => {
            // FIXME: If the implementation does not support use of Distinctive Identifier(s) in combination
            //        with accumulated configuration and restrictions, return NotSupported.
        }
        MediaKeysRequirement::Optional => {
            // Continue with the following steps.
        }
        MediaKeysRequirement::NotAllowed => {
            // FIXME: If the implementation requires use of Distinctive Identifier(s) or Distinctive Permanent Identifier(s)
            //        in combination with accumulated configuration and restrictions, return NotSupported.
        }
    }

    // 7. Set the distinctiveIdentifier member of accumulated configuration to equal distinctive identifier requirement.
    accumulated_configuration.distinctive_identifier = distinctive_identifier_requirement;

    // 8. Let persistent state requirement be equal to the value of candidate configuration's persistentState member.
    let mut persistent_state_requirement = candidate_configuration.persistent_state;

    // 9. If persistent state requirement is "optional" and persisting state is not allowed according to restrictions,
    //    set persistent state requirement to "not-allowed".
    if persistent_state_requirement == MediaKeysRequirement::Optional && !restrictions.persist_state {
        persistent_state_requirement = MediaKeysRequirement::NotAllowed;
    }

    // 10. Follow the steps for persistent state requirement from the following list:
    match persistent_state_requirement {
        MediaKeysRequirement::Required => {
            // FIXME: If the implementation does not support persisting state in combination with accumulated configuration
            //        and restrictions, return NotSupported.
        }
        MediaKeysRequirement::Optional => {
            // Continue with the following steps.
        }
        MediaKeysRequirement::NotAllowed => {
            // FIXME: If the implementation requires persisting state in combination with accumulated configuration
            //        and restrictions, return NotSupported.
        }
    }

    // 12. Set the persistentState member of accumulated configuration to equal the value of persistent state requirement.
    accumulated_configuration.persistent_state = persistent_state_requirement;

    // 1. Follow the steps for the first matching condition from the following list:
    let session_types = match &candidate_configuration.session_types {
        // * If the sessionTypes member is present in candidate configuration
        Some(session_types) => {
            // Let session types be candidate configuration's sessionTypes member.
            session_types.clone()
        }
        // * Otherwise
        None => {
            // Let session types be [ "temporary" ].
            vec![Utf16String::from_ascii(b"temporary")]
        }
    };

    // 13. For each value in session types:
    for session_type in &session_types {
        // 1. Let session type be the value.
        // AD-HOC: sessionTypes is a sequence of DOMString, but Is persistent session type? takes a MediaKeySessionType
        //         value. A session type that is not one is not supported, so the candidate configuration is not either.
        //         See https://github.com/w3c/encrypted-media/issues/447
        let session_type = MediaKeySessionType::from_string(session_type)?;

        // 2. If accumulated configuration's persistentState value is "not-allowed" and the Is persistent session type? algorithm
        //    returns true for session type return NotSupported.
        if accumulated_configuration.persistent_state == MediaKeysRequirement::NotAllowed
            && is_persistent_session_type(session_type)
        {
            return None;
        }

        // 3. FIXME: If the implementation does not support session type in combination with accumulated configuration and restrictions for other reasons, return NotSupported.

        // 4. If accumulated configuration's persistentState value is "optional" and the result of running the Is persistent session type? algorithm
        //    on session type is true, change accumulated configuration's persistentState value to "required".
        if accumulated_configuration.persistent_state == MediaKeysRequirement::Optional
            && is_persistent_session_type(session_type)
        {
            accumulated_configuration.persistent_state = MediaKeysRequirement::Required;
        }
    }

    // 14. Set the sessionTypes member of accumulated configuration to session types.
    accumulated_configuration.session_types = Some(session_types);

    // 15. If the videoCapabilities and audioCapabilities members in candidate configuration are both empty, return NotSupported.
    if candidate_configuration.video_capabilities.is_empty() && candidate_configuration.audio_capabilities.is_empty() {
        return None;
    }

    // 16. If the videoCapabilities member in candidate configuration is non-empty:
    if !candidate_configuration.video_capabilities.is_empty() {
        // 1. Let video capabilities be the result of executing the Get Supported Capabilities for Audio/Video Type algorithm
        //    on Video, candidate configuration's videoCapabilities member, accumulated configuration, and restrictions.
        let video_capabilities = get_supported_capabilities_for_audio_video_type(
            implementation,
            CapabilitiesType::Video,
            candidate_configuration.video_capabilities.clone(),
            accumulated_configuration.clone(),
            restrictions,
        );

        // 2. If video capabilities is null, return NotSupported.
        let video_capabilities = video_capabilities?;

        // 3. Set the videoCapabilities member of accumulated configuration to video capabilities.
        accumulated_configuration.video_capabilities = video_capabilities;
    }
    // Otherwise:
    else {
        // 1. Set the videoCapabilities member of accumulated configuration to an empty sequence.
        accumulated_configuration.video_capabilities = Vec::new();
    }

    // 1. If the audioCapabilities member in candidate configuration is non-empty:
    if !candidate_configuration.audio_capabilities.is_empty() {
        // 1. Let audio capabilities be the result of executing the Get Supported Capabilities for Audio/Video Type algorithm
        //    on Audio, candidate configuration's audioCapabilities member, accumulated configuration, and restrictions.
        let audio_capabilities = get_supported_capabilities_for_audio_video_type(
            implementation,
            CapabilitiesType::Audio,
            candidate_configuration.audio_capabilities.clone(),
            accumulated_configuration.clone(),
            restrictions,
        );

        // 2. If audio capabilities is null, return NotSupported.
        let audio_capabilities = audio_capabilities?;

        // 3. Set the audioCapabilities member of accumulated configuration to audio capabilities.
        accumulated_configuration.audio_capabilities = audio_capabilities;
    }
    // Otherwise:
    else {
        // 1. Set the audioCapabilities member of accumulated configuration to an empty sequence.
        accumulated_configuration.audio_capabilities = Vec::new();
    }

    // 18. If accumulated configuration's distinctiveIdentifier value is "optional", follow the steps for the first matching condition from the following list:
    if accumulated_configuration.distinctive_identifier == MediaKeysRequirement::Optional {
        // FIXME: 1. If the implementation requires use of Distinctive Identifier(s) or Distinctive Permanent Identifier(s) for any of the combinations in accumulated configuration:
        if false {
            // 1. Change accumulated configuration's distinctiveIdentifier value to "required".
            accumulated_configuration.distinctive_identifier = MediaKeysRequirement::Required;
        }
        // Otherwise
        else {
            // 1. Change accumulated configuration's distinctiveIdentifier value to "not-allowed".
            accumulated_configuration.distinctive_identifier = MediaKeysRequirement::NotAllowed;
        }
    }

    // 19. If accumulated configuration's persistentState value is "optional", follow the steps for the first matching condition from the following list:
    if accumulated_configuration.persistent_state == MediaKeysRequirement::Optional {
        // FIXME: 1. If the implementation requires persisting state for any of the combinations in accumulated configuration
        if false {
            // 1. Change accumulated configuration's persistentState value to "required".
            accumulated_configuration.persistent_state = MediaKeysRequirement::Required;
        }
        // Otherwise
        else {
            // 1. Change accumulated configuration's persistentState value to "not-allowed".
            accumulated_configuration.persistent_state = MediaKeysRequirement::NotAllowed;
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
    let consent_status = get_consent_status(&accumulated_configuration, restrictions, origin);
    match consent_status {
        ConsentStatus::ConsentDenied => {
            // Return ConsentDenied and updated restrictions.
            return None;
        }
        ConsentStatus::InformUser => {
            // FIXME: Inform the user that accumulated configuration is in use in the origin including, specifically,
            //        the information that Distinctive Identifier(s) and/or Distinctive Permanent Identifier(s) as
            //        appropriate will be used if the distinctiveIdentifier member of accumulated configuration is "required".
            //        Continue to the next step.
        }
        ConsentStatus::Allowed => {
            // Continue to the next step.
        }
    }

    // 23. Return accumulated configuration.
    Some(ConsentConfiguration {
        status: consent_status,
        configuration: Some(accumulated_configuration),
    })
}

// https://w3c.github.io/encrypted-media/#get-supported-configuration
pub(crate) fn get_supported_configuration(
    implementation: &dyn KeySystem,
    candidate_configuration: &MediaKeySystemConfiguration,
    origin: &Origin,
) -> Option<ConsentConfiguration> {
    // 1. Let supported configuration be ConsentDenied.
    let mut supported_configuration = Some(ConsentConfiguration {
        status: ConsentStatus::ConsentDenied,
        configuration: None,
    });

    // 2. Initialize restrictions to indicate that no configurations have had user consent denied.
    let mut restrictions = MediaKeyRestrictions::default();

    let mut loop_count = 0;

    // 3. Repeat the following step while supported configuration is ConsentDenied:
    while supported_configuration
        .as_ref()
        .is_some_and(|supported_configuration| supported_configuration.status == ConsentStatus::ConsentDenied)
    {
        // 1. Let supported configuration and, if provided, restrictions be the result of executing
        //    the Get Supported Configuration and Consent algorithm with implementation, candidate configuration, restrictions and origin.
        supported_configuration =
            get_supported_configuration_and_consent(implementation, candidate_configuration, &mut restrictions, origin);

        // AD-HOC: While this is being implemented, we use this to avoid a possible infinite loop
        if loop_count > 5 {
            break;
        }
        loop_count += 1;
    }

    // 4. Return supported configuration.
    supported_configuration
}

// https://w3c.github.io/encrypted-media/#dfn-common-key-systems
pub(crate) fn is_supported_key_system(key_system: &Utf16String) -> bool {
    const SUPPORTED_KEY_SYSTEMS: [&str; 1] = [
        // https://w3c.github.io/encrypted-media/#clear-key
        "org.w3.clearkey",
    ];

    SUPPORTED_KEY_SYSTEMS
        .iter()
        .any(|supported_key_system| *key_system == *supported_key_system)
}

pub(crate) fn key_system_from_string(key_system: &Utf16String) -> Box<dyn KeySystem> {
    if *key_system == "org.w3.clearkey" {
        return Box::new(ClearKeySystem);
    }

    unreachable!("the key system is supported")
}
