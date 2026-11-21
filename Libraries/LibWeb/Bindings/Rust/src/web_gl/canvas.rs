/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a WebGL context asks of the rest of LibWeb, which is C++: its canvas and the sources of texture images.
//! Libraries/LibWeb/WebGL/WebGLRenderingContextBase.cpp defines these functions.

use core::ffi::c_void;
use core::ptr::NonNull;

use libgfx_rust::image_frame::ImageFrameHandle;
use libjs_rust::api::{Gc, Object, Realm};

use crate::bindings::conversions::CppInterface;
use crate::bindings::event_target::CppEventTarget;
use crate::canvas_2d::host::TransportAbi;
use crate::compositor_connection::CompositorConnection;

/// The sources of texture images whose frame LibWeb reads, in the order of the C++ enum of the same name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum TexImageSourceKind {
    HTMLImageElement,
    HTMLCanvasElement,
    OffscreenCanvas,
}

unsafe extern "C" {
    fn web_webgl_canvas_did_change_content(canvas_host: *mut c_void);
    fn web_webgl_canvas_did_create_backing_storage(canvas_host: *mut c_void);
    fn web_webgl_canvas_event_target(canvas_host: *mut c_void) -> *mut c_void;
    fn web_webgl_canvas_bitmap_size(canvas_host: *mut c_void, out_width: *mut i32, out_height: *mut i32);
    fn web_webgl_canvas_wrapper(canvas: *mut c_void, canvas_is_offscreen: bool, realm: *mut c_void) -> *mut c_void;
    fn web_webgl_canvas_relevant_realm(canvas_host: *mut c_void) -> *mut c_void;
    safe fn web_webgl_expose_experimental_interfaces() -> bool;
    fn web_webgl_texture_image_frame(source_wrapper: *mut c_void, source_kind: TexImageSourceKind) -> *mut c_void;
    fn web_canvas_2d_canvas_create_transport(canvas_host: *mut c_void, out_transport: *mut TransportAbi) -> bool;
}

#[derive(Clone, Copy)]
pub(crate) struct Canvas {
    /// The cell of the canvas, which the context keeps alive through a slot of its own.
    pub(crate) cell: NonNull<c_void>,
    pub(crate) host: NonNull<c_void>,
    pub(crate) is_offscreen: bool,
}

// SAFETY (of every call below): The context that holds the canvas keeps its cell alive, and the host is the same
// object.
impl Canvas {
    pub(crate) fn did_change_content(self) {
        unsafe { web_webgl_canvas_did_change_content(self.host.as_ptr()) };
    }

    pub(crate) fn did_create_backing_storage(self) {
        unsafe { web_webgl_canvas_did_create_backing_storage(self.host.as_ptr()) };
    }

    pub(crate) fn event_target(self) -> CppEventTarget {
        // SAFETY: The event target of a canvas is the canvas.
        unsafe { CppEventTarget::from_raw(web_webgl_canvas_event_target(self.host.as_ptr())) }
    }

    pub(crate) fn compositor_connection(self) -> Option<CompositorConnection> {
        let mut transport = TransportAbi::default();
        // SAFETY: LibWeb hands over a reference to the connection where it answers true.
        unsafe { web_canvas_2d_canvas_create_transport(self.host.as_ptr(), &raw mut transport) }.then(|| {
            let connection = NonNull::new(transport.connection).expect("LibWeb passes the connection");
            // SAFETY: The caller takes over the reference to the connection, which outlives the value.
            unsafe { CompositorConnection::lent(connection) }
        })
    }

    pub(crate) fn bitmap_size(self) -> (i32, i32) {
        let (mut width, mut height) = (0, 0);
        unsafe { web_webgl_canvas_bitmap_size(self.host.as_ptr(), &raw mut width, &raw mut height) };
        (width, height)
    }

    pub(crate) fn wrapper(self, realm: Gc<Realm>) -> CppInterface {
        let wrapper = unsafe { web_webgl_canvas_wrapper(self.cell.as_ptr(), self.is_offscreen, realm.as_ptr().cast()) };
        let wrapper = NonNull::new(wrapper.cast::<Object>()).expect("LibWeb returns the wrapper of the canvas");
        // SAFETY: LibWeb returns a live wrapper of an HTMLCanvasElement or an OffscreenCanvas.
        unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
    }

    /// The realm of the relevant global object of the canvas, once that global object has a wrapper.
    pub(crate) fn relevant_realm(self) -> Option<Gc<Realm>> {
        let realm = unsafe { web_webgl_canvas_relevant_realm(self.host.as_ptr()) };
        // SAFETY: LibWeb returns a live realm, or null.
        NonNull::new(realm.cast::<Realm>()).map(|realm| unsafe { Gc::from_non_null(realm) })
    }
}

pub(crate) fn expose_experimental_interfaces() -> bool {
    web_webgl_expose_experimental_interfaces()
}

pub(crate) fn texture_image_frame(source: CppInterface, source_kind: TexImageSourceKind) -> Option<ImageFrameHandle> {
    // SAFETY: The wrapper is live and of the interface that the kind names, and LibWeb hands over a frame that it
    //         allocated with new, or null.
    let frame = unsafe { web_webgl_texture_image_frame(source.wrapper().as_ptr().cast(), source_kind) };
    (!frame.is_null()).then(|| unsafe { ImageFrameHandle::adopt(frame) })
}
