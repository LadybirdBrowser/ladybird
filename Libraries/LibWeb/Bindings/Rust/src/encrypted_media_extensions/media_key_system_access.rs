/*
 * Copyright (c) 2025, stelar7 <dudedbz@gmail.com>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! MediaKeySystemAccess, https://w3c.github.io/encrypted-media/#mediakeysystemaccess-interface

use ak::Utf16String;
use libjs_rust::api::{Trace, Vm};

use super::key_system::KeySystem;
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::{CallContext, WebIdlResult};

// https://w3c.github.io/encrypted-media/#dom-mediakeysrequirement
#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum MediaKeysRequirement {
    Required,
    Optional,
    NotAllowed,
}

// https://w3c.github.io/encrypted-media/#dom-mediakeysystemconfiguration
#[derive(Clone)]
pub(crate) struct MediaKeySystemConfiguration {
    pub(crate) label: Utf16String,
    pub(crate) init_data_types: Vec<Utf16String>,
    pub(crate) audio_capabilities: Vec<MediaKeySystemMediaCapability>,
    pub(crate) video_capabilities: Vec<MediaKeySystemMediaCapability>,
    pub(crate) distinctive_identifier: MediaKeysRequirement,
    pub(crate) persistent_state: MediaKeysRequirement,
    pub(crate) session_types: Option<Vec<Utf16String>>,
}

impl Default for MediaKeySystemConfiguration {
    fn default() -> Self {
        Self {
            label: Utf16String::from_ascii(b""),
            init_data_types: Vec::new(),
            audio_capabilities: Vec::new(),
            video_capabilities: Vec::new(),
            distinctive_identifier: MediaKeysRequirement::Optional,
            persistent_state: MediaKeysRequirement::Optional,
            session_types: None,
        }
    }
}

// https://w3c.github.io/encrypted-media/#dom-mediakeysystemmediacapability
#[derive(Clone)]
pub(crate) struct MediaKeySystemMediaCapability {
    pub(crate) content_type: Utf16String,
    pub(crate) encryption_scheme: Option<Utf16String>,
    pub(crate) robustness: Utf16String,
}

// https://w3c.github.io/encrypted-media/#dom-mediakeysessiontype
#[derive(Clone, Copy)]
pub(crate) enum MediaKeySessionType {
    Temporary,
    PersistentLicense,
}

impl MediaKeySessionType {
    pub(crate) fn from_string(session_type: &Utf16String) -> Option<Self> {
        if *session_type == "temporary" {
            return Some(Self::Temporary);
        }
        if *session_type == "persistent-license" {
            return Some(Self::PersistentLicense);
        }
        None
    }
}

// https://w3c.github.io/encrypted-media/#dom-mediakeysystemaccess
#[derive(Trace)]
pub(crate) struct MediaKeySystemAccess {
    key_system: Utf16String,

    #[gc(untraced)]
    configuration: MediaKeySystemConfiguration,
    #[gc(untraced)]
    #[cfg_attr(
        web_idl_bindings,
        expect(dead_code, reason = "createMediaKeys() is not implemented yet")
    )]
    cdm_implementation: Box<dyn KeySystem>,
}

impl Wrappable for MediaKeySystemAccess {}

impl MediaKeySystemAccess {
    pub(crate) fn create(
        vm: &Vm,
        key_system: Utf16String,
        configuration: MediaKeySystemConfiguration,
        cdm_implementation: Box<dyn KeySystem>,
    ) -> Ref<Self> {
        allocate(vm, |_| Self {
            key_system,
            configuration,
            cdm_implementation,
        })
    }

    pub(crate) fn key_system(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.key_system.clone())
    }

    pub(crate) fn get_configuration(&self, _: &mut CallContext) -> WebIdlResult<MediaKeySystemConfiguration> {
        Ok(self.configuration.clone())
    }
}
