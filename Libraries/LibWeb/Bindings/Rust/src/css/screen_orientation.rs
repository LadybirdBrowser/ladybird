/*
 * Copyright (c) 2024, Shannon Booth <shannon@serenityos.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! ScreenOrientation, https://w3c.github.io/screen-orientation/#screenorientation-interface

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Realm, Trace, Vm};

use crate::bindings::event_target::{CppEventTarget, GetTheParent};
use crate::bindings::interface::{Ref, Wrappable, allocate};
use crate::bindings::promise::WebIdlPromise;
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// The enumeration OrientationType of CSS/ScreenOrientation.idl.
#[derive(Clone, Copy, Eq, PartialEq)]
#[expect(dead_code, reason = "the type attribute is a stub that reports one orientation")]
pub(crate) enum OrientationType {
    PortraitPrimary,
    PortraitSecondary,
    LandscapePrimary,
    LandscapeSecondary,
}

/// The enumeration OrientationLockType of CSS/ScreenOrientation.idl.
#[derive(Clone, Copy, Eq, PartialEq)]
pub(crate) enum OrientationLockType {
    Any,
    Natural,
    Landscape,
    Portrait,
    PortraitPrimary,
    PortraitSecondary,
    LandscapePrimary,
    LandscapeSecondary,
}

// https://w3c.github.io/screen-orientation/#screenorientation-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ScreenOrientation {
    event_target: CppEventTarget,
    realm: Gc<Realm>,
}

impl Wrappable for ScreenOrientation {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        Some(self.realm)
    }

    fn cpp_parent(&self) -> Option<NonNull<c_void>> {
        self.event_target.as_cpp_parent()
    }
}

impl GetTheParent for ScreenOrientation {
    fn get_the_parent(&self) -> Option<CppEventTarget> {
        None
    }
}

impl ScreenOrientation {
    pub(crate) fn create(vm: &Vm, realm: Gc<Realm>) -> Ref<Self> {
        let object = allocate(vm, |_| ScreenOrientation {
            event_target: CppEventTarget::new(),
            realm,
        });
        object.event_target.set_owner(object);
        object
    }

    // https://w3c.github.io/screen-orientation/#lock-method
    pub(crate) fn lock(&self, _: &mut CallContext, _: OrientationLockType) -> WebIdlResult<WebIdlPromise> {
        Err(Exception::Dom(
            "NotSupportedError",
            Utf16String::from_utf8("FIXME: ScreenOrientation::lock() is not implemented"),
        ))
    }

    // https://w3c.github.io/screen-orientation/#unlock-method
    pub(crate) fn unlock(&self, _: &mut CallContext) -> WebIdlResult<()> {
        // FIXME: Implement this.
        Ok(())
    }

    // https://w3c.github.io/screen-orientation/#type-attribute
    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<OrientationType> {
        // FIXME: Implement this.
        Ok(OrientationType::LandscapePrimary)
    }

    // https://w3c.github.io/screen-orientation/#angle-attribute
    pub(crate) fn angle(&self, _: &mut CallContext) -> WebIdlResult<u16> {
        // FIXME: Implement this.
        Ok(0)
    }
}
