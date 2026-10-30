/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The serialization steps of DOMException, https://webidl.spec.whatwg.org/#idl-DOMException, which LibWeb implements in
//! C++ (Libraries/LibWeb/WebIDL/DOMException.cpp).

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libjs_rust::api::{Gc, Object, Realm};

use crate::bindings::WebIdlResult;
use crate::html::structured_serialize::cpp::StructuredSerializeReader;
use crate::html::structured_serialize::writer::StructuredSerializeWriter;

unsafe extern "C" {
    fn web_dom_exception_name_and_message(exception: *mut c_void, name: &mut usize, message: &mut usize);
    fn web_dom_exception_create(realm: *mut c_void, name: usize, message: usize) -> *mut c_void;
}

/// The name and the message of a DOMException.
pub(crate) struct DOMExceptionFields {
    pub(crate) name: Utf16String,
    pub(crate) message: Utf16String,
}

impl DOMExceptionFields {
    /// The fields of the DOMException, or of an object of an interface that inherits from it.
    pub(crate) fn of(exception: Gc<Object>) -> Self {
        let (mut name, mut message) = (0, 0);
        // SAFETY: The object is a live DOMException.
        unsafe { web_dom_exception_name_and_message(exception.as_ptr().cast(), &mut name, &mut message) };
        // SAFETY: LibWeb hands over the strings.
        unsafe {
            Self {
                name: Utf16String::from_raw_owned(name),
                message: Utf16String::from_raw_owned(message),
            }
        }
    }
}

/// A new DOMException with the fields, created in the realm.
pub(crate) fn create(realm: Gc<Realm>, fields: &DOMExceptionFields) -> Gc<Object> {
    // SAFETY: The realm is live, and the call borrows the strings.
    let exception = unsafe {
        web_dom_exception_create(
            realm.as_ptr().cast(),
            fields.name.raw_identity(),
            fields.message.raw_identity(),
        )
    };
    // SAFETY: LibWeb returns a live object.
    unsafe { Gc::from_non_null(NonNull::new(exception.cast()).expect("LibWeb creates the exception")) }
}

// https://webidl.spec.whatwg.org/#idl-DOMException
pub(crate) fn serialization_steps(value: &DOMExceptionFields, serialized: &StructuredSerializeWriter) {
    // 1. Set serialized.[[Name]] to value’s name.
    serialized.encode_utf16_string(&value.name);

    // 2. Set serialized.[[Message]] to value’s message.
    serialized.encode_utf16_string(&value.message);

    // FIXME: 3. User agents should attach a serialized representation of any interesting accompanying data which are not yet specified, notably the stack property, to serialized.
}

// https://webidl.spec.whatwg.org/#idl-DOMException
pub(crate) fn deserialization_steps(serialized: StructuredSerializeReader) -> WebIdlResult<DOMExceptionFields> {
    // 1. Set value’s name to serialized.[[Name]].
    let name = serialized.decode_utf16_string()?;

    // 2. Set value’s message to serialized.[[Message]].
    let message = serialized.decode_utf16_string()?;

    // FIXME: 3. If any other data is attached to serialized, then deserialize and attach it to value.

    Ok(DOMExceptionFields { name, message })
}
