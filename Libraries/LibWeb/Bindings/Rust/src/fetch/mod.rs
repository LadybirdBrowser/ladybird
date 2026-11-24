/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The Fetch Standard, https://fetch.spec.whatwg.org/

pub(crate) mod body_init;
pub(crate) mod infrastructure;

use core::ffi::c_void;

use libjs_rust::api::{Gc, Realm, Vm};

use crate::bindings::interface::{InterfaceCell, cell_into_abi};
use crate::bindings::{CallContext, MustNotThrow};
use crate::encoding::text_decoder_stream::TextDecoderStream;
use crate::streams::algorithms::{CancelAlgorithm, PullAlgorithm, StartAlgorithm};
use crate::streams::readable_stream::{ReadableStream, create_readable_stream, readable_stream_close};

/// https://fetch.spec.whatwg.org/#dom-body-textstream
#[unsafe(no_mangle)]
pub extern "C" fn web_rust_body_text_stream(vm: &Vm, realm: Gc<Realm>, stream: Option<&InterfaceCell>) -> *mut c_void {
    let cx = CallContext::new(vm, realm);

    // 1. If this is unusable, then throw a TypeError.
    // AD-HOC: This is implemented in the C++ caller

    // 2. If this's body is null:
    let Some(stream) = stream else {
        // 1. Let emptyStream be a new ReadableStream in this's relevant realm.
        // 2. Set up emptyStream.
        let empty_stream = create_readable_stream(
            &cx,
            StartAlgorithm::ReturnsUndefined,
            PullAlgorithm::ReturnsResolvedUndefined,
            CancelAlgorithm::ReturnsResolvedUndefined,
            None,
            None,
        )
        .must();

        // 3. Close emptyStream.
        readable_stream_close(vm, empty_stream);

        // 4. Return emptyStream.
        return cell_into_abi(empty_stream);
    };

    // 3. Let stream be this's body's stream.
    let stream = stream.object::<ReadableStream>();

    // 4. Let decoder be a new TextDecoderStream object in this's relevant realm.
    // 5. Set up decoder with UTF-8.
    let decoder = TextDecoderStream::set_up(&cx, encoding_rs::UTF_8, false, false);

    // 6. Return the result of stream, piped through decoder.
    cell_into_abi(ReadableStream::piped_through(
        stream,
        vm,
        decoder.transform(),
        false,
        false,
        false,
        None,
    ))
}
