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

/// The bytes of the names of the extensions C++ lends.
fn extension_names(names: ak::Span<'_, ak::Span<'_, u8>>) -> Vec<Vec<u8>> {
    names.as_slice().iter().map(|name| name.as_slice().to_vec()).collect()
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
pub struct ContextCreationArguments<'a> {
    /// A RemoteWebGLTransport that has created its context, with a reference that the new context takes over.
    transport: *mut c_void,
    is_webgl2: bool,
    realm: *mut c_void,
    canvas: *mut c_void,
    canvas_host: *mut c_void,
    canvas_is_offscreen: bool,
    context_attributes: ContextAttributes,
    supported_extensions: ak::Span<'a, ak::Span<'a, u8>>,
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
    arguments: &ContextCreationArguments<'_>,
) -> *mut c_void {
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
        supported_opengl_extensions: extension_names(arguments.supported_extensions),
    });
    match version {
        WebGLVersion::WebGL1 => {
            let context = allocate(vm, |_| RenderingContext::<WebGL1>::new(state));
            context.set_context_ref(ContextRef::WebGL1(context));
            cell_into_abi(context)
        }
        WebGLVersion::WebGL2 => {
            let context = allocate(vm, |_| RenderingContext::<WebGL2>::new(state));
            context.set_context_ref(ContextRef::WebGL2(context));
            cell_into_abi(context)
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_canvas_host(context: &ContextCell) -> *mut c_void {
    with_context(context, |context| context.canvas().host.as_ptr())
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_realm(context: &ContextCell) -> *mut c_void {
    with_context(context, |context| context.realm().as_ptr().cast())
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_is_webgl2(context: &ContextCell) -> bool {
    with_context(context, ContextState::is_webgl2)
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
        context.proxy.set_size(
            width.clamp(1, MAX_WEBGL_DRAWING_BUFFER_DIMENSION),
            height.clamp(1, MAX_WEBGL_DRAWING_BUFFER_DIMENSION),
        );
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

/// A Gfx::Bitmap with a reference that the caller takes over, or null.
#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_read_back_drawing_buffer(
    context: &ContextCell,
    x: i32,
    y: i32,
    width: i32,
    height: i32,
) -> *mut c_void {
    let bitmap = with_context(context, |context| {
        context.proxy.read_back_drawing_buffer(x, y, width, height)
    });
    bitmap.map_or(core::ptr::null_mut(), super::host::Bitmap::into_raw)
}

/// False when the context was lost already.
#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_mark_lost(context: &ContextCell) -> bool {
    with_context(context, ContextState::mark_lost)
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_set_restore_requested(context: &ContextCell, requested: bool) {
    with_context(context, |context| context.set_context_restore_requested(requested));
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_wants_restoring(context: &ContextCell) -> bool {
    with_context(context, ContextState::wants_restoring)
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_actual_context_parameters(
    context: &ContextCell,
    out_context_attributes: &mut ContextAttributes,
) {
    let context_attributes = with_context(context, |context| context.actual_context_parameters);
    *out_context_attributes = context_attributes.into();
}

/// The context takes over the reference to `transport`.
///
/// # Safety
///
/// `transport` is a RemoteWebGLTransport that has created its context.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn web_webgl_context_restore(
    context: &ContextCell,
    transport: *mut c_void,
    supported_extensions: ak::Span<'_, ak::Span<'_, u8>>,
) {
    let transport = NonNull::new(transport).expect("C++ passes the transport");
    // SAFETY: The caller hands over a reference to the transport.
    let transport = unsafe { CppTransport::from_raw(transport) };
    let supported_opengl_extensions = extension_names(supported_extensions);
    with_context(context, |context| {
        context.restore(transport, supported_opengl_extensions);
    });
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_xr_compatible(context: &ContextCell) -> bool {
    with_context(context, ContextState::xr_compatible)
}

#[unsafe(no_mangle)]
pub extern "C" fn web_webgl_context_set_xr_compatible(context: &ContextCell, xr_compatible: bool) {
    with_context(context, |context| context.set_xr_compatible(xr_compatible));
}
