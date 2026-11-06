/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Dispatch to the steps of serializable interfaces. The generated Rust bindings classify their wrappers exactly,
//! and C++ reports the primary interface of objects that it implements.

use libjs_rust::api::{Gc, Object, Value, Vm};

use crate::bindings::structured_data::{SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, WebIdlResult};
use crate::html::structured_serialize::cpp::cpp_serializable_interface_name;

#[cfg(web_idl_bindings)]
use crate::bindings::generated::serializable_objects as generated;

// A standalone cargo build has no wrapper classes or interface identifiers to dispatch on.
#[cfg(not(web_idl_bindings))]
mod generated {
    use super::*;

    pub(crate) fn rust_interface_name(_: Gc<Object>) -> Option<u16> {
        unreachable!("serialization requires the generated bindings")
    }

    pub(crate) fn is_cpp_serializable_interface(_: u16) -> bool {
        unreachable!("serialization requires the generated bindings")
    }

    pub(crate) fn serialization_steps(
        _: u16,
        _: &Vm,
        _: Gc<Object>,
        _: &mut SerializationWriter,
        _: bool,
    ) -> WebIdlResult<()> {
        unreachable!("serialization requires the generated bindings")
    }

    pub(crate) fn deserialization_steps(_: u16, _: &CallContext, _: &mut SerializationReader) -> WebIdlResult<Value> {
        unreachable!("serialization requires the generated bindings")
    }
}

pub(crate) use generated::{deserialization_steps, serialization_steps};

pub(crate) fn serializable_interface_name(object: Gc<Object>) -> Option<u16> {
    generated::rust_interface_name(object).or_else(|| {
        cpp_serializable_interface_name(object).filter(|&name| generated::is_cpp_serializable_interface(name))
    })
}
