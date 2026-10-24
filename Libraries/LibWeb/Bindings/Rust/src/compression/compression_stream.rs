/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CompressionStream, https://compression.spec.whatwg.org/#compressionstream

use core::cell::RefCell;
use std::io;

use ak::Utf16String;
use libcompress_rust::{Compressor, Format};
use libjs_rust::api::{EmbedderClass, EmbedderValue, Trace, Value};

use crate::bindings::conversions::create_uint8_array;
use crate::bindings::interface::Ref;
use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::compression::buffer_source_of_chunk;
use crate::streams::generic_transform_stream::{TransformSteps, create_and_set_up};
use crate::streams::readable_stream::ReadableStream;
use crate::streams::transform_stream::TransformStream;
use crate::streams::writable_stream::WritableStream;

/// https://compression.spec.whatwg.org/#enumdef-compressionformat
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CompressionFormat {
    Brotli,
    Deflate,
    DeflateRaw,
    Gzip,
}

impl From<CompressionFormat> for Format {
    fn from(format: CompressionFormat) -> Self {
        match format {
            CompressionFormat::Brotli => Format::Brotli,
            CompressionFormat::Deflate => Format::Zlib,
            CompressionFormat::DeflateRaw => Format::Deflate,
            CompressionFormat::Gzip => Format::Gzip,
        }
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CompressionStream {
    // https://streams.spec.whatwg.org/#generictransformstream-transform
    transform: Ref<TransformStream>,
}

impl Wrappable for CompressionStream {}

impl CompressionStream {
    // https://compression.spec.whatwg.org/#dom-compressionstream-compressionstream
    pub(crate) fn construct(cx: &mut CallContext, format: CompressionFormat) -> WebIdlResult<Self> {
        // 1. If format is unsupported in CompressionStream, then throw a TypeError.
        // 2. Set this's format to format.
        let context = CompressionContext::new(format);

        // 3. Let transformAlgorithm be an algorithm which takes a chunk argument and runs the compress and enqueue a chunk
        //    algorithm with this and chunk.
        // 4. Let flushAlgorithm be an algorithm which takes no argument and runs the compress flush and enqueue algorithm with this.
        // 5. Set this's transform to a new TransformStream.
        // 6. Set up this's transform with transformAlgorithm set to transformAlgorithm and flushAlgorithm set to flushAlgorithm.
        Ok(Self {
            transform: create_and_set_up(cx, context),
        })
    }

    // https://streams.spec.whatwg.org/#dom-generictransformstream-readable
    pub(crate) fn readable(&self, _: &mut CallContext) -> WebIdlResult<Ref<ReadableStream>> {
        // The readable getter steps are to return this's transform.[[readable]].
        Ok(self.transform.readable_stream())
    }

    // https://streams.spec.whatwg.org/#dom-generictransformstream-writable
    pub(crate) fn writable(&self, _: &mut CallContext) -> WebIdlResult<Ref<WritableStream>> {
        // The writable getter steps are to return this's transform.[[writable]].
        Ok(self.transform.writable_stream())
    }
}

/// https://compression.spec.whatwg.org/#compression-context, which the algorithms of the stream's transform run on.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct CompressionContext {
    #[gc(untraced)]
    compressor: RefCell<Compressor>,
}

static COMPRESSION_CONTEXT_CLASS: EmbedderClass<CompressionContext> = EmbedderClass::new("CompressionContext");

impl EmbedderValue for CompressionContext {
    const CLASS: &'static EmbedderClass<Self> = &COMPRESSION_CONTEXT_CLASS;
}

impl CompressionContext {
    fn new(format: CompressionFormat) -> Self {
        Self {
            compressor: RefCell::new(Compressor::new(format.into())),
        }
    }
}

fn compression_failed(step: &str, error: &io::Error) -> Exception {
    Exception::TypeError(Utf16String::from_utf8(&format!("Unable to compress {step}: {error}")))
}

impl TransformSteps for CompressionContext {
    // https://compression.spec.whatwg.org/#compress-and-enqueue-a-chunk
    fn transform(&self, cx: &CallContext, transform: Ref<TransformStream>, chunk: Value) -> WebIdlResult<()> {
        // 1. If chunk is not a BufferSource type, then throw a TypeError.
        let chunk = buffer_source_of_chunk(cx, chunk)?;

        // 2. Let buffer be the result of compressing chunk with cs's format and context.
        // SAFETY: Compressing neither runs script nor allocates a cell.
        let buffer = unsafe { chunk.with_bytes(|bytes| self.compressor.borrow_mut().compress(bytes)) }
            .map_err(|error| compression_failed("chunk", &error))?;

        // 3. If buffer is empty, return.
        if buffer.is_empty() {
            return Ok(());
        }

        // 4. Let arrays be the result of splitting buffer into one or more non-empty pieces and converting them into
        //    Uint8Arrays.
        let array = create_uint8_array(cx, &buffer)?;

        // 5. For each Uint8Array array of arrays, enqueue array in cs's transform.
        TransformStream::enqueue(transform, cx.vm(), Value::from_object(array));
        Ok(())
    }

    // https://compression.spec.whatwg.org/#compress-flush-and-enqueue
    fn flush(&self, cx: &CallContext, transform: Ref<TransformStream>) -> WebIdlResult<()> {
        // 1. Let buffer be the result of compressing an empty input with cs's format and context, with the finish flag.
        let buffer = self
            .compressor
            .borrow_mut()
            .finish()
            .map_err(|error| compression_failed("flush", &error))?;

        // 2. If buffer is empty, return.
        if buffer.is_empty() {
            return Ok(());
        }

        // 3. Let arrays be the result of splitting buffer into one or more non-empty pieces and converting them into
        //    Uint8Arrays.
        let array = create_uint8_array(cx, &buffer)?;

        // 4. For each Uint8Array array of arrays, enqueue array in cs's transform.
        TransformStream::enqueue(transform, cx.vm(), Value::from_object(array));
        Ok(())
    }
}
