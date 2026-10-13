/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The runtime of the WebIDL bindings that Meta/Generators/generate_libweb_bindings.py writes in Rust, for the
//! interfaces that their IDL marks [ImplementedIn=Rust]. The bindings of such an interface are the native functions,
//! constructor and prototype of the interface, which call the methods of the interface's implementation in this
//! crate: one per constructor, attribute and operation, whose signatures the generated bindings document. A dictionary
//! of the interface's IDL is a struct next to the implementation, whose fields are the dictionary's members; the
//! bindings convert it.
//!
//! Values and cells live in locals while a binding runs, which the collector scans; a binding that gathers values in
//! a container keeps them in a MarkedVec.

pub(crate) mod conversions;
#[cfg(web_idl_bindings)]
pub(crate) mod generated;
pub(crate) mod interface;
pub(crate) mod realm_data;
pub(crate) mod services;

use ak::Utf16String;
use libjs_rust::api::{ErrorKind, Gc, Realm, Throw, ThrowCompletionOr, Vm};

/// What the bindings pass the implementation of an interface: the VM, and the realm that the implementation creates
/// its results in. For an attribute or an operation, that is the realm of the object it runs on; for a constructor,
/// the current realm.
pub(crate) struct CallContext<'vm> {
    vm: &'vm Vm,
    realm: Gc<Realm>,
}

impl<'vm> CallContext<'vm> {
    pub(crate) fn new(vm: &'vm Vm, realm: Gc<Realm>) -> Self {
        Self { vm, realm }
    }

    pub(crate) fn vm(&self) -> &'vm Vm {
        self.vm
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm
    }
}

/// An exception that the implementation of an interface throws.
pub(crate) enum Exception {
    /// A value that JavaScript code threw.
    Thrown(Throw),
    /// A simple exception with the message, which the bindings create in the current realm.
    TypeError(Utf16String),
    RangeError(Utf16String),
}

impl From<Throw> for Exception {
    fn from(throw: Throw) -> Self {
        Self::Thrown(throw)
    }
}

impl Exception {
    pub(crate) fn throw(self, vm: &Vm) -> Throw {
        match self {
            Self::Thrown(throw) => throw,
            Self::TypeError(message) => thrown(vm.throw_completion_with_utf16_message(ErrorKind::TypeError, message)),
            Self::RangeError(message) => thrown(vm.throw_completion_with_utf16_message(ErrorKind::RangeError, message)),
        }
    }
}

fn thrown(completion: ThrowCompletionOr<()>) -> Throw {
    match completion {
        Err(throw) => throw,
        Ok(()) => unreachable!("throwing a completion throws"),
    }
}

pub(crate) type WebIdlResult<T> = Result<T, Exception>;
