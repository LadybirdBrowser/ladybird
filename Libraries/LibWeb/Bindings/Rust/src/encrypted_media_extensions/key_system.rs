/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;

use super::encrypted_media_extensions::MediaKeyRestrictions;
use super::media_key_system_access::MediaKeySystemConfiguration;

pub(crate) trait KeySystem {
    fn supports_init_data_type(&self, init_data_type: &Utf16String) -> bool;
    fn supports_encryption_scheme(&self, encryption_scheme: &Utf16String) -> bool;
    fn supports_robustness(&self, robustness: &Utf16String) -> bool;
    fn definitely_supports_playback(
        &self,
        container: &str,
        media_types: &str,
        encryption_scheme: Option<&Utf16String>,
        robustness: &Utf16String,
        accumulated_configuration: &MediaKeySystemConfiguration,
        restrictions: &MediaKeyRestrictions,
    ) -> bool;
}

pub(crate) struct ClearKeySystem;

impl KeySystem for ClearKeySystem {
    // https://w3c.github.io/encrypted-media/#clear-key-behavior
    fn supports_init_data_type(&self, init_data_type: &Utf16String) -> bool {
        // Implementations SHOULD support the "keyids" type
        // TODO: Implementations MAY support any combination of registered Initialization Data Types
        // https://www.w3.org/TR/eme-initdata-registry/
        const REGISTERED_INIT_DATA_TYPES: [&str; 1] = ["keyids"];

        REGISTERED_INIT_DATA_TYPES
            .iter()
            .any(|registered_init_data_type| *init_data_type == *registered_init_data_type)
    }

    // https://w3c.github.io/encrypted-media/#clear-key-capabilities
    fn supports_encryption_scheme(&self, encryption_scheme: &Utf16String) -> bool {
        // encryptionScheme: Implementations MUST support the "cenc" scheme, and MAY support other schemes.
        *encryption_scheme == "cenc"
    }

    // https://w3c.github.io/encrypted-media/#clear-key-capabilities
    fn supports_robustness(&self, robustness: &Utf16String) -> bool {
        // robustness: Only the empty string is supported.
        robustness.is_empty()
    }

    fn definitely_supports_playback(
        &self,
        _container: &str,
        _media_types: &str,
        _encryption_scheme: Option<&Utf16String>,
        _robustness: &Utf16String,
        _accumulated_configuration: &MediaKeySystemConfiguration,
        _restrictions: &MediaKeyRestrictions,
    ) -> bool {
        // FIXME: Do some checks here
        true
    }
}
