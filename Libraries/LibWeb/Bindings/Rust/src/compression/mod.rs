/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Compression Standard, https://compression.spec.whatwg.org/

pub(crate) mod compression_stream;
pub(crate) mod decompression_stream;

use ak::Utf16String;
use libjs_rust::api::Value;

use crate::bindings::conversions::{BufferSource, BufferSourceRules, buffer_source_if};
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// The chunk as a BufferSource, or the TypeError that compressing or decompressing a chunk throws when it is not a
/// BufferSource type.
fn buffer_source_of_chunk(cx: &CallContext, chunk: Value) -> WebIdlResult<BufferSource> {
    let rules = BufferSourceRules {
        allow_shared: false,
        allow_resizable: false,
    };
    let buffer_source = if chunk.is_object() {
        buffer_source_if(cx.vm(), chunk.as_object(), &rules)?
    } else {
        None
    };
    buffer_source.ok_or_else(|| Exception::TypeError(Utf16String::from_ascii(b"Chunk is not a BufferSource type")))
}
