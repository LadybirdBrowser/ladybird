/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Objects, their properties and their internal methods.

use crate::layout::cell::Gc;
use crate::layout::value::Value;
use crate::runtime::object::HostIntrinsicAccessor;
use crate::runtime::realm::Realm;

pub fn call_host_intrinsic_accessor(accessor: HostIntrinsicAccessor, realm: Gc<Realm>) -> Value {
    // SAFETY: The embedder defined the accessor for a property of an object of this runtime, and computes its value in
    //         the realm of that object.
    Value(unsafe { accessor(realm.as_ptr()) })
}
