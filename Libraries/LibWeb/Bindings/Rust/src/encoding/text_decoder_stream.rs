/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TextDecoderStream, https://encoding.spec.whatwg.org/#interface-textdecoderstream

use core::cell::RefCell;

use ak::Utf16String;
use encoding_rs::Encoding;
use libjs_rust::api::{EmbedderClass, EmbedderValue, PrimitiveString, Trace, Value};
use libtextcodec_rust::{DecodingFailed, StreamingDecoder, encoding_for_label};

use crate::bindings::conversions::{CppInterface, UsvString, buffer_source_type_of};
use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::encoding::text_decoder::TextDecoderOptions;
use crate::streams::generic_transform_stream::{TransformSteps, TransformStream};

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TextDecoderStream {
    // https://encoding.spec.whatwg.org/#textdecoder-encoding
    #[gc(untraced)]
    encoding: &'static Encoding,
    // https://encoding.spec.whatwg.org/#textdecoder-error-mode
    fatal: bool,
    // https://encoding.spec.whatwg.org/#textdecoder-ignore-bom-flag
    ignore_bom: bool,
    // https://streams.spec.whatwg.org/#generictransformstream-transform
    transform: TransformStream,
}

impl Wrappable for TextDecoderStream {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl TextDecoderStream {
    // https://encoding.spec.whatwg.org/#dom-textdecoderstream
    pub(crate) fn construct(
        cx: &mut CallContext,
        label: Utf16String,
        options: TextDecoderOptions,
    ) -> WebIdlResult<Self> {
        // 1. Let encoding be the result of getting an encoding from label.
        // 2. If encoding is failure or replacement, then throw a RangeError.
        let encoding = encoding_for_label(label.as_units())
            .filter(|&encoding| encoding != encoding_rs::REPLACEMENT)
            .ok_or_else(|| Exception::RangeError(Utf16String::from_ascii(b"Invalid encoding")))?;

        // 3. Let errorMode be "fatal" if options["fatal"] is true; otherwise "replacement".
        // 4. Set up a text decoder stream with this, encoding, errorMode, and options["ignoreBOM"].
        Ok(Self::set_up(cx, encoding, options.fatal, options.ignore_bom))
    }

    // https://encoding.spec.whatwg.org/#set-up-a-text-decoder-stream
    fn set_up(cx: &CallContext, encoding: &'static Encoding, fatal: bool, ignore_bom: bool) -> Self {
        // 1. Assert: encoding is not replacement.
        assert!(
            encoding != encoding_rs::REPLACEMENT,
            "a text decoder stream has an encoding with a decoder"
        );

        // 2. Set stream's encoding to encoding.
        // 3. Set stream's error mode to errorMode.
        // 4. Set stream's ignore BOM to ignoreBOM.
        // 5. Set stream's decoder to a new instance of encoding's decoder.
        // 6. Set stream's I/O queue to a new I/O queue.
        // NB: The decoder holds the I/O queue, and whether the BOM was seen.
        let decoder = Decoder {
            decoder: RefCell::new(StreamingDecoder::new(encoding, fatal, ignore_bom)),
        };

        // 7. Let transformAlgorithm be an algorithm which takes a chunk argument and runs the decode and enqueue a
        //    chunk algorithm with stream and chunk.
        // 8. Let flushAlgorithm be an algorithm which takes no arguments and runs the flush and enqueue algorithm with
        //    stream.
        // 9. Let transformStream be a new TransformStream.
        // 10. Set up transformStream with transformAlgorithm set to transformAlgorithm and flushAlgorithm set to
        //     flushAlgorithm.
        // 11. Set stream's transform to transformStream.
        Self {
            encoding,
            fatal,
            ignore_bom,
            transform: TransformStream::create_and_set_up(cx, decoder),
        }
    }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-encoding
    pub(crate) fn encoding(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The encoding getter steps are to return this's encoding's name, ASCII lowercased.
        let name = self.encoding.name().to_ascii_lowercase();
        Ok(Utf16String::from_ascii(name.as_bytes()))
    }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-fatal
    pub(crate) fn fatal(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The fatal getter steps are to return true if this's error mode is "fatal", otherwise false.
        Ok(self.fatal)
    }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-ignorebom
    pub(crate) fn ignore_bom(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The ignoreBOM getter steps are to return this's ignore BOM.
        Ok(self.ignore_bom)
    }

    // https://streams.spec.whatwg.org/#dom-generictransformstream-readable
    pub(crate) fn readable(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        // The readable getter steps are to return this's transform.[[readable]].
        Ok(self.transform.readable(cx))
    }

    // https://streams.spec.whatwg.org/#dom-generictransformstream-writable
    pub(crate) fn writable(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        // The writable getter steps are to return this's transform.[[writable]].
        Ok(self.transform.writable(cx))
    }
}

/// The decoder of a TextDecoderStream, with its I/O queue, which the algorithms of the stream's transform run on.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct Decoder {
    #[gc(untraced)]
    decoder: RefCell<StreamingDecoder>,
}

static DECODER_CLASS: EmbedderClass<Decoder> = EmbedderClass::new("TextDecoderStreamDecoder");

impl EmbedderValue for Decoder {
    const CLASS: &'static EmbedderClass<Self> = &DECODER_CLASS;
}

/// Enqueues the output of the decoder in the transform as a string, unless it is the empty string.
fn enqueue_output_chunk(cx: &CallContext, transform: &TransformStream, output: &[u16]) {
    let output_chunk = UsvString::from_utf16_replacing_lone_surrogates(output).into_utf16_string();
    if !output_chunk.is_empty() {
        transform.enqueue(Value::from_string(PrimitiveString::create(cx.vm(), output_chunk)));
    }
}

fn decoding_failed(_: DecodingFailed) -> Exception {
    Exception::TypeError(Utf16String::from_ascii(b"Decoding failed"))
}

impl TransformSteps for Decoder {
    // https://encoding.spec.whatwg.org/#decode-and-enqueue-a-chunk
    fn transform(&self, cx: &CallContext, transform: &TransformStream, chunk: Value) -> WebIdlResult<()> {
        // 1. Let bufferSource be the result of converting chunk to an AllowSharedBufferSource.
        let Some(buffer_source) = buffer_source_type_of(chunk) else {
            return Err(Exception::TypeError(Utf16String::from_ascii(
                b"Chunk is not a BufferSource",
            )));
        };

        // 2. Push a copy of bufferSource to decoder's I/O queue.
        // 3. Let output be the I/O queue of scalar values « end-of-queue ».
        // 4. While true:
        //     1. Let item be the result of reading from decoder's I/O queue.
        //     2. If item is end-of-queue:
        //         1. Let outputChunk be the result of running serialize I/O queue with decoder and output.
        //         2. If outputChunk is not the empty string, then enqueue outputChunk in decoder's transform.
        //         3. Return.
        //     3. Let result be the result of processing an item with item, decoder's decoder, decoder's I/O queue,
        //        output, and decoder's error mode.
        //     4. If result is error, then throw a TypeError.
        // NB: The decoder decodes the bytes as more input to follow, and keeps what it cannot decode yet.
        let mut decoder = self.decoder.borrow_mut();
        // SAFETY: Decoding runs no script and allocates no cell, so nothing detaches, resizes or frees the buffer of
        //         the chunk while its bytes are read.
        let output =
            unsafe { buffer_source.with_bytes(|bytes| decoder.decode(bytes, true)) }.map_err(decoding_failed)?;
        enqueue_output_chunk(cx, transform, &output);
        Ok(())
    }

    // https://encoding.spec.whatwg.org/#flush-and-enqueue
    fn flush(&self, cx: &CallContext, transform: &TransformStream) -> WebIdlResult<()> {
        // 1. Let output be the I/O queue of scalar values « end-of-queue ».
        // 2. While true:
        //     1. Let item be the result of reading from decoder's I/O queue.
        //     2. Let result be the result of processing an item with item, decoder's decoder, decoder's I/O queue,
        //        output, and decoder's error mode.
        //     3. If result is finished:
        //         1. Let outputChunk be the result of running serialize I/O queue with decoder and output.
        //         2. If outputChunk is not the empty string, then enqueue outputChunk in decoder's transform.
        //         3. Return.
        //     4. Otherwise, if result is error, throw a TypeError.
        // NB: The decoder's I/O queue holds no more bytes, so the decoder decodes the end of the input.
        let output = self.decoder.borrow_mut().decode(&[], false).map_err(decoding_failed)?;
        enqueue_output_chunk(cx, transform, &output);
        Ok(())
    }
}
