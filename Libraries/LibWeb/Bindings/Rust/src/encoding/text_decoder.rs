/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TextDecoder, https://encoding.spec.whatwg.org/#interface-textdecoder

use core::cell::RefCell;

use ak::Utf16String;
use libjs_rust::api::Trace;
use libtextcodec_rust::{DecodingFailed, StreamingDecoder, encoding_for_label};

use crate::bindings::conversions::{BufferSource, UsvString};
use crate::bindings::interface::Wrappable;
use crate::bindings::{CallContext, Exception, WebIdlResult};

/// The dictionary TextDecoderOptions of Encoding/TextDecoder.idl, whose members the bindings convert.
pub(crate) struct TextDecoderOptions {
    pub(crate) fatal: bool,
    pub(crate) ignore_bom: bool,
}

/// The dictionary TextDecodeOptions of Encoding/TextDecoder.idl, whose members the bindings convert.
pub(crate) struct TextDecodeOptions {
    pub(crate) stream: bool,
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TextDecoder {
    /// The encoding, the error mode and the ignore BOM flag of the decoder, with the decoder of the encoding, which
    /// holds the I/O queue of a decode() that asked for more input to follow.
    #[gc(untraced)]
    decoder: RefCell<StreamingDecoder>,
}

impl Wrappable for TextDecoder {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl TextDecoder {
    // https://encoding.spec.whatwg.org/#dom-textdecoder
    pub(crate) fn construct(
        _: &mut CallContext,
        label: Utf16String,
        options: TextDecoderOptions,
    ) -> WebIdlResult<Self> {
        // 1. Let encoding be the result of getting an encoding from label.
        // 2. If encoding is failure or replacement, then throw a RangeError.
        let encoding = encoding_for_label(label.as_units())
            .filter(|&encoding| encoding != encoding_rs::REPLACEMENT)
            .ok_or_else(|| Exception::RangeError(Utf16String::from_ascii(b"Invalid encoding")))?;

        // 3. Set this's encoding to encoding.
        // 4. If options["fatal"] is true, then set this's error mode to "fatal".
        // 5. Set this's ignore BOM to options["ignoreBOM"].
        Ok(Self {
            decoder: RefCell::new(StreamingDecoder::new(encoding, options.fatal, options.ignore_bom)),
        })
    }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-encoding
    pub(crate) fn encoding(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The encoding getter steps are to return this's encoding's name, ASCII lowercased.
        let name = self.decoder.borrow().encoding().name().to_ascii_lowercase();
        Ok(Utf16String::from_ascii(name.as_bytes()))
    }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-fatal
    pub(crate) fn fatal(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The fatal getter steps are to return true if this's error mode is "fatal", otherwise false.
        Ok(self.decoder.borrow().fatal())
    }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-ignorebom
    pub(crate) fn ignore_bom(&self, _: &mut CallContext) -> WebIdlResult<bool> {
        // The ignoreBOM getter steps are to return this's ignore BOM.
        Ok(self.decoder.borrow().ignore_bom())
    }

    // https://encoding.spec.whatwg.org/#dom-textdecoder-decode
    pub(crate) fn decode(
        &self,
        _: &mut CallContext,
        input: Option<BufferSource>,
        options: TextDecodeOptions,
    ) -> WebIdlResult<UsvString> {
        // 1. If this's do not flush is false, then set this's decoder to a new instance of this's encoding's decoder,
        //    this's I/O queue to the I/O queue of bytes « end-of-queue », and this's BOM seen to false.
        // 2. Set this's do not flush to options["stream"].
        // 3. If input is given, then push a copy of input to this's I/O queue.
        // 4. Let output be the I/O queue of scalar values « end-of-queue ».
        // 5. While true:
        //     1. Let item be the result of reading from this's I/O queue.
        //     2. If item is end-of-queue and this's do not flush is true, then return the result of running
        //        serialize I/O queue with this and output.
        //     3. Otherwise:
        //         1. Let result be the result of processing an item with item, this's decoder, this's I/O queue,
        //            output, and this's error mode.
        //         2. If result is finished, then return the result of running serialize I/O queue with this and
        //            output.
        //         3. Otherwise, if result is error, throw a TypeError.
        // NB: The decoder holds the I/O queue and whether the BOM was seen, and is replaced when a call that does not
        //     stream returns, where step 1 replaces it in the next call.
        let mut decoder = self.decoder.borrow_mut();
        let decoded = match &input {
            // SAFETY: Decoding runs no script and allocates no cell, so nothing detaches, resizes or frees the
            //         buffer of the input while its bytes are read.
            Some(input) => unsafe { input.with_bytes(|bytes| decoder.decode(bytes, options.stream)) },
            None => decoder.decode(&[], options.stream),
        };
        let decoded =
            decoded.map_err(|DecodingFailed| Exception::TypeError(Utf16String::from_ascii(b"Decoding failed")))?;
        Ok(UsvString::from_utf16_replacing_lone_surrogates(&decoded))
    }
}
