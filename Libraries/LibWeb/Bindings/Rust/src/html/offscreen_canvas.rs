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
        fields: &mut TransferredFields,
    ) -> RawNativeFunctionResult;
    fn web_offscreen_canvas_create_from_transfer(
        realm: *mut c_void,
        fields: &TransferredFields,
    ) -> RawNativeFunctionResult;
}

/// What the transfer steps of Libraries/LibWeb/HTML/OffscreenCanvas.cpp set the fields of the data holder to.
#[repr(C)]
#[derive(Default)]
struct TransferredFields {
    width: u64,
    height: u64,
    has_placeholder_canvas: bool,
    placeholder_canvas_id: u64,
    placeholder_secret: u64,
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
    let mut fields = TransferredFields::default();
    // SAFETY: The realm and the object are live, and the call writes to the fields.
    unsafe {
        web_offscreen_canvas_transfer_steps(data_holder.realm().as_ptr().cast(), object.as_ptr().cast(), &mut fields)
    }
    .into_completion()?;

    // 6. Set dataHolder.[[Width]] to width and dataHolder.[[Height]] to height.
    data_holder.write(&fields.width);
    data_holder.write(&fields.height);

    // FIXME: 7. Set dataHolder.[[Language]] to language and dataHolder.[[Direction]] to direction.

    // 8. Set dataHolder.[[PlaceholderCanvas]] to be a weak reference to value's placeholder canvas element, if value
    //    has one, or null if it does not.
    data_holder.write_bool(fields.has_placeholder_canvas);
    if fields.has_placeholder_canvas {
        data_holder.write(&fields.placeholder_canvas_id);
        data_holder.write(&fields.placeholder_secret);
    }
    Ok(())
}

/// https://html.spec.whatwg.org/multipage/canvas.html#the-offscreencanvas-interface:transfer-receiving-steps
pub(crate) fn create_from_transfer(data_holder: &mut TransferDataReader) -> WebIdlResult<Value> {
    let mut fields = TransferredFields {
        width: data_holder.read()?,
        height: data_holder.read()?,
        has_placeholder_canvas: data_holder.read_bool()?,
        ..TransferredFields::default()
    };
    if fields.has_placeholder_canvas {
        fields.placeholder_canvas_id = data_holder.read()?;
        fields.placeholder_secret = data_holder.read()?;
    }
    // SAFETY: The realm is live, and the call borrows the fields.
    Ok(
        unsafe { web_offscreen_canvas_create_from_transfer(data_holder.realm().as_ptr().cast(), &fields) }
            .into_completion()?,
    )
}
