/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI that Libraries/LibWeb/WebGL/WebGLRenderingContextBase.cpp declares, and calls with the cell of a
//! context of either kind.

use core::ffi::c_void;
use core::marker::PhantomData;
use core::ptr::NonNull;

use libjs_rust::api::Vm;

use super::canvas::Canvas;
use super::context_loss::{
    fire_webgl_context_event, lose_contexts_of_this_thread, register_context, restore_contexts_of_this_thread,
};
use super::host::{ContextCreation, RemoteContext};
use super::proxy::recorder::Proxy;
use super::state::{
    ContextParameters, ContextRef, ContextState, RenderingContext, WebGL1, WebGL2, WebGLVersion,
    drawing_buffer_dimension,
};
use super::web_gl_rendering_context::WebGLPowerPreference;
use super::web_gl_rendering_context_base::WebGLContextAttributes;
use crate::bindings::interface::{Interface, allocate, cell_into_abi, realm_arguments};

/// Mirrors `Web::WebGL::ContextAttributes`.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct ContextAttributes {
    alpha: bool,
    depth: bool,
    stencil: bool,
    antialias: bool,
    premultiplied_alpha: bool,
    preserve_drawing_buffer: bool,
    /// 0 is "default", 1 is "low-power" and 2 is "high-performance".
    power_preference: u8,
    fail_if_major_performance_caveat: bool,
    desynchronized: bool,
}

impl From<ContextAttributes> for WebGLContextAttributes {
    fn from(attributes: ContextAttributes) -> Self {
        Self {
            alpha: attributes.alpha,
            depth: attributes.depth,
            stencil: attributes.stencil,
            antialias: attributes.antialias,
            premultiplied_alpha: attributes.premultiplied_alpha,
            preserve_drawing_buffer: attributes.preserve_drawing_buffer,
            power_preference: match attributes.power_preference {
                0 => WebGLPowerPreference::Default,
                1 => WebGLPowerPreference::LowPower,
                2 => WebGLPowerPreference::HighPerformance,
                unknown => unreachable!("LibWeb passed {unknown} for a power preference"),
            },
            fail_if_major_performance_caveat: attributes.fail_if_major_performance_caveat,
            desynchronized: attributes.desynchronized,
        }
    }
}

impl From<WebGLContextAttributes> for ContextAttributes {
    fn from(attributes: WebGLContextAttributes) -> Self {
        Self {
            alpha: attributes.alpha,
            depth: attributes.depth,
            stencil: attributes.stencil,
            antialias: attributes.antialias,
            premultiplied_alpha: attributes.premultiplied_alpha,
            preserve_drawing_buffer: attributes.preserve_drawing_buffer,
            power_preference: match attributes.power_preference {
                WebGLPowerPreference::Default => 0,
                WebGLPowerPreference::LowPower => 1,
                WebGLPowerPreference::HighPerformance => 2,
            },
            fail_if_major_performance_caveat: attributes.fail_if_major_performance_caveat,
            desynchronized: attributes.desynchronized,
        }
    }
}

/// Mirrors `Web::WebGL::ContextCreationArguments`.
#[repr(C)]
pub struct ContextCreationArguments {
    is_webgl2: bool,
    realm: *mut c_void,
    canvas: *mut c_void,
    canvas_host: *mut c_void,
    canvas_is_offscreen: bool,
    context_attributes: ContextAttributes,
}

pub(super) fn create_remote_context(
    canvas: Canvas,
    webgl2: bool,
    context_attributes: &WebGLContextAttributes,
) -> Option<(RemoteContext, Vec<Vec<u8>>)> {
    let (width, height) = canvas.bitmap_size();
    let creation = ContextCreation {
        webgl2,
        size: libgfx_rust::IntSize {
            width: drawing_buffer_dimension(width),
            height: drawing_buffer_dimension(height),
        },
        depth: context_attributes.depth,
        stencil: context_attributes.stencil,
        antialias: context_attributes.antialias,
    };
    let connection = canvas.compositor_connection()?;
    // SAFETY: The canvas handed over a reference to the connection.
    unsafe { RemoteContext::create(connection, &creation) }
}

/// The cell of a WebGLRenderingContext or of a WebGL2RenderingContext, which only C++ lends out, for as long as the
/// context is live.
#[repr(C)]
pub struct ContextCell {
    // NB: The cell is mutable behind a shared reference, and bigger than Rust sees, so a reference to one must not tell
    //     the compiler that the memory behind it is read only or unaliased.
    _opaque: core::cell::UnsafeCell<[u8; 0]>,
    _not_send_sync_or_unpin: PhantomData<(*const (), core::marker::PhantomPinned)>,
}

fn with_context<R>(context: &ContextCell, function: impl FnOnce(&ContextState) -> R) -> R {
    let cell = NonNull::from(context).cast();
    // SAFETY: A ContextCell is the live cell of one of the two classes, which the downcasts tell apart.
    unsafe {
        if let Some(context) = RenderingContext::<WebGL1>::CELL_CLASS.downcast(cell) {
            return function(&context);
        }
        let context = RenderingContext::<WebGL2>::CELL_CLASS
            .downcast(cell)
            .expect("the cell is a WebGL context");
        function(&context)
    }
}

/// # Safety
///
/// The pointers are the live VM and what [`ContextCreationArguments`] describes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_create(
    vm: *mut c_void,
    arguments: &ContextCreationArguments,
) -> *mut c_void {
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, arguments.realm) };

    let canvas = Canvas {
        cell: NonNull::new(arguments.canvas).expect("C++ passes the canvas"),
        host: NonNull::new(arguments.canvas_host).expect("C++ passes the canvas"),
        is_offscreen: arguments.canvas_is_offscreen,
    };
    let version = if arguments.is_webgl2 {
        WebGLVersion::WebGL2
    } else {
        WebGLVersion::WebGL1
    };
    let context_attributes: WebGLContextAttributes = arguments.context_attributes.into();
    let Some((remote_context, supported_opengl_extensions)) =
        create_remote_context(canvas, version == WebGLVersion::WebGL2, &context_attributes)
    else {
        // https://www.khronos.org/registry/webgl/specs/latest/1.0/#fire-a-webgl-context-creation-error
        // 1. Fire a WebGL context event named "webglcontextcreationerror" at canvas, optionally with its statusMessage attribute set to a platform dependent string about the nature of the failure.
        fire_webgl_context_event(vm, canvas, "webglcontextcreationerror");
        return core::ptr::null_mut();
    };
    let parameters = ContextParameters {
        proxy: Proxy::new(remote_context),
        version,
        realm,
        canvas,
        context_attributes,
        supported_opengl_extensions,
    };
    // NB: The display list must be re-recorded so its DrawCanvas command refers to the new remote context's
    //     canvas id. Content updates alone don't invalidate the display list, so do it here.
    canvas.did_create_backing_storage();
    match version {
        WebGLVersion::WebGL1 => {
            let context = allocate(vm, |defer_gc| {
                RenderingContext::<WebGL1>::new(defer_gc, ContextState::new(defer_gc, parameters))
            });
            context.set_context_ref(ContextRef::WebGL1(context));
            register_context(vm, ContextRef::WebGL1(context));
            cell_into_abi(context)
        }
        WebGLVersion::WebGL2 => {
            let context = allocate(vm, |defer_gc| {
                RenderingContext::<WebGL2>::new(defer_gc, ContextState::new(defer_gc, parameters))
            });
            context.set_context_ref(ContextRef::WebGL2(context));
            register_context(vm, ContextRef::WebGL2(context));
            cell_into_abi(context)
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_is_lost(context: &ContextCell) -> bool {
    with_context(context, ContextState::is_context_lost)
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_canvas_id(context: &ContextCell, out_canvas_id: &mut u64) -> bool {
    let Some(canvas_id) = with_context(context, |context| context.proxy.canvas_id()) else {
        return false;
    };
    *out_canvas_id = canvas_id;
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_set_size(context: &ContextCell, width: i32, height: i32) {
    with_context(context, |context| {
        context
            .proxy
            .set_size(drawing_buffer_dimension(width), drawing_buffer_dimension(height));
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_present_for_compositing(context: &ContextCell) {
    with_context(context, |context| {
        let preserve_drawing_buffer = context.context_creation_parameters.preserve_drawing_buffer;
        context.proxy.present_canvas_for_compositing(preserve_drawing_buffer);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_clear_drawing_buffer(context: &ContextCell) {
    with_context(context, |context| context.proxy.clear_drawing_buffer());
}

/// A Gfx::DecodedImageFrame allocated with new that the caller takes over, or null.
#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_read_back_drawing_buffer(
    context: &ContextCell,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
) -> *mut c_void {
    let frame = with_context(context, |context| {
        context.proxy.read_back_drawing_buffer(x, y, width, height)
    });
    frame.map_or(core::ptr::null_mut(), |frame| frame.hand_over_copy())
}

/// # Safety
///
/// `vm` must be the live VM of the thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_lose_contexts_of_this_thread(vm: *mut c_void) {
    // SAFETY: The caller passes the live VM.
    lose_contexts_of_this_thread(unsafe { &*vm.cast::<Vm>() });
}

/// # Safety
///
/// `vm` must be the live VM of the thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_restore_contexts_of_this_thread(vm: *mut c_void) {
    // SAFETY: The caller passes the live VM.
    restore_contexts_of_this_thread(unsafe { &*vm.cast::<Vm>() });
}

/// Converts the options of getContext() to the dictionary, and answers true with the exception that the conversion
/// threw.
///
/// # Safety
///
/// `vm` must be the live VM and `value` a live value.
#[cfg(web_idl_bindings)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_attributes_from_value(
    vm: *mut c_void,
    value: u64,
    out_context_attributes: &mut ContextAttributes,
    out_exception: &mut u64,
) -> bool {
    use crate::bindings::conversions::FromJs;

    // SAFETY: The caller passes the live VM and a live value.
    let (vm, value) = unsafe {
        (
            &*vm.cast::<libjs_rust::api::Vm>(),
            libjs_rust::api::Value::from_raw(value),
        )
    };
    match WebGLContextAttributes::from_js(vm, value) {
        Ok(context_attributes) => {
            *out_context_attributes = context_attributes.into();
            false
        }
        Err(throw) => {
            *out_exception = throw.value().to_raw();
            true
        }
    }
}
