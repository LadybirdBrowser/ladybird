/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What StructuredSerializeWithTransfer asks of OffscreenCanvas, which LibWeb implements in C++, in
//! Libraries/LibWeb/HTML/OffscreenCanvas.cpp.

use core::ffi::c_void;

use libjs_rust::api::{Gc, Object, RawNativeFunctionResult, Value};

use crate::bindings::WebIdlResult;
use crate::bindings::conversions::implements_cpp_interface;
use crate::bindings::structured_data::{TransferDataReader, TransferDataWriter};

unsafe extern "C" {
    fn web_offscreen_canvas_interface_name() -> u16;
    fn web_offscreen_canvas_is_detached(object: *mut c_void) -> bool;
    fn web_offscreen_canvas_transfer_steps(
        realm: *mut c_void,
        object: *mut c_void,
        data_holder: *mut c_void,
    ) -> RawNativeFunctionResult;
    fn web_offscreen_canvas_create_from_transfer(
        realm: *mut c_void,
        data_holder: *mut c_void,
    ) -> RawNativeFunctionResult;
}

/// Web::Bindings::InterfaceName of OffscreenCanvas.
pub(crate) fn interface_name() -> u16 {
    // SAFETY: The call has no arguments.
    unsafe { web_offscreen_canvas_interface_name() }
}

pub(crate) fn is_offscreen_canvas(object: Gc<Object>) -> bool {
    implements_cpp_interface(object, interface_name())
}

/// The [[Detached]] internal slot of the OffscreenCanvas, which is its detached context mode.
pub(crate) fn is_detached(object: Gc<Object>) -> bool {
    // SAFETY: The object is a live OffscreenCanvas.
    unsafe { web_offscreen_canvas_is_detached(object.as_ptr().cast()) }
}

/// https://html.spec.whatwg.org/multipage/canvas.html#the-offscreencanvas-interface:transfer-steps
pub(crate) fn transfer_steps(object: Gc<Object>, data_holder: &mut TransferDataWriter) -> WebIdlResult<()> {
    // SAFETY: The realm and the object are live, and the data holder is lent for the steps.
    unsafe {
        web_offscreen_canvas_transfer_steps(
            data_holder.realm().as_ptr().cast(),
            object.as_ptr().cast(),
            data_holder.as_raw(),
        )
    }
    .into_completion()?;
    Ok(())
}

/// https://html.spec.whatwg.org/multipage/canvas.html#the-offscreencanvas-interface:transfer-receiving-steps
pub(crate) fn create_from_transfer(data_holder: &mut TransferDataReader) -> WebIdlResult<Value> {
    // SAFETY: The realm is live, and the data holder is lent for the steps.
    Ok(
        unsafe { web_offscreen_canvas_create_from_transfer(data_holder.realm().as_ptr().cast(), data_holder.as_raw()) }
            .into_completion()?,
    )
}
