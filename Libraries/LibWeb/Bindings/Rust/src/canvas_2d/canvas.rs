/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a 2D context asks of the rest of LibWeb, which is C++: its canvas, the CSS values that are resolved against the
//! canvas, and the image sources that are implemented in C++. Libraries/LibWeb/HTML/Canvas/Canvas2DContextBase.cpp
//! defines these functions.

use core::ffi::c_void;
use core::ptr::NonNull;

use ak::Utf16String;
use libcompositing_rust::display_list::canvas_2d_recorder::ColorSpace;
use libcompositing_rust::filter_bytes::FfiFilterFunction;
use libgfx_rust::Color;
use libgfx_rust::font::FontCascadeListHandle;
use libgfx_rust::image_frame::ImageFrameHandle;
use libjs_rust::api::{Gc, Object, Realm};

use super::drawing_state::CanvasColor;
use super::host::CppTransport;
use crate::bindings::conversions::CppInterface;
use crate::html::predefined_color_space::PredefinedColorSpace;

unsafe extern "C" {
    fn web_canvas_2d_canvas_did_change_content(canvas_host: *mut c_void);
    fn web_canvas_2d_canvas_did_create_backing_storage(canvas_host: *mut c_void);
    fn web_canvas_2d_canvas_create_transport(canvas_host: *mut c_void) -> *mut c_void;
    fn web_canvas_2d_canvas_wrapper(canvas: *mut c_void, canvas_is_offscreen: bool, realm: *mut c_void) -> *mut c_void;
    fn web_canvas_2d_canvas_relevant_realm(canvas_host: *mut c_void) -> *mut c_void;
    fn web_canvas_2d_canvas_font_environment_generation(canvas_host: *mut c_void) -> u64;
    fn web_canvas_2d_canvas_resolve_font(
        canvas: *mut c_void,
        canvas_is_offscreen: bool,
        font: usize,
        out_font: *mut ResolvedFontAbi,
    ) -> bool;
    fn web_canvas_2d_canvas_resolve_color(
        canvas_host: *mut c_void,
        color: *const c_void,
        font: FontMetricsSource,
        color_space: ColorSpace,
        out_color: *mut Color,
        out_color_in_color_space: *mut Color,
    );
    fn web_canvas_2d_canvas_resolve_length(
        canvas_host: *mut c_void,
        length: *const c_void,
        font: FontMetricsSource,
    ) -> f32;
    fn web_canvas_2d_canvas_parse_filter(
        canvas_host: *mut c_void,
        filter: usize,
        font: FontMetricsSource,
        functions: *mut c_void,
        append_function: unsafe extern "C" fn(*mut c_void, *const FfiFilterFunction),
    ) -> bool;
    fn web_canvas_2d_parse_color(color: usize) -> *const c_void;
    fn web_canvas_2d_parse_length(length: usize, out_serialized: *mut usize) -> *const c_void;
    fn web_canvas_2d_style_value_ref(style_value: *const c_void);
    fn web_canvas_2d_style_value_unref(style_value: *const c_void);
    fn web_canvas_2d_image_frame_from_pixels(
        first_row: *const u8,
        pitch: usize,
        width: i32,
        height: i32,
        color_space: ColorSpace,
    ) -> *mut c_void;

    fn web_canvas_2d_check_usability_of_image(
        source_wrapper: *mut c_void,
        source_kind: CanvasImageSourceKind,
        out_message: *mut usize,
    ) -> u8;
    fn web_canvas_2d_image_source_size(
        source_wrapper: *mut c_void,
        source_kind: CanvasImageSourceKind,
        out_width: *mut i32,
        out_height: *mut i32,
    );
    fn web_canvas_2d_image_is_not_origin_clean(source_wrapper: *mut c_void, source_kind: CanvasImageSourceKind)
    -> bool;
    fn web_canvas_2d_image_source_canvas_id(
        source_wrapper: *mut c_void,
        source_kind: CanvasImageSourceKind,
        out_canvas_id: *mut u64,
        out_source_is_2d: *mut bool,
    ) -> bool;
    fn web_canvas_2d_image_source_frame(source_wrapper: *mut c_void, source_kind: CanvasImageSourceKind)
    -> *mut c_void;
    fn web_canvas_2d_capture_video_frame(source_wrapper: *mut c_void, out_frame: *mut CapturedVideoFrameAbi) -> bool;
    fn web_canvas_2d_video_transport(source_wrapper: *mut c_void) -> *mut c_void;
    fn web_canvas_2d_video_frame_hold_unref(hold: *mut c_void);
}

/// What a CSS value of a context is resolved with besides its canvas: the font of the context, if it has one yet.
/// `FontMetricsSource` of Canvas2DContextBase.cpp mirrors it.
#[derive(Clone, Copy)]
#[repr(C)]
pub(crate) struct FontMetricsSource {
    /// The Gfx::FontCascadeList of the font, or null for the font that a context starts with.
    cascade: *const c_void,
    /// The size of the font, as the raw value of a CSSPixels.
    font_size: i32,
}

impl FontMetricsSource {
    pub(crate) fn of(font: Option<&ResolvedFont>) -> Self {
        match font {
            Some(font) => Self {
                cascade: font.cascade.as_raw(),
                font_size: font.font_size,
            },
            None => Self {
                cascade: core::ptr::null(),
                font_size: 0,
            },
        }
    }
}

/// `ResolvedFont` of Canvas2DContextBase.cpp.
#[repr(C)]
struct ResolvedFontAbi {
    /// A Gfx::FontCascadeList with a reference that the caller takes over.
    cascade: *const c_void,
    environment_generation: u64,
    font_size: i32,
    /// An AK::Utf16String that the caller takes over.
    serialized: usize,
}

/// The font of a context: the cascade of fonts that its text is drawn with, the font environment that chose them, and
/// what the font attribute returns.
#[derive(Clone)]
pub(crate) struct ResolvedFont {
    pub(crate) cascade: FontCascadeListHandle,
    pub(crate) environment_generation: u64,
    /// The size of the font, as the raw value of a CSSPixels.
    pub(crate) font_size: i32,
    pub(crate) serialized: Utf16String,
}

/// A CSS::StyleValue of LibWeb, one reference to which the value owns.
pub(crate) struct StyleValue(NonNull<c_void>);

impl StyleValue {
    /// # Safety
    ///
    /// `style_value` is a CSS::StyleValue with a reference that the value takes over, or null.
    unsafe fn adopt(style_value: *const c_void) -> Option<Self> {
        NonNull::new(style_value.cast_mut()).map(Self)
    }
}

impl Clone for StyleValue {
    fn clone(&self) -> Self {
        // SAFETY: The value owns a reference to the style value, so it is live.
        unsafe { web_canvas_2d_style_value_ref(self.0.as_ptr()) };
        Self(self.0)
    }
}

impl Drop for StyleValue {
    fn drop(&mut self) {
        // SAFETY: The value owns a reference to the style value.
        unsafe { web_canvas_2d_style_value_unref(self.0.as_ptr()) };
    }
}

/// The sources of images that are implemented in C++, in the order of the C++ enum of the same name.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum CanvasImageSourceKind {
    HTMLImageElement,
    SVGImageElement,
    HTMLVideoElement,
    HTMLCanvasElement,
    OffscreenCanvas,
}

/// The values of the C++ enum `CanvasImageSourceUsability`, and an exception.
const USABILITY_GOOD: u8 = 0;
const USABILITY_BAD: u8 = 1;
const USABILITY_INVALID_STATE: u8 = 2;

/// https://html.spec.whatwg.org/multipage/canvas.html#check-the-usability-of-the-image-argument
pub(crate) enum Usability {
    Good,
    Bad,
    /// The check threw an "InvalidStateError" DOMException with the message.
    InvalidState(Utf16String),
}

/// `CapturedVideoFrame` of Canvas2DContextBase.cpp.
#[repr(C)]
struct CapturedVideoFrameAbi {
    hold: *mut c_void,
    sink_handle: u64,
    generation: u64,
}

/// A frame of a video, which the hold keeps until the Compositor has drawn it.
pub(crate) struct CapturedVideoFrame {
    hold: NonNull<c_void>,
    pub(crate) sink_handle: u64,
    pub(crate) generation: u64,
}

impl CapturedVideoFrame {
    /// The Compositing::VideoFrameHold, which the value keeps alive.
    pub(crate) fn hold(&self) -> NonNull<c_void> {
        self.hold
    }
}

impl Drop for CapturedVideoFrame {
    fn drop(&mut self) {
        // SAFETY: The value owns a reference to the hold.
        unsafe { web_canvas_2d_video_frame_hold_unref(self.hold.as_ptr()) };
    }
}

#[derive(Clone, Copy)]
pub(crate) struct Canvas {
    /// The cell of the canvas, which the context keeps alive through a slot of its own.
    pub(crate) cell: NonNull<c_void>,
    pub(crate) host: NonNull<c_void>,
    pub(crate) is_offscreen: bool,
}

// SAFETY (of every call below): The context that holds the canvas keeps its cell alive, and the host is the same
// object. Strings are lent for the call, and the style values are live.
impl Canvas {
    pub(crate) fn did_change_content(self) {
        unsafe { web_canvas_2d_canvas_did_change_content(self.host.as_ptr()) };
    }

    pub(crate) fn did_create_backing_storage(self) {
        unsafe { web_canvas_2d_canvas_did_create_backing_storage(self.host.as_ptr()) };
    }

    /// A transport to a new context in the Compositor, if the page of the canvas has a Compositor.
    pub(crate) fn create_transport(self) -> Option<CppTransport> {
        let transport = unsafe { web_canvas_2d_canvas_create_transport(self.host.as_ptr()) };
        // SAFETY: LibWeb hands over a reference to the transport.
        NonNull::new(transport).map(|transport| unsafe { CppTransport::from_raw(transport) })
    }

    pub(crate) fn wrapper(self, realm: Gc<Realm>) -> CppInterface {
        let wrapper =
            unsafe { web_canvas_2d_canvas_wrapper(self.cell.as_ptr(), self.is_offscreen, realm.as_ptr().cast()) };
        let wrapper = NonNull::new(wrapper.cast::<Object>()).expect("LibWeb returns the wrapper of the canvas");
        // SAFETY: LibWeb returns a live wrapper of an HTMLCanvasElement or an OffscreenCanvas.
        unsafe { CppInterface::from_wrapper(Gc::from_non_null(wrapper)) }
    }

    /// The realm of the relevant global object of the canvas, once that global object has a wrapper.
    pub(crate) fn relevant_realm(self) -> Option<Gc<Realm>> {
        let realm = unsafe { web_canvas_2d_canvas_relevant_realm(self.host.as_ptr()) };
        // SAFETY: LibWeb returns a live realm, or null.
        NonNull::new(realm.cast::<Realm>()).map(|realm| unsafe { Gc::from_non_null(realm) })
    }

    /// The generation of the fonts that the canvas can use, which changes when a font loads.
    pub(crate) fn font_environment_generation(self) -> u64 {
        unsafe { web_canvas_2d_canvas_font_environment_generation(self.host.as_ptr()) }
    }

    /// The font that a value of the font attribute describes, resolved against the canvas, unless the value is not a
    /// CSS font.
    pub(crate) fn resolve_font(self, font: &Utf16String) -> Option<ResolvedFont> {
        let mut resolved = ResolvedFontAbi {
            cascade: core::ptr::null(),
            environment_generation: 0,
            font_size: 0,
            serialized: 0,
        };
        let is_font = unsafe {
            web_canvas_2d_canvas_resolve_font(
                self.cell.as_ptr(),
                self.is_offscreen,
                font.raw_identity(),
                &raw mut resolved,
            )
        };
        if !is_font {
            return None;
        }
        Some(ResolvedFont {
            // SAFETY: LibWeb hands over a reference to the cascade.
            cascade: unsafe { FontCascadeListHandle::adopt(resolved.cascade) },
            environment_generation: resolved.environment_generation,
            font_size: resolved.font_size,
            // SAFETY: LibWeb hands over the string.
            serialized: unsafe { Utf16String::from_raw_owned(resolved.serialized) },
        })
    }

    /// The used color of a parsed CSS color, resolved against the canvas, in sRGB and in the color space.
    pub(crate) fn resolve_color(
        self,
        color: &StyleValue,
        font: FontMetricsSource,
        color_space: PredefinedColorSpace,
    ) -> CanvasColor {
        let mut srgb = Color::default();
        let mut in_color_space = Color::default();
        unsafe {
            web_canvas_2d_canvas_resolve_color(
                self.host.as_ptr(),
                color.0.as_ptr(),
                font,
                color_space.into(),
                &raw mut srgb,
                &raw mut in_color_space,
            );
        };
        CanvasColor { srgb, in_color_space }
    }

    /// A parsed CSS length in CSS pixels, resolved against the canvas.
    pub(crate) fn resolve_length(self, length: &StyleValue, font: FontMetricsSource) -> f32 {
        unsafe { web_canvas_2d_canvas_resolve_length(self.host.as_ptr(), length.0.as_ptr(), font) }
    }

    /// The functions of a <filter-value-list>, with their lengths and colors resolved against the canvas, unless the
    /// value is not one.
    pub(crate) fn parse_filter(self, filter: &Utf16String, font: FontMetricsSource) -> Option<Vec<FfiFilterFunction>> {
        unsafe extern "C" fn append_function(functions: *mut c_void, function: *const FfiFilterFunction) {
            // SAFETY: The functions are the vector below, and LibWeb lends the function for the call.
            unsafe { (*functions.cast::<Vec<FfiFilterFunction>>()).push(*function) };
        }
        let mut functions = Vec::new();
        let is_filter = unsafe {
            web_canvas_2d_canvas_parse_filter(
                self.host.as_ptr(),
                filter.raw_identity(),
                font,
                (&raw mut functions).cast(),
                append_function,
            )
        };
        is_filter.then_some(functions)
    }
}

/// A CSS <color>, parsed without a canvas to resolve it against, unless the string is not one.
pub(crate) fn parse_color(color: &Utf16String) -> Option<StyleValue> {
    // SAFETY: The string is lent for the call, and LibWeb hands over a reference to the style value, or null.
    unsafe { StyleValue::adopt(web_canvas_2d_parse_color(color.raw_identity())) }
}

/// A CSS <length>, parsed without a canvas to resolve it against, and its serialization, unless the string is not
/// one.
pub(crate) fn parse_length(length: &Utf16String) -> Option<(StyleValue, Utf16String)> {
    let mut serialized = 0;
    // SAFETY: The string is lent for the call, and LibWeb hands over a reference to the style value, or null.
    let style_value =
        unsafe { StyleValue::adopt(web_canvas_2d_parse_length(length.raw_identity(), &raw mut serialized)) }?;
    // SAFETY: LibWeb hands over the serialization with the style value.
    Some((style_value, unsafe { Utf16String::from_raw_owned(serialized) }))
}

/// A frame of a copy of `height` rows of `width` RGBA8888 pixels that are unpremultiplied, `pitch` bytes apart and in
/// the color space, or None if there is no memory for it.
///
/// # Safety
///
/// The rows must be readable, and no script may change them during the call.
pub(crate) unsafe fn image_frame_from_pixels(
    first_row: NonNull<u8>,
    pitch: usize,
    width: i32,
    height: i32,
    color_space: ColorSpace,
) -> Option<ImageFrameHandle> {
    // SAFETY: The caller passes readable rows.
    let frame = unsafe { web_canvas_2d_image_frame_from_pixels(first_row.as_ptr(), pitch, width, height, color_space) };
    // SAFETY: LibWeb hands over a frame that it allocated with new, or null.
    (!frame.is_null()).then(|| unsafe { ImageFrameHandle::adopt(frame) })
}

// SAFETY (of every call below): The wrapper is live and of the interface that the kind names.
impl CanvasImageSourceKind {
    /// https://html.spec.whatwg.org/multipage/canvas.html#check-the-usability-of-the-image-argument
    pub(crate) fn check_usability(self, source: CppInterface) -> Usability {
        let mut message = 0;
        let usability =
            unsafe { web_canvas_2d_check_usability_of_image(source.wrapper().as_ptr().cast(), self, &raw mut message) };
        match usability {
            USABILITY_GOOD => Usability::Good,
            USABILITY_BAD => Usability::Bad,
            // SAFETY: LibWeb hands over the message of the exception.
            USABILITY_INVALID_STATE => Usability::InvalidState(unsafe { Utf16String::from_raw_owned(message) }),
            unknown => unreachable!("LibWeb returned {unknown} for the usability of an image"),
        }
    }

    /// The width and the height of the image, as the operations that do not take its size use them.
    pub(crate) fn size(self, source: CppInterface) -> (i32, i32) {
        let (mut width, mut height) = (0, 0);
        unsafe {
            web_canvas_2d_image_source_size(source.wrapper().as_ptr().cast(), self, &raw mut width, &raw mut height);
        }
        (width, height)
    }

    /// https://html.spec.whatwg.org/multipage/canvas.html#the-image-argument-is-not-origin-clean
    pub(crate) fn is_not_origin_clean(self, source: CppInterface) -> bool {
        unsafe { web_canvas_2d_image_is_not_origin_clean(source.wrapper().as_ptr().cast(), self) }
    }

    /// The id of the canvas that a canvas or an OffscreenCanvas draws into, readied to be drawn, and whether a 2D
    /// context draws into it, if it is drawn by its id.
    pub(crate) fn canvas_id(self, source: CppInterface) -> Option<(u64, bool)> {
        let (mut canvas_id, mut source_is_2d) = (0, false);
        let has_id = unsafe {
            web_canvas_2d_image_source_canvas_id(
                source.wrapper().as_ptr().cast(),
                self,
                &raw mut canvas_id,
                &raw mut source_is_2d,
            )
        };
        has_id.then_some((canvas_id, source_is_2d))
    }

    /// The frame of an image or a canvas, if it has one. This can read a canvas back, which flushes the commands of
    /// every canvas.
    pub(crate) fn frame(self, source: CppInterface) -> Option<ImageFrameHandle> {
        let frame = unsafe { web_canvas_2d_image_source_frame(source.wrapper().as_ptr().cast(), self) };
        // SAFETY: LibWeb hands over a frame that it allocated with new, or null.
        (!frame.is_null()).then(|| unsafe { ImageFrameHandle::adopt(frame) })
    }
}

/// The frame of the video that its element shows, if it shows one.
pub(crate) fn capture_video_frame(video: CppInterface) -> Option<CapturedVideoFrame> {
    let mut frame = CapturedVideoFrameAbi {
        hold: core::ptr::null_mut(),
        sink_handle: 0,
        generation: 0,
    };
    // SAFETY: The wrapper is a live HTMLVideoElement, and LibWeb hands over a reference to the hold.
    if !unsafe { web_canvas_2d_capture_video_frame(video.wrapper().as_ptr().cast(), &raw mut frame) } {
        return None;
    }
    Some(CapturedVideoFrame {
        hold: NonNull::new(frame.hold).expect("LibWeb passes the hold of the frame"),
        sink_handle: frame.sink_handle,
        generation: frame.generation,
    })
}

/// A transport to a new context in the Compositor of the page of the video, if the page has a Compositor.
pub(crate) fn video_transport(video: CppInterface) -> Option<CppTransport> {
    // SAFETY: The wrapper is a live HTMLVideoElement.
    let transport = unsafe { web_canvas_2d_video_transport(video.wrapper().as_ptr().cast()) };
    // SAFETY: LibWeb hands over a reference to the transport.
    NonNull::new(transport).map(|transport| unsafe { CppTransport::from_raw(transport) })
}
