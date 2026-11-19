/*
 * Copyright (c) 2025, Psychpsyo <psychpsyo@gmail.com>
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::media_key_system_access::MediaKeySystemConfiguration;

pub(crate) struct MediaKeyRestrictions {
    pub(crate) distinctive_identifiers: bool,
    pub(crate) persist_state: bool,
}

impl Default for MediaKeyRestrictions {
    fn default() -> Self {
        Self {
            distinctive_identifiers: true,
            persist_state: true,
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum CapabilitiesType {
    Audio,
    Video,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum ConsentStatus {
    ConsentDenied,
    #[cfg_attr(
        web_idl_bindings,
        expect(dead_code, reason = "Get Consent Status is not implemented yet")
    )]
    InformUser,
    Allowed,
}

pub(crate) struct ConsentConfiguration {
    pub(crate) status: ConsentStatus,
    pub(crate) configuration: Option<MediaKeySystemConfiguration>,
}
