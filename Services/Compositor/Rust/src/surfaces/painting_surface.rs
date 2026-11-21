/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A surface the Compositor paints into, which the core and the C++ around it share by reference counting: a pointer to
//! a `Gfx::PaintingSurface` is the `PaintingSurface` of an `Rc`. The Skia part of the surface is C++, and the surface
//! owns it. Only the Compositor's main thread may retain, release or read a surface.

use libgfx_rust::IntSize;
use std::ffi::c_void;
use std::ptr::NonNull;
use std::rc::Rc;

pub struct PaintingSurface {
    size: IntSize,
    gpu_backend_context: *const c_void,
    skia_surface: NonNull<c_void>,
    destroy_skia_surface: unsafe extern "C" fn(*mut c_void),
}

impl PaintingSurface {
    pub fn size(&self) -> IntSize {
        self.size
    }

    pub fn uses_gpu(&self) -> bool {
        !self.gpu_backend_context.is_null()
    }

    pub fn shares_backend_with(&self, other: &PaintingSurface) -> bool {
        self.gpu_backend_context == other.gpu_backend_context
    }

    /// Null for a surface in memory.
    pub fn gpu_backend_context(&self) -> *const c_void {
        self.gpu_backend_context
    }

    pub fn as_ffi(self: &Rc<Self>) -> *const PaintingSurface {
        Rc::as_ptr(self)
    }

    /// # Safety
    ///
    /// `surface` must be a live surface, which then has one more owner.
    pub unsafe fn retained_from_ffi(surface: *const PaintingSurface) -> Rc<PaintingSurface> {
        assert!(!surface.is_null(), "the C++ side passes a surface");
        // SAFETY: The caller passes the PaintingSurface of a live Rc.
        unsafe {
            Rc::increment_strong_count(surface);
            Rc::from_raw(surface)
        }
    }

    /// # Safety
    ///
    /// `surface` must be a surface whose one reference the caller hands over, or null.
    pub unsafe fn adopted_from_ffi(surface: *const PaintingSurface) -> Option<Rc<PaintingSurface>> {
        if surface.is_null() {
            return None;
        }
        // SAFETY: The caller hands over one reference to the PaintingSurface of a live Rc.
        Some(unsafe { Rc::from_raw(surface) })
    }

    pub fn into_ffi(surface: Rc<PaintingSurface>) -> *const PaintingSurface {
        Rc::into_raw(surface)
    }
}

impl Drop for PaintingSurface {
    fn drop(&mut self) {
        // SAFETY: The surface owns its Skia part, which nothing uses once the last reference is gone.
        unsafe { (self.destroy_skia_surface)(self.skia_surface.as_ptr()) };
    }
}

/// A new surface whose one reference the caller owns and gives up with `compositor_painting_surface_release()`. It
/// takes over the Skia part, which it destroys with `destroy_skia_surface` once the last reference is gone. A GPU
/// surface names the backend context it renders with, which surfaces that can be drawn into each other share.
///
/// # Safety
///
/// `skia_surface` must be a live Skia part that `destroy_skia_surface` destroys.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_painting_surface_create(
    size: IntSize,
    gpu_backend_context: *const c_void,
    skia_surface: *mut c_void,
    destroy_skia_surface: unsafe extern "C" fn(*mut c_void),
) -> *const PaintingSurface {
    let skia_surface = NonNull::new(skia_surface).expect("the C++ side passes the Skia part of the surface");
    PaintingSurface::into_ffi(Rc::new(PaintingSurface {
        size,
        gpu_backend_context,
        skia_surface,
        destroy_skia_surface,
    }))
}

/// # Safety
///
/// `surface` must be a live surface.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_painting_surface_retain(surface: *const PaintingSurface) {
    assert!(!surface.is_null(), "the C++ side passes a surface");
    // SAFETY: The caller passes the PaintingSurface of a live Rc.
    unsafe { Rc::increment_strong_count(surface) };
}

/// # Safety
///
/// `surface` must be a live surface of which the caller owns a reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_painting_surface_release(surface: *const PaintingSurface) {
    assert!(!surface.is_null(), "the C++ side passes a surface");
    // SAFETY: The caller gives up one of its references to the PaintingSurface of a live Rc.
    unsafe { Rc::decrement_strong_count(surface) };
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_painting_surface_skia_surface(surface: &PaintingSurface) -> *mut c_void {
    surface.skia_surface.as_ptr()
}

/// NB: A C++ function returns Gfx::IntSize in memory rather than in a register on Windows, so the size is written.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_painting_surface_size(surface: &PaintingSurface, size: &mut IntSize) {
    *size = surface.size;
}

#[cfg(test)]
pub mod testing {
    use super::*;
    use std::cell::Cell;

    thread_local! {
        pub static DESTROYED_SKIA_SURFACES: Cell<usize> = const { Cell::new(0) };
    }

    unsafe extern "C" fn count_destroyed_skia_surface(_: *mut c_void) {
        DESTROYED_SKIA_SURFACES.with(|count| count.set(count.get() + 1));
    }

    pub fn surface_for_testing(size: IntSize, gpu_backend_context: usize) -> Rc<PaintingSurface> {
        // SAFETY: The Skia part is a dangling pointer that only the counting destroy function sees.
        let surface = unsafe {
            compositor_painting_surface_create(
                size,
                gpu_backend_context as *const c_void,
                NonNull::<u8>::dangling().as_ptr().cast(),
                count_destroyed_skia_surface,
            )
        };
        // SAFETY: The surface was just made with one reference, which this takes over.
        unsafe { PaintingSurface::adopted_from_ffi(surface) }.expect("a new surface")
    }
}

#[cfg(test)]
mod tests {
    use super::testing::*;
    use super::*;

    #[test]
    fn skia_part_is_destroyed_with_the_last_reference_from_either_side() {
        let before = DESTROYED_SKIA_SURFACES.with(|count| count.get());
        let surface = surface_for_testing(IntSize { width: 4, height: 3 }, 0);
        let pointer = surface.as_ffi();
        // SAFETY: The surface is live, and the reference taken here is given up below.
        unsafe { compositor_painting_surface_retain(pointer) };
        drop(surface);
        assert_eq!(DESTROYED_SKIA_SURFACES.with(|count| count.get()), before);
        // SAFETY: The reference taken above is given up.
        unsafe { compositor_painting_surface_release(pointer) };
        assert_eq!(DESTROYED_SKIA_SURFACES.with(|count| count.get()), before + 1);
    }

    #[test]
    fn surfaces_share_a_backend_by_its_context() {
        let size = IntSize { width: 1, height: 1 };
        let gpu = surface_for_testing(size, 0x10);
        let same_gpu = surface_for_testing(size, 0x10);
        let cpu = surface_for_testing(size, 0);
        assert!(gpu.uses_gpu());
        assert!(!cpu.uses_gpu());
        assert!(gpu.shares_backend_with(&same_gpu));
        assert!(!gpu.shares_backend_with(&cpu));
    }
}
