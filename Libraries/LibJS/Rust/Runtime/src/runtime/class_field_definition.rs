/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::layout::cell::Gc;
use crate::layout::function_object::EcmascriptFunctionObject;
use crate::layout::value::Value;
use crate::runtime::private_environment::PrivateName;
use crate::runtime::property_key::PropertyKey;

/// The name of a class element: Variant<PropertyKey, PrivateName>.
#[derive(Clone, Debug, PartialEq, Eq, Trace)]
pub enum ClassElementName {
    PropertyKey(PropertyKey),
    PrivateName(#[gc(untraced)] PrivateName),
}

/// [[Initializer]]: Variant<GC::Ref<ECMAScriptFunctionObject>, Value, Empty>.
#[derive(Clone, Copy, Debug, Trace)]
pub enum ClassFieldInitializer {
    Function(Gc<EcmascriptFunctionObject>),
    Value(Value),
    Empty,
}

// 6.2.10 The ClassFieldDefinition Record Specification Type, https://tc39.es/ecma262/#sec-classfielddefinition-record-specification-type
#[derive(Clone, Debug, Trace)]
pub struct ClassFieldDefinition {
    pub name: ClassElementName,             // [[Name]]
    pub initializer: ClassFieldInitializer, // [[Initializer]]
}
