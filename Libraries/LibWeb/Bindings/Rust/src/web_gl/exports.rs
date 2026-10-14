/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI that Libraries/LibWeb/WebGL/WebGLRenderingContextBase.cpp declares, and calls with the cell of a
//! context of either kind.

use core::ffi::c_void;
use core::ptr::NonNull;

use super::canvas::Canvas;
use super::fingerprint::verify_that_rust_and_cpp_agree_on_layouts_and_constants;
use super::host::CppTransport;
use super::proxy::recorder::Proxy;
use super::state::{
    ContextParameters, ContextRef, ContextState, MAX_WEBGL_DRAWING_BUFFER_DIMENSION, RenderingContext, WebGL1, WebGL2,
    WebGLVersion,
};
use super::web_gl_rendering_context::WebGLPowerPreference;
use super::web_gl_rendering_context_base::WebGLContextAttributes;
use crate::bindings::interface::{Interface, allocate, cell_into_abi, realm_arguments};

#[repr(C)]
pub struct ByteSpan {
    data: *const u8,
    size: usize,
}

/// # Safety
///
/// The spans are live, and each is the bytes of a string.
unsafe fn extension_names(names: *const ByteSpan, name_count: usize) -> Vec<Vec<u8>> {
    if name_count == 0 {
        return Vec::new();
    }
    // SAFETY: The caller passes `name_count` live spans.
    let names = unsafe { core::slice::from_raw_parts(names, name_count) };
    names
        .iter()
        .map(|name| {
            if name.size == 0 {
                return Vec::new();
            }
            // SAFETY: The caller passes the bytes of a string.
            unsafe { core::slice::from_raw_parts(name.data, name.size) }.to_vec()
        })
        .collect()
}

/// Mirrors `Web::WebGL::RustContextAttributes`.
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
    xr_compatible: bool,
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
            xr_compatible: attributes.xr_compatible,
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
            xr_compatible: attributes.xr_compatible,
        }
    }
}

/// Mirrors `Web::WebGL::ContextCreationArguments`.
#[repr(C)]
pub struct ContextCreationArguments {
    /// A RemoteWebGLTransport that has created its context, with a reference that the new context takes over.
    transport: *mut c_void,
    is_webgl2: bool,
    realm: *mut c_void,
    canvas: *mut c_void,
    canvas_host: *mut c_void,
    canvas_is_offscreen: bool,
    context_attributes: ContextAttributes,
    supported_extensions: *const ByteSpan,
    supported_extension_count: usize,
}

/// # Safety
///
/// `context` is the live cell of a WebGLRenderingContext or of a WebGL2RenderingContext.
unsafe fn with_context<R>(context: *mut c_void, function: impl FnOnce(&ContextState) -> R) -> R {
    let cell = NonNull::new(context).expect("C++ passes the cell of a context");
    // SAFETY: The caller passes a live cell of one of the two classes, which the downcasts tell apart.
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
    arguments: *const ContextCreationArguments,
) -> *mut c_void {
    // SAFETY: The caller passes live arguments.
    let arguments = unsafe { &*arguments };
    // SAFETY: The caller passes the live VM and a live realm.
    let (vm, realm) = unsafe { realm_arguments(vm, arguments.realm) };
    verify_that_rust_and_cpp_agree_on_layouts_and_constants();

    let transport = NonNull::new(arguments.transport).expect("C++ passes the transport");
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
    let state = ContextState::new(ContextParameters {
        // SAFETY: The caller hands over a reference to the transport.
        proxy: Proxy::new(unsafe { CppTransport::from_raw(transport) }),
        version,
        realm,
        canvas,
        context_attributes: arguments.context_attributes.into(),
        // SAFETY: The caller passes live names.
        supported_opengl_extensions: unsafe {
            extension_names(arguments.supported_extensions, arguments.supported_extension_count)
        },
    });
    match version {
        WebGLVersion::WebGL1 => {
            let context = allocate(vm, RenderingContext::<WebGL1>::new(state));
            context.set_context_ref(ContextRef::WebGL1(context));
            cell_into_abi(context)
        }
        WebGLVersion::WebGL2 => {
            let context = allocate(vm, RenderingContext::<WebGL2>::new(state));
            context.set_context_ref(ContextRef::WebGL2(context));
            cell_into_abi(context)
        }
    }
}

// SAFETY (of every function below): The caller passes the live cell of a context, and live pointers to what a
// function stores.

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_canvas_host(context: *mut c_void) -> *mut c_void {
    unsafe { with_context(context, |context| context.canvas().host.as_ptr()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_realm(context: *mut c_void) -> *mut c_void {
    unsafe { with_context(context, |context| context.realm().as_ptr().cast()) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_is_webgl2(context: *mut c_void) -> bool {
    unsafe { with_context(context, ContextState::is_webgl2) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_is_lost(context: *mut c_void) -> bool {
    unsafe { with_context(context, ContextState::is_context_lost) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_canvas_id(context: *mut c_void, out_canvas_id: *mut u64) -> bool {
    let Some(canvas_id) = (unsafe { with_context(context, |context| context.proxy.canvas_id()) }) else {
        return false;
    };
    unsafe { out_canvas_id.write(canvas_id) };
    true
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_set_size(context: *mut c_void, width: i32, height: i32) {
    unsafe {
        with_context(context, |context| {
            context.proxy.set_size(
                width.clamp(1, MAX_WEBGL_DRAWING_BUFFER_DIMENSION),
                height.clamp(1, MAX_WEBGL_DRAWING_BUFFER_DIMENSION),
            );
        });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_present_for_compositing(context: *mut c_void) {
    unsafe {
        with_context(context, |context| {
            let preserve_drawing_buffer = context.context_creation_parameters.preserve_drawing_buffer;
            context.proxy.present_canvas_for_compositing(preserve_drawing_buffer);
        });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_clear_drawing_buffer(context: *mut c_void) {
    unsafe { with_context(context, |context| context.proxy.clear_drawing_buffer()) };
}

/// A Gfx::Bitmap with a reference that the caller takes over, or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_read_back_drawing_buffer(
    context: *mut c_void,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
) -> *mut c_void {
    let bitmap = unsafe {
        with_context(context, |context| {
            context.proxy.read_back_drawing_buffer(x, y, width, height)
        })
    };
    bitmap.map_or(core::ptr::null_mut(), super::host::Bitmap::into_raw)
}

/// False when the context was lost already.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_mark_lost(context: *mut c_void) -> bool {
    unsafe { with_context(context, ContextState::mark_lost) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_set_restore_requested(context: *mut c_void, requested: bool) {
    unsafe { with_context(context, |context| context.set_context_restore_requested(requested)) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_wants_restoring(context: *mut c_void) -> bool {
    unsafe { with_context(context, ContextState::wants_restoring) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_actual_context_parameters(
    context: *mut c_void,
    out_context_attributes: *mut ContextAttributes,
) {
    let context_attributes = unsafe { with_context(context, |context| context.actual_context_parameters) };
    unsafe { out_context_attributes.write(context_attributes.into()) };
}

/// The context takes over the reference to `transport`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_restore(
    context: *mut c_void,
    transport: *mut c_void,
    supported_extensions: *const ByteSpan,
    supported_extension_count: usize,
) {
    let transport = NonNull::new(transport).expect("C++ passes the transport");
    unsafe {
        let supported_opengl_extensions = extension_names(supported_extensions, supported_extension_count);
        with_context(context, |context| {
            context.restore(CppTransport::from_raw(transport), supported_opengl_extensions);
        });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_xr_compatible(context: *mut c_void) -> bool {
    unsafe { with_context(context, ContextState::xr_compatible) }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_set_xr_compatible(context: *mut c_void, xr_compatible: bool) {
    unsafe { with_context(context, |context| context.set_xr_compatible(xr_compatible)) };
}
