/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The state that both kinds of WebGL context share: the error, the objects a context may use, and what is bound to
//! what.

use core::cell::{Cell, RefCell};
use core::ffi::c_void;
use core::marker::PhantomData;
use core::ops::Deref;

use ak::Utf16String;
use libjs_rust::api::{DeferGc, ForeignCellSlot, Gc, GcRefCell, Realm, Trace, Visitor, Vm};

use super::canvas::{
    Canvas, TexImageSourceFrame, TexImageSourceKind, read_tex_image_frame, read_tex_image_pixels, read_tex_image_source,
};
use super::constants::{
    GL_BROWSER_DEFAULT_WEBGL, GL_CONTEXT_LOST_WEBGL, GL_INVALID_OPERATION, GL_INVALID_VALUE, GL_NO_ERROR,
};
use super::extensions::{EnabledEmptyExtension, EnabledExtension};
use super::host::{CppTransport, Frame};
use super::pixel_unpacking::PixelUnpackState;
use super::proxy::custom::FrameUpload;
use super::proxy::recorder::Proxy;
use super::web_gl_buffer::WebGLBuffer;
use super::web_gl_framebuffer::WebGLFramebuffer;
use super::web_gl_object::WebGLObject;
use super::web_gl_program::WebGLProgram;
use super::web_gl_query::WebGLQuery;
use super::web_gl_renderbuffer::WebGLRenderbuffer;
use super::web_gl_rendering_context_base::WebGLContextAttributes;
use super::web_gl_rendering_context_overloads::TexImageSource;
use super::web_gl_texture::WebGLTexture;
use super::web_gl_transform_feedback::WebGLTransformFeedback;
use super::web_gl_uniform_location::WebGLUniformLocation;
use super::web_gl_vertex_array_object::WebGLVertexArrayObject;
use super::web_gl_vertex_array_object_oes::WebGLVertexArrayObjectOES;
use crate::bindings::interface::{Inherits, Interface, Ref, Wrappable, cell_into_abi};
use crate::bindings::{Exception, WebIdlResult};
use crate::canvas_2d::canvas::CanvasImageSourceKind;

pub(crate) const MAX_WEBGL_DRAWING_BUFFER_DIMENSION: i32 = 16384;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum WebGLVersion {
    WebGL1,
    WebGL2,
}

pub(crate) trait Version: Sized + 'static {}

pub(crate) struct WebGL1;
pub(crate) struct WebGL2;

impl Version for WebGL1 {}
impl Version for WebGL2 {}

/// The operations that WebGLRenderingContext and WebGL2RenderingContext share are implemented once, for every `V`.
pub(crate) struct RenderingContext<V: Version> {
    state: ContextState,
    version: PhantomData<V>,
}

impl<V: Version> RenderingContext<V> {
    pub(crate) fn new(_: &DeferGc<'_>, state: ContextState) -> Self {
        Self {
            state,
            version: PhantomData,
        }
    }
}

impl<V: Version> Deref for RenderingContext<V> {
    type Target = ContextState;

    fn deref(&self) -> &ContextState {
        &self.state
    }
}

// SAFETY: The state holds every cell that a context can reach.
unsafe impl<V: Version> Trace for RenderingContext<V> {
    fn trace(&self, visitor: &mut Visitor) {
        self.state.trace(visitor);
    }
}

impl<V: Version> Wrappable for RenderingContext<V> {
    fn relevant_realm(&self, _vm: &Vm) -> Option<Gc<Realm>> {
        self.state.canvas().relevant_realm()
    }
}

#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum ContextRef {
    WebGL1(Ref<RenderingContext<WebGL1>>),
    WebGL2(Ref<RenderingContext<WebGL2>>),
}

impl ContextRef {
    pub(crate) fn state(&self) -> &ContextState {
        match self {
            Self::WebGL1(context) => &context.state,
            Self::WebGL2(context) => &context.state,
        }
    }

    pub(crate) fn cell(&self) -> *mut c_void {
        match *self {
            Self::WebGL1(context) => cell_into_abi(context),
            Self::WebGL2(context) => cell_into_abi(context),
        }
    }
}

#[derive(Clone, Copy, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum VertexArrayBinding {
    WebGL2(Ref<WebGLVertexArrayObject>),
    Oes(Ref<WebGLVertexArrayObjectOES>),
}

pub(crate) type Binding<T> = Cell<Option<Ref<T>>>;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct ContextState {
    #[gc(untraced)]
    pub(crate) proxy: Proxy<CppTransport>,
    #[gc(untraced)]
    version: WebGLVersion,
    context_ref: Cell<Option<ContextRef>>,
    realm: Gc<Realm>,
    canvas_cell: ForeignCellSlot,
    #[gc(untraced)]
    canvas: Canvas,

    /// https://www.khronos.org/registry/webgl/specs/latest/1.0/#context-creation-parameters
    #[gc(untraced)]
    pub(crate) context_creation_parameters: WebGLContextAttributes,
    /// https://www.khronos.org/registry/webgl/specs/latest/1.0/#actual-context-parameters
    #[gc(untraced)]
    pub(crate) actual_context_parameters: WebGLContextAttributes,
    #[gc(untraced)]
    pub(crate) supported_opengl_extensions: RefCell<Vec<Vec<u8>>>,

    #[gc(untraced)]
    error: Cell<u32>,
    /// https://registry.khronos.org/webgl/specs/latest/2.0/#webgl-context-lost-flag
    #[gc(untraced)]
    context_lost: Cell<bool>,
    #[gc(untraced)]
    context_restore_requested: Cell<bool>,
    /// https://immersive-web.github.io/webxr/#xr-compatible
    #[gc(untraced)]
    xr_compatible: Cell<bool>,
    #[gc(untraced)]
    context_generation: Cell<u64>,

    #[gc(untraced)]
    pub(crate) unpack_state: Cell<PixelUnpackState>,
    #[gc(untraced)]
    pub(crate) unpack_flip_y: Cell<bool>,
    #[gc(untraced)]
    pub(crate) unpack_premultiply_alpha: Cell<bool>,
    #[gc(untraced)]
    pub(crate) unpack_colorspace_conversion: Cell<u32>,

    #[gc(untraced)]
    pub(crate) enabled_compressed_texture_formats: RefCell<Vec<u32>>,
    pub(crate) enabled_extensions: GcRefCell<Vec<EnabledExtension>>,
    #[gc(untraced)]
    pub(crate) enabled_empty_extensions: RefCell<Vec<EnabledEmptyExtension>>,

    pub(crate) array_buffer_binding: Binding<WebGLBuffer>,
    pub(crate) element_array_buffer_binding: Binding<WebGLBuffer>,
    pub(crate) current_program: Binding<WebGLProgram>,
    pub(crate) framebuffer_binding: Binding<WebGLFramebuffer>,
    pub(crate) renderbuffer_binding: Binding<WebGLRenderbuffer>,
    pub(crate) texture_binding_2d: Binding<WebGLTexture>,
    pub(crate) texture_binding_cube_map: Binding<WebGLTexture>,
    // FIXME: The bindings from here on are of WebGL 2 only, but operations that the two kinds share use them.
    pub(crate) uniform_buffer_binding: Binding<WebGLBuffer>,
    pub(crate) copy_read_buffer_binding: Binding<WebGLBuffer>,
    pub(crate) copy_write_buffer_binding: Binding<WebGLBuffer>,
    pub(crate) transform_feedback_buffer_binding: Binding<WebGLBuffer>,
    pub(crate) pixel_pack_buffer_binding: Binding<WebGLBuffer>,
    pub(crate) pixel_unpack_buffer_binding: Binding<WebGLBuffer>,
    pub(crate) texture_binding_2d_array: Binding<WebGLTexture>,
    pub(crate) texture_binding_3d: Binding<WebGLTexture>,
    pub(crate) transform_feedback_binding: Binding<WebGLTransformFeedback>,
    pub(crate) any_samples_passed: Binding<WebGLQuery>,
    pub(crate) any_samples_passed_conservative: Binding<WebGLQuery>,
    pub(crate) transform_feedback_primitives_written: Binding<WebGLQuery>,
    pub(crate) current_vertex_array: Cell<Option<VertexArrayBinding>>,
}

pub(crate) struct ContextParameters {
    pub(crate) proxy: Proxy<CppTransport>,
    pub(crate) version: WebGLVersion,
    pub(crate) realm: Gc<Realm>,
    pub(crate) canvas: Canvas,
    pub(crate) context_attributes: WebGLContextAttributes,
    pub(crate) supported_opengl_extensions: Vec<Vec<u8>>,
}

impl ContextState {
    pub(crate) fn new(_: &DeferGc<'_>, parameters: ContextParameters) -> Self {
        let canvas_cell = ForeignCellSlot::empty();
        // SAFETY: The canvas is a live cell of the heap that the runtime allocates from.
        unsafe { canvas_cell.set(Some(parameters.canvas.cell)) };
        Self {
            proxy: parameters.proxy,
            version: parameters.version,
            context_ref: Cell::new(None),
            realm: parameters.realm,
            canvas_cell,
            canvas: parameters.canvas,
            context_creation_parameters: parameters.context_attributes,
            actual_context_parameters: parameters.context_attributes,
            supported_opengl_extensions: RefCell::new(parameters.supported_opengl_extensions),
            error: Cell::new(GL_NO_ERROR),
            context_lost: Cell::new(false),
            context_restore_requested: Cell::new(false),
            xr_compatible: Cell::new(false),
            context_generation: Cell::new(0),
            unpack_state: Cell::new(PixelUnpackState::default()),
            unpack_flip_y: Cell::new(false),
            unpack_premultiply_alpha: Cell::new(false),
            unpack_colorspace_conversion: Cell::new(GL_BROWSER_DEFAULT_WEBGL),
            enabled_compressed_texture_formats: RefCell::new(Vec::new()),
            enabled_extensions: GcRefCell::new(Vec::new()),
            enabled_empty_extensions: RefCell::new(Vec::new()),
            array_buffer_binding: Cell::new(None),
            element_array_buffer_binding: Cell::new(None),
            current_program: Cell::new(None),
            framebuffer_binding: Cell::new(None),
            renderbuffer_binding: Cell::new(None),
            texture_binding_2d: Cell::new(None),
            texture_binding_cube_map: Cell::new(None),
            uniform_buffer_binding: Cell::new(None),
            copy_read_buffer_binding: Cell::new(None),
            copy_write_buffer_binding: Cell::new(None),
            transform_feedback_buffer_binding: Cell::new(None),
            pixel_pack_buffer_binding: Cell::new(None),
            pixel_unpack_buffer_binding: Cell::new(None),
            texture_binding_2d_array: Cell::new(None),
            texture_binding_3d: Cell::new(None),
            transform_feedback_binding: Cell::new(None),
            any_samples_passed: Cell::new(None),
            any_samples_passed_conservative: Cell::new(None),
            transform_feedback_primitives_written: Cell::new(None),
            current_vertex_array: Cell::new(None),
        }
    }

    pub(crate) fn set_context_ref(&self, context_ref: ContextRef) {
        self.context_ref.set(Some(context_ref));
    }

    pub(crate) fn context_ref(&self) -> ContextRef {
        self.context_ref
            .get()
            .expect("a context knows its cell from its creation on")
    }

    pub(crate) fn version(&self) -> WebGLVersion {
        self.version
    }

    pub(crate) fn is_webgl2(&self) -> bool {
        self.version == WebGLVersion::WebGL2
    }

    pub(crate) fn realm(&self) -> Gc<Realm> {
        self.realm
    }

    pub(crate) fn canvas(&self) -> Canvas {
        self.canvas
    }

    pub(crate) fn context_generation(&self) -> u64 {
        self.context_generation.get()
    }

    pub(crate) fn is_context_lost(&self) -> bool {
        self.context_lost.get()
    }

    pub(crate) fn xr_compatible(&self) -> bool {
        self.xr_compatible.get()
    }

    pub(crate) fn set_xr_compatible(&self, xr_compatible: bool) {
        self.xr_compatible.set(xr_compatible);
    }

    /// Makes `error` the error of the context, unless it has one already.
    // TODO: The specification of glGetError allows for a queue of errors, which this could keep.
    pub(crate) fn set_error(&self, error: u32) {
        if self.error.get() == GL_NO_ERROR {
            self.error.set(error);
        }
    }

    pub(crate) fn take_error(&self) -> u32 {
        if self.context_lost.get() {
            return self.error.replace(GL_NO_ERROR);
        }
        let local_error = self.proxy.take_pending_local_error();
        if local_error != GL_NO_ERROR {
            return local_error;
        }
        let context_error = self.proxy.get_error();
        if context_error != GL_NO_ERROR {
            return context_error;
        }
        self.error.replace(GL_NO_ERROR)
    }

    pub(crate) fn handle_of<O: Interface + Inherits<WebGLObject>>(&self, object: Ref<O>) -> Option<u32> {
        let handle = object.parent().handle(self).ok();
        if handle.is_none() {
            self.set_error(GL_INVALID_OPERATION);
        }
        handle
    }

    pub(crate) fn handle_for_deletion_of<O: Interface + Inherits<WebGLObject>>(&self, object: Ref<O>) -> Option<u32> {
        match object.parent().handle_for_deletion(self) {
            Ok(handle) => handle,
            Err(_) => {
                self.set_error(GL_INVALID_OPERATION);
                None
            }
        }
    }

    pub(crate) fn handle_for_query_of<O: Interface + Inherits<WebGLObject>>(&self, object: Ref<O>) -> Option<u32> {
        match object.parent().handle_for_query(self) {
            Ok(handle) => handle,
            Err(_) => {
                self.set_error(GL_INVALID_OPERATION);
                None
            }
        }
    }

    pub(crate) fn read_texture_image_source(
        &self,
        source: &TexImageSource,
        format: u32,
        r#type: u32,
    ) -> WebIdlResult<Option<(Frame, FrameUpload)>> {
        // If this function is called with an HTMLImageElement or HTMLVideoElement whose origin differs from the origin
        // of the containing Document, or with an HTMLCanvasElement, ImageBitmap or OffscreenCanvas whose bitmap's
        // origin-clean flag is set to false, a SECURITY_ERR exception must be thrown. See Origin Restrictions.
        // https://registry.khronos.org/webgl/specs/latest/1.0/#ORIGIN_RESTRICTIONS
        if tex_image_source_is_not_origin_clean(source) {
            return Err(Exception::Dom(
                "SecurityError",
                Utf16String::from_ascii(b"Cannot upload a texture source that is not origin-clean"),
            ));
        }

        let source_frame = match *source {
            TexImageSource::ImageBitmap(image_bitmap) => match image_bitmap.frame() {
                Some(image_frame) => read_tex_image_frame(&image_frame, format, r#type),
                // If this function is called with an ImageBitmap that has been neutered, an INVALID_VALUE error is
                // generated.
                None => TexImageSourceFrame::InvalidValue,
            },
            TexImageSource::ImageData(image_data) => match image_data.pixels() {
                Some(pixels) => {
                    let (width, height) = image_data.size();
                    // SAFETY: Nothing runs script while the pixels are copied.
                    unsafe { read_tex_image_pixels(pixels, width, height, format, r#type) }
                }
                // If this function is called with an ImageData whose data attribute has been neutered, an
                // INVALID_VALUE error is generated.
                None => TexImageSourceFrame::InvalidValue,
            },
            TexImageSource::HTMLImageElement(object) => {
                read_tex_image_source(object, TexImageSourceKind::HTMLImageElement, format, r#type)
            }
            TexImageSource::HTMLCanvasElement(object) => {
                read_tex_image_source(object, TexImageSourceKind::HTMLCanvasElement, format, r#type)
            }
            TexImageSource::HTMLVideoElement(object) => {
                read_tex_image_source(object, TexImageSourceKind::HTMLVideoElement, format, r#type)
            }
            TexImageSource::OffscreenCanvas(object) => {
                read_tex_image_source(object, TexImageSourceKind::OffscreenCanvas, format, r#type)
            }
        };
        let frame = match source_frame {
            TexImageSourceFrame::Frame(frame) => frame,
            TexImageSourceFrame::NoFrame => return Ok(None),
            TexImageSourceFrame::InvalidValue => {
                self.set_error(GL_INVALID_VALUE);
                return Ok(None);
            }
        };
        // The first pixel transferred from the source to the WebGL implementation corresponds to the upper left
        // corner of the source. This behavior is modified by the UNPACK_FLIP_Y_WEBGL pixel storage parameter,
        // except for ImageBitmap arguments, as described in the abovementioned section.
        let flip_y = self.unpack_flip_y.get() && !matches!(source, TexImageSource::ImageBitmap(_));
        let upload = FrameUpload {
            destination_size: None,
            flip_y,
            premultiply_alpha: self.unpack_premultiply_alpha.get(),
        };
        Ok(Some((frame, upload)))
    }

    /// https://registry.khronos.org/webgl/specs/latest/2.0/#PIXEL_STORE_PARAM_CONSTRAINTS
    /// If UNPACK_FLIP_Y_WEBGL or UNPACK_PREMULTIPLY_ALPHA_WEBGL is set to true, texImage3D and texSubImage3D generate
    /// an INVALID_OPERATION error if they upload data from a PIXEL_UNPACK_BUFFER or a non-null client side
    /// ArrayBufferView.
    pub(crate) fn unpack_flags_allow_3d_upload_from_array_buffer_view(&self) -> bool {
        if self.unpack_flip_y.get() || self.unpack_premultiply_alpha.get() {
            self.set_error(GL_INVALID_OPERATION);
            return false;
        }
        true
    }

    pub(crate) fn uniform_location_handle(&self, location: Ref<WebGLUniformLocation>) -> Option<i32> {
        let handle = location.handle(self.current_program.get()).ok();
        if handle.is_none() {
            self.set_error(GL_INVALID_OPERATION);
        }
        handle
    }

    pub(crate) fn did_update_canvas_content(&self) {
        self.canvas.did_change_content();
    }

    /// https://registry.khronos.org/webgl/specs/latest/1.0/#CONTEXT_LOST
    pub(crate) fn mark_lost(&self) -> bool {
        if self.context_lost.replace(true) {
            return false;
        }
        self.proxy.set_lost();
        // NB: The next getError() has to return CONTEXT_LOST_WEBGL, once.
        self.error.set(GL_CONTEXT_LOST_WEBGL);
        true
    }

    pub(crate) fn set_context_restore_requested(&self, requested: bool) {
        self.context_restore_requested.set(requested);
    }

    pub(crate) fn wants_restoring(&self) -> bool {
        self.context_lost.get() && self.context_restore_requested.get()
    }

    /// The new replayer context has no GL objects: the page creates its objects anew once told of the restore.
    pub(crate) fn restore(&self, transport: CppTransport, supported_opengl_extensions: Vec<Vec<u8>>) {
        self.proxy.restore(transport);
        self.supported_opengl_extensions.replace(supported_opengl_extensions);

        self.context_generation.set(self.context_generation.get() + 1);
        self.unpack_state.set(PixelUnpackState::default());
        self.unpack_flip_y.set(false);
        self.unpack_premultiply_alpha.set(false);
        self.unpack_colorspace_conversion.set(GL_BROWSER_DEFAULT_WEBGL);
        self.reset_client_side_state();

        self.context_lost.set(false);
        self.context_restore_requested.set(false);
        self.error.set(GL_NO_ERROR);
    }

    fn reset_client_side_state(&self) {
        self.array_buffer_binding.set(None);
        self.element_array_buffer_binding.set(None);
        self.current_program.set(None);
        self.framebuffer_binding.set(None);
        self.renderbuffer_binding.set(None);
        self.texture_binding_2d.set(None);
        self.texture_binding_cube_map.set(None);
        self.uniform_buffer_binding.set(None);
        self.copy_read_buffer_binding.set(None);
        self.copy_write_buffer_binding.set(None);
        self.transform_feedback_buffer_binding.set(None);
        self.pixel_pack_buffer_binding.set(None);
        self.pixel_unpack_buffer_binding.set(None);
        self.texture_binding_2d_array.set(None);
        self.texture_binding_3d.set(None);
        self.transform_feedback_binding.set(None);
        self.current_vertex_array.set(None);
        self.any_samples_passed.set(None);
        self.any_samples_passed_conservative.set(None);
        self.transform_feedback_primitives_written.set(None);
    }
}

/// https://html.spec.whatwg.org/multipage/canvas.html#the-image-argument-is-not-origin-clean
fn tex_image_source_is_not_origin_clean(source: &TexImageSource) -> bool {
    match *source {
        TexImageSource::ImageBitmap(image_bitmap) => !image_bitmap.origin_clean(),
        TexImageSource::ImageData(_) => false,
        TexImageSource::HTMLImageElement(object) => CanvasImageSourceKind::HTMLImageElement.is_not_origin_clean(object),
        TexImageSource::HTMLCanvasElement(object) => {
            CanvasImageSourceKind::HTMLCanvasElement.is_not_origin_clean(object)
        }
        TexImageSource::HTMLVideoElement(object) => CanvasImageSourceKind::HTMLVideoElement.is_not_origin_clean(object),
        TexImageSource::OffscreenCanvas(object) => CanvasImageSourceKind::OffscreenCanvas.is_not_origin_clean(object),
    }
}
