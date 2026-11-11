/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/canvas.html#the-imagebitmaprenderingcontext-interface

use core::cell::{Cell, RefCell};
use core::ffi::c_void;

use ak::Utf16String;
use libgfx_rust::image_frame::ImageFrameHandle;
use libjs_rust::api::{DeferGc, ForeignCellSlot, Gc, Realm, Trace};

use crate::bindings::conversions::CppInterface;
use crate::bindings::interface::{Ref, Wrappable};
use crate::bindings::{CallContext, Exception, WebIdlResult};
use crate::canvas_2d::canvas::Canvas;
use crate::html::image_bitmap::ImageBitmap;

/// https://html.spec.whatwg.org/multipage/canvas.html#concept-imagebitmaprenderingcontext-bitmap-mode
enum BitmapMode {
    /// The output bitmap is a default transparent bitmap, of the size of the canvas.
    Blank,
    /// The output bitmap refers to bitmap data that was acquired via transferFromImageBitmap() — none, for an
    /// ImageBitmap that was created without any.
    Valid(Option<ImageFrameHandle>),
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ImageBitmapRenderingContext {
    canvas_cell: ForeignCellSlot,
    #[gc(untraced)]
    canvas: Canvas,
    #[gc(untraced)]
    bitmap_mode: RefCell<BitmapMode>,
    /// The origin-clean flag of the output bitmap,
    /// https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
    #[gc(untraced)]
    origin_clean: Cell<bool>,
    /// https://html.spec.whatwg.org/multipage/canvas.html#concept-imagebitmaprenderingcontext-alpha
    #[gc(untraced)]
    alpha: bool,
}

impl Wrappable for ImageBitmapRenderingContext {
    fn relevant_realm(&self) -> Option<Gc<Realm>> {
        self.canvas.relevant_realm()
    }

    const OWNS_EXTERNAL_MEMORY: bool = true;

    fn external_memory_size(&self) -> usize {
        // NB: LibGC asks while it sweeps, when nothing borrows the bitmap mode mutably.
        match self.bitmap_mode.try_borrow().as_deref() {
            Ok(BitmapMode::Valid(Some(frame))) => frame.size_in_bytes(),
            _ => 0,
        }
    }
}

impl ImageBitmapRenderingContext {
    /// https://html.spec.whatwg.org/multipage/canvas.html#imagebitmaprenderingcontext-creation-algorithm
    pub(crate) fn new(_: &DeferGc<'_>, canvas: Canvas, alpha: bool) -> Self {
        // 1. Let settings be the result of converting options to the dictionary type ImageBitmapRenderingContextSettings.
        //    (This can throw an exception.)
        // NB: HTMLCanvasElement::get_context() converts the options, and passes alpha.

        // 2. Let context be a new ImageBitmapRenderingContext object.
        // 3. Initialize context's canvas attribute to point to target.
        let canvas_cell = ForeignCellSlot::empty();
        // SAFETY: The canvas is a live cell of the heap that the runtime allocates from.
        unsafe { canvas_cell.set(Some(canvas.cell)) };

        // 4. Set context's output bitmap to the same bitmap as target's bitmap (so that they are shared).
        // 5. Run the steps to set an ImageBitmapRenderingContext's output bitmap with context.
        // 6. Initialize context's alpha flag to true.
        // 7. Process each of the members of settings as follows:
        //    alpha: If false, then set context's alpha flag to false.
        // 8. Return context.
        Self {
            canvas_cell,
            canvas,
            bitmap_mode: RefCell::new(BitmapMode::Blank),
            origin_clean: Cell::new(true),
            alpha,
        }
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-imagebitmaprenderingcontext-canvas
    pub(crate) fn canvas(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        // The canvas attribute must return the value it was initialized to when the object was created.
        Ok(self.canvas.wrapper(cx.realm()))
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#dom-imagebitmaprenderingcontext-transferfromimagebitmap
    pub(crate) fn transfer_from_image_bitmap(
        &self,
        _: &mut CallContext,
        bitmap: Option<Ref<ImageBitmap>>,
    ) -> WebIdlResult<()> {
        // 1. Let bitmapContext be the ImageBitmapRenderingContext object on which the transferFromImageBitmap() method
        //    was called.
        // 2. If bitmap is null, then run the steps to set an ImageBitmapRenderingContext's output bitmap, with
        //    bitmapContext as the context argument and no bitmap argument, then return.
        let Some(bitmap) = bitmap else {
            self.set_output_bitmap(None);
            return Ok(());
        };

        // 3. If the value of bitmap's [[Detached]] internal slot is set to true, then throw an "InvalidStateError"
        //    DOMException.
        if bitmap.is_detached() {
            return Err(Exception::Dom(
                "InvalidStateError",
                Utf16String::from_utf8("The ImageBitmap is detached"),
            ));
        }

        // 4. Run the steps to set an ImageBitmapRenderingContext's output bitmap, with the context argument equal to
        //    bitmapContext, and the bitmap argument referring to bitmap's underlying bitmap data.
        self.set_output_bitmap(Some((bitmap.frame(), bitmap.origin_clean())));

        // 5. Set the value of bitmap's [[Detached]] internal slot to true.
        // 6. Unset bitmap's bitmap data.
        bitmap.close_bitmap();
        Ok(())
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#set-an-imagebitmaprenderingcontext's-output-bitmap
    fn set_output_bitmap(&self, bitmap: Option<(Option<ImageFrameHandle>, bool)>) {
        match bitmap {
            // 1. If a bitmap argument was not provided:
            None => {
                // 1. Set context's bitmap mode to blank.
                // 2. Let canvas be the canvas element to which context is bound.
                // 3. Set context's output bitmap to be transparent black with a natural width equal to the numeric
                //    value of canvas's width attribute and a natural height equal to the numeric value of canvas's
                //    height attribute, those values being interpreted in CSS pixels.
                // NB: The canvas makes that bitmap when something reads it, at the size its attributes have then.
                self.bitmap_mode.replace(BitmapMode::Blank);

                // 4. Set the output bitmap's origin-clean flag to true.
                self.origin_clean.set(true);
            }
            // 2. If a bitmap argument was provided:
            Some((frame, origin_clean)) => {
                // 1. Set context's bitmap mode to valid.
                // 2. Set context's output bitmap to refer to the same underlying bitmap data as bitmap, without making
                //    a copy.
                //    The origin-clean flag of bitmap is included in the bitmap data to be referenced by context's
                //    output bitmap.
                self.bitmap_mode.replace(BitmapMode::Valid(frame));
                self.origin_clean.set(origin_clean);
            }
        }
        self.canvas.did_change_content();
    }

    /// The frame of the output bitmap, which stays valid until the output bitmap changes; null in blank mode, and for
    /// an output bitmap without bitmap data.
    pub(crate) fn raw_frame(&self) -> *const c_void {
        match &*self.bitmap_mode.borrow() {
            BitmapMode::Valid(Some(frame)) => frame.as_raw(),
            _ => core::ptr::null(),
        }
    }

    /// Whether the bitmap mode is blank.
    pub(crate) fn is_blank(&self) -> bool {
        matches!(*self.bitmap_mode.borrow(), BitmapMode::Blank)
    }

    pub(crate) fn origin_clean(&self) -> bool {
        self.origin_clean.get()
    }

    pub(crate) fn alpha(&self) -> bool {
        self.alpha
    }
}
