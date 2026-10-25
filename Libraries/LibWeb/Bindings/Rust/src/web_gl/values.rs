/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16String;
use libjs_rust::api::{
    Array, ArrayBuffer, ArrayBufferShared, MarkedVec, PrimitiveString, TypedArrayKind, Value,
    create_typed_array_on_buffer,
};

use super::proxy::recorder::bytes_of;
use super::state::ContextState;
use crate::bindings::CallContext;
use crate::bindings::interface::{Interface, Ref, wrap};

impl ContextState {
    fn typed_array(&self, cx: &CallContext, kind: TypedArrayKind, bytes: &[u8], length: usize) -> Value {
        let array_buffer = ArrayBuffer::create_from_bytes(cx.vm(), self.realm(), bytes, ArrayBufferShared::No);
        let length = u32::try_from(length).expect("a typed array of a few numbers");
        Value::from_object(create_typed_array_on_buffer(
            cx.vm(),
            self.realm(),
            kind,
            length,
            array_buffer,
        ))
    }

    pub(crate) fn int32_array(&self, cx: &CallContext, numbers: &[i32]) -> Value {
        self.typed_array(cx, TypedArrayKind::Int32Array, bytes_of(numbers), numbers.len())
    }

    pub(crate) fn uint32_array(&self, cx: &CallContext, numbers: &[u32]) -> Value {
        self.typed_array(cx, TypedArrayKind::Uint32Array, bytes_of(numbers), numbers.len())
    }

    pub(crate) fn float32_array(&self, cx: &CallContext, numbers: &[f32]) -> Value {
        self.typed_array(cx, TypedArrayKind::Float32Array, bytes_of(numbers), numbers.len())
    }

    /// A `sequence<GLboolean>` returned as `any` is a JS array.
    pub(crate) fn boolean_array(&self, cx: &CallContext, booleans: &[bool]) -> Value {
        let elements = MarkedVec::with_capacity(cx.vm(), booleans.len());
        for &boolean in booleans {
            elements.push(Value::from_bool(boolean));
        }
        Value::from_object(Array::create_from_list(cx.vm(), self.realm(), &elements))
    }
}

pub(crate) fn string_value(cx: &CallContext, string: Utf16String) -> Value {
    Value::from_string(PrimitiveString::create(cx.vm(), string))
}

pub(crate) fn object_or_null<T: Interface>(cx: &CallContext, object: Option<Ref<T>>) -> Value {
    object.map_or(Value::NULL, |object| Value::from_object(wrap(cx, object)))
}
