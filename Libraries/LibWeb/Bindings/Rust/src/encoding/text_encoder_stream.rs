/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! TextEncoderStream, https://encoding.spec.whatwg.org/#interface-textencoderstream

use core::cell::Cell;

use ak::Utf16String;
use libjs_rust::api::{EmbedderClass, EmbedderValue, Trace, Value};

use crate::bindings::conversions::{create_uint8_array, to_dom_string};
use crate::bindings::interface::Ref;
use crate::bindings::interface::{Wrappable, with_temporary_execution_context};
use crate::bindings::{CallContext, WebIdlResult};
use crate::streams::generic_transform_stream::{TransformSteps, create_and_set_up};
use crate::streams::readable_stream::ReadableStream;
use crate::streams::transform_stream::TransformStream;
use crate::streams::writable_stream::WritableStream;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct TextEncoderStream {
    // https://streams.spec.whatwg.org/#generictransformstream-transform
    transform: Ref<TransformStream>,
}

impl Wrappable for TextEncoderStream {}

#[expect(
    clippy::unnecessary_wraps,
    reason = "the bindings call every method of an implementation for its WebIDL result, whether or not it can fail"
)]
impl TextEncoderStream {
    // https://encoding.spec.whatwg.org/#dom-textencoderstream
    pub(crate) fn construct(cx: &mut CallContext) -> WebIdlResult<Self> {
        // 1. Set this's encoder to an instance of the UTF-8 encoder.
        let encoder = Encoder {
            leading_surrogate: Cell::new(None),
        };

        // 2. Let transformAlgorithm be an algorithm which takes a chunk argument and runs the encode and enqueue a
        //    chunk algorithm with this and chunk.
        // 3. Let flushAlgorithm be an algorithm which runs the encode and flush algorithm with this.
        // 4. Let transformStream be a new TransformStream.
        // 5. Set up transformStream with transformAlgorithm set to transformAlgorithm and flushAlgorithm set to
        //    flushAlgorithm.
        // 6. Set this's transform to transformStream.
        Ok(Self {
            transform: create_and_set_up(cx, encoder),
        })
    }

    // https://encoding.spec.whatwg.org/#dom-textencoder-encoding
    pub(crate) fn encoding(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        // The encoding getter steps are to return "utf-8".
        Ok(Utf16String::from_ascii(b"utf-8"))
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

/// The UTF-8 encoder of a TextEncoderStream, with its leading surrogate, which the algorithms of the stream's
/// transform run on.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
struct Encoder {
    // https://encoding.spec.whatwg.org/#textencoderstream-pending-high-surrogate
    leading_surrogate: Cell<Option<u16>>,
}

static ENCODER_CLASS: EmbedderClass<Encoder> = EmbedderClass::new("TextEncoderStreamEncoder");

impl EmbedderValue for Encoder {
    const CLASS: &'static EmbedderClass<Self> = &ENCODER_CLASS;
}

/// The bytes of U+FFFD (�) in UTF-8.
const REPLACEMENT_CHARACTER_UTF8_BYTES: [u8; 3] = [0xEF, 0xBF, 0xBD];

impl Encoder {
    // https://encoding.spec.whatwg.org/#convert-code-unit-to-scalar-value
    fn convert_code_unit_to_scalar_value(&self, item: u16, input_position: &mut usize) -> Option<char> {
        // 1. If encoder's leading surrogate is non-null:
        if let Some(leading_surrogate) = self.leading_surrogate.get() {
            // 1. Let leadingSurrogate be encoder's leading surrogate.
            // 2. Set encoder's leading surrogate to null.
            self.leading_surrogate.set(None);

            // 3. If item is a trailing surrogate, then return a scalar value from surrogates given leadingSurrogate
            //    and item.
            if is_trailing_surrogate(item) {
                return Some(scalar_value_from_surrogates(leading_surrogate, item));
            }

            // 4. Restore item to input.
            *input_position -= 1;

            // 5. Return U+FFFD (�).
            return Some(char::REPLACEMENT_CHARACTER);
        }

        // 2. If item is a leading surrogate, then set encoder's leading surrogate to item and return continue.
        if is_leading_surrogate(item) {
            self.leading_surrogate.set(Some(item));
            return None;
        }

        // 3. If item is a trailing surrogate, then return U+FFFD (�).
        if is_trailing_surrogate(item) {
            return Some(char::REPLACEMENT_CHARACTER);
        }

        // 4. Return item.
        Some(char::from_u32(u32::from(item)).expect("a code unit that is not a surrogate is a scalar value"))
    }
}

fn is_leading_surrogate(code_unit: u16) -> bool {
    (0xD800..=0xDBFF).contains(&code_unit)
}

fn is_trailing_surrogate(code_unit: u16) -> bool {
    (0xDC00..=0xDFFF).contains(&code_unit)
}

// https://encoding.spec.whatwg.org/#scalar-value-from-surrogates
fn scalar_value_from_surrogates(leading: u16, trailing: u16) -> char {
    // To obtain a scalar value from surrogates, given a leading surrogate leading and a trailing surrogate trailing,
    // return 0x10000 + ((leading − 0xD800) << 10) + (trailing − 0xDC00).
    let scalar_value = 0x10000 + ((u32::from(leading) - 0xD800) << 10) + (u32::from(trailing) - 0xDC00);
    char::from_u32(scalar_value).expect("surrogates make a scalar value")
}

impl TransformSteps for Encoder {
    // https://encoding.spec.whatwg.org/#encode-and-enqueue-a-chunk
    fn transform(&self, cx: &CallContext, transform: Ref<TransformStream>, chunk: Value) -> WebIdlResult<()> {
        // 1. Let input be the result of converting chunk to a DOMString.
        let input = with_temporary_execution_context(cx.realm(), || to_dom_string(cx.vm(), chunk))?;

        // 2. Convert input to an I/O queue of code units.
        let input = input.to_utf16();
        let mut input_position = 0;

        // 3. Let output be the I/O queue of bytes « end-of-queue ».
        let mut output = Vec::new();

        // 4. While true:
        loop {
            // 1. Let item be the result of reading from input.
            let Some(&item) = input.get(input_position) else {
                // 2. If item is end-of-queue:
                // 1. Convert output into a byte sequence.
                // 2. If output is not empty:
                if !output.is_empty() {
                    // 1. Let chunk be the result of creating a Uint8Array object given output and encoder's relevant
                    //    realm.
                    let chunk = create_uint8_array(cx, &output)?;

                    // 2. Enqueue chunk into encoder's transform.
                    TransformStream::enqueue(transform, cx.vm(), Value::from_object(chunk));
                }

                // 3. Return.
                return Ok(());
            };
            input_position += 1;

            // 3. Let result be the result of executing the convert code unit to scalar value algorithm with encoder,
            //    item and input.
            // 4. If result is not continue, then process an item with result, encoder's encoder, input, output, and
            //    "fatal".
            if let Some(result) = self.convert_code_unit_to_scalar_value(item, &mut input_position) {
                let mut bytes = [0; 4];
                output.extend_from_slice(result.encode_utf8(&mut bytes).as_bytes());
            }
        }
    }

    // https://encoding.spec.whatwg.org/#encode-and-flush
    fn flush(&self, cx: &CallContext, transform: Ref<TransformStream>) -> WebIdlResult<()> {
        // 1. If encoder's leading surrogate is non-null:
        if self.leading_surrogate.get().is_some() {
            // 1. Let chunk be the result of creating a Uint8Array object given « 0xEF, 0xBF, 0xBD » and encoder's
            //    relevant realm.
            // NB: This is U+FFFD (�) in UTF-8 bytes.
            let chunk = create_uint8_array(cx, &REPLACEMENT_CHARACTER_UTF8_BYTES)?;

            // 2. Enqueue chunk into encoder's transform.
            TransformStream::enqueue(transform, cx.vm(), Value::from_object(chunk));
        }
        Ok(())
    }
}
