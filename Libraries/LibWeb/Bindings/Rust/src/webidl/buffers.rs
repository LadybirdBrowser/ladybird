/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_rust::api::{ArrayBuffer, Gc};

use crate::bindings::conversions::{ArrayBufferView, BufferSource};

/// The union of WebIDL/Buffers.idl, whose members the bindings convert.
#[derive(Clone, Copy)]
pub(crate) enum AllowSharedBufferSource {
    ArrayBuffer(Gc<ArrayBuffer>),
    SharedArrayBuffer(Gc<ArrayBuffer>),
    ArrayBufferView(ArrayBufferView),
}

impl From<AllowSharedBufferSource> for BufferSource {
    fn from(source: AllowSharedBufferSource) -> Self {
        match source {
            AllowSharedBufferSource::ArrayBuffer(buffer) | AllowSharedBufferSource::SharedArrayBuffer(buffer) => {
                Self::ArrayBuffer(buffer)
            }
            AllowSharedBufferSource::ArrayBufferView(view) => view.into(),
        }
    }
}
