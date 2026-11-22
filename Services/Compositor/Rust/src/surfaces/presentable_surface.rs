/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A surface whose content the window server can show as it is, instead of after it is composited into a frame, which
//! the core and the C++ around it share by reference counting: a pointer to a `Compositor::PresentableSurface` is the
//! `PresentableSurface` of an `Rc`. While a lease of it is held, as by a frame on its way to the window server and by
//! the layer that shows it, or while the window server still reads it, the surface is busy, and its content must stay
//! as it is. Only the Compositor's main thread may use a surface.

use crate::cpp_object::{CppObject, FfiCppObject};
use crate::surfaces::painting_surface::PaintingSurface;
use libgfx_rust::IntSize;
use std::cell::Cell;
use std::ffi::c_void;
use std::rc::{Rc, Weak};

/// What a presentable surface is to the window server, as the C++ side makes it.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum WindowServerSurfaceKind {
    /// Memory that the window server cannot read.
    None,
    /// An IOSurface, on macOS.
    IOSurface,
    /// A Direct3D 12 texture shared by an NT handle, on Windows, which the window server reads as it is where it can,
    /// and which is copied into a swap chain to be shown otherwise.
    D3DSharedTexture,
    /// Memory on the GPU that the window server cannot read, on Windows, which is copied into a swap chain to be shown.
    CopiedIntoSwapChain,
}

/// The part of a presentable surface that the window server shows, as the C++ side hands it over.
#[repr(C)]
pub struct FfiWindowServerSurface {
    pub kind: WindowServerSurfaceKind,
    /// The IOSurface, or the NT handle of the texture, which lives for as long as the owner does, or as the painting
    /// surface does where there is no owner.
    pub handle: *mut c_void,
    /// What keeps the handle alive where the surface owns that, or null.
    pub owner: FfiCppObject,
}

/// The part of a presentable surface that the window server shows.
pub enum WindowServerSurface {
    #[cfg(target_os = "macos")]
    IOSurface {
        surface: crate::window_server::IOSurface,
        _owner: Option<CppObject>,
    },
    #[cfg(windows)]
    SharedTexture(crate::window_server::SharedTexture),
    #[cfg(windows)]
    CopiedIntoSwapChain,
}

impl WindowServerSurface {
    /// # Safety
    ///
    /// The handle must be of the kind, and live for as long as the owner, or the painting surface where there is none.
    unsafe fn from_ffi(kind: WindowServerSurfaceKind, handle: *mut c_void, owner: Option<CppObject>) -> Option<Self> {
        #[cfg(target_os = "macos")]
        if kind == WindowServerSurfaceKind::IOSurface {
            return Some(WindowServerSurface::IOSurface {
                // SAFETY: The caller passes a live IOSurface.
                surface: unsafe { crate::window_server::IOSurface::retain(handle) }?,
                _owner: owner,
            });
        }
        #[cfg(windows)]
        match kind {
            WindowServerSurfaceKind::D3DSharedTexture => {
                // SAFETY: The caller passes the NT handle of the texture that the owner owns.
                let texture = unsafe { crate::window_server::SharedTexture::new(handle, owner?) };
                return Some(WindowServerSurface::SharedTexture(texture));
            }
            WindowServerSurfaceKind::CopiedIntoSwapChain => return Some(WindowServerSurface::CopiedIntoSwapChain),
            _ => {}
        }
        // No other kind is shown by the window server of this system.
        let _ = (kind, handle, owner);
        None
    }

    fn may_be_read_by_window_server(&self) -> bool {
        match *self {
            #[cfg(target_os = "macos")]
            WindowServerSurface::IOSurface { ref surface, .. } => surface.is_in_use(),
            #[cfg(windows)]
            WindowServerSurface::SharedTexture(ref texture) => texture.may_be_read_by_window_server(),
            // The swap chain a surface is copied into is another surface.
            #[cfg(windows)]
            WindowServerSurface::CopiedIntoSwapChain => false,
        }
    }
}

pub struct PresentableSurface {
    painting_surface: Rc<PaintingSurface>,
    content_is_upside_down: bool,
    window_server_surface: Option<WindowServerSurface>,
    was_handed_to_window_server: Cell<bool>,
    lease_count: Cell<usize>,
}

impl PresentableSurface {
    pub fn painting_surface(&self) -> &Rc<PaintingSurface> {
        &self.painting_surface
    }

    pub fn size(&self) -> IntSize {
        self.painting_surface.size()
    }

    pub fn as_ffi(self: &Rc<Self>) -> *const PresentableSurface {
        Rc::as_ptr(self)
    }

    /// A surface over the painting surface that draws into memory the window server cannot read.
    pub fn shown_by_no_window_server(painting_surface: Rc<PaintingSurface>) -> Rc<PresentableSurface> {
        Rc::new(PresentableSurface {
            painting_surface,
            content_is_upside_down: false,
            window_server_surface: None,
            was_handed_to_window_server: Cell::new(false),
            lease_count: Cell::new(0),
        })
    }

    pub fn can_be_shown_by_window_server(&self) -> bool {
        self.window_server_surface.is_some()
    }

    pub fn content_is_upside_down(&self) -> bool {
        self.content_is_upside_down
    }

    /// The IOSurface the window server shows the surface from, where it is one.
    #[cfg(target_os = "macos")]
    pub fn io_surface(&self) -> Option<crate::window_server::IOSurfaceRef> {
        self.window_server_surface
            .as_ref()
            .map(|WindowServerSurface::IOSurface { surface, .. }| surface.as_ptr())
    }

    /// The texture the window server shows the surface from as it is, where it is one it can.
    #[cfg(windows)]
    pub fn shared_texture(&self) -> Option<&crate::window_server::SharedTexture> {
        match &self.window_server_surface {
            Some(WindowServerSurface::SharedTexture(texture)) => Some(texture),
            _ => None,
        }
    }

    /// GPU work in flight on a surface counts as use too, so only a surface handed to the window server is checked.
    pub fn did_hand_to_window_server(&self) {
        self.was_handed_to_window_server.set(true);
    }

    /// # Safety
    ///
    /// `surface` must be a live surface that the caller lends, or null.
    pub unsafe fn retained_from_ffi(surface: *const PresentableSurface) -> Option<Rc<PresentableSurface>> {
        if surface.is_null() {
            return None;
        }
        // SAFETY: The caller lends the PresentableSurface of a live Rc, of which this takes a reference of its own.
        unsafe {
            Rc::increment_strong_count(surface);
            Some(Rc::from_raw(surface))
        }
    }

    /// # Safety
    ///
    /// `surface` must be a surface whose one reference the caller hands over, or null.
    pub unsafe fn adopted_from_ffi(surface: *const PresentableSurface) -> Option<Rc<PresentableSurface>> {
        if surface.is_null() {
            return None;
        }
        // SAFETY: The caller hands over one reference to the PresentableSurface of a live Rc.
        Some(unsafe { Rc::from_raw(surface) })
    }

    // GPU work in flight on a surface counts as use too, so only a surface handed to the window server is checked.
    pub fn is_read_by_window_server(&self) -> bool {
        self.was_handed_to_window_server.get()
            && self
                .window_server_surface
                .as_ref()
                .is_some_and(WindowServerSurface::may_be_read_by_window_server)
    }

    pub fn is_busy(&self) -> bool {
        self.lease_count.get() > 0 || self.is_read_by_window_server()
    }

    pub fn lease(self: &Rc<Self>) -> PresentableSurfaceLease {
        self.lease_count.set(self.lease_count.get() + 1);
        PresentableSurfaceLease(self.clone())
    }
}

impl Drop for PresentableSurface {
    fn drop(&mut self) {
        assert_eq!(self.lease_count.get(), 0, "a lease holds the surface");
    }
}

/// A surface is the same as no other surface, whatever it holds.
impl PartialEq for PresentableSurface {
    fn eq(&self, other: &Self) -> bool {
        std::ptr::eq(self, other)
    }
}

impl std::fmt::Debug for PresentableSurface {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        formatter
            .debug_struct("PresentableSurface")
            .field("address", &std::ptr::from_ref(self))
            .field("size", &self.size())
            .finish()
    }
}

/// Keeps the content of the surface as it is for as long as it is held. Holding the surface itself does not.
#[derive(Debug)]
pub struct PresentableSurfaceLease(Rc<PresentableSurface>);

impl PresentableSurfaceLease {
    pub fn surface(&self) -> &Rc<PresentableSurface> {
        &self.0
    }
}

impl Clone for PresentableSurfaceLease {
    fn clone(&self) -> Self {
        self.0.lease()
    }
}

impl Drop for PresentableSurfaceLease {
    fn drop(&mut self) {
        let count = self.0.lease_count.get();
        assert!(count > 0, "the lease counts towards the surface");
        self.0.lease_count.set(count - 1);
    }
}

/// The surfaces that one piece of content, such as a canvas, is presented from in turn. The content goes into its
/// latest surface again unless that one is busy, so content that is only ever composited into frames keeps a single
/// surface.
#[derive(Default)]
pub struct PresentableSurfacePool {
    // In the order the content went into them, the latest last.
    surfaces: Vec<Rc<PresentableSurface>>,
}

// A surface is only added while every other one is busy, so a pool holds about as many as are on their way to the
// window server or still read by it at once, which is a few. The limit only stops a pool whose surfaces never stop being
// busy from growing without end.
const MAXIMUM_SURFACE_COUNT_OF_POOL: usize = 8;

impl PresentableSurfacePool {
    /// A surface of the size for the next content, rendered with the GPU backend context given, which is never one that
    /// is busy. None when every surface is busy and the pool is full, or when `create` makes none, and the content then
    /// has to wait for a later turn.
    pub fn surface_for_next_content(
        &mut self,
        size: IntSize,
        gpu_backend_context: *const c_void,
        create: impl FnOnce() -> Option<Rc<PresentableSurface>>,
    ) -> Option<Rc<PresentableSurface>> {
        let fits = |surface: &PresentableSurface| {
            surface.size() == size && surface.painting_surface.gpu_backend_context() == gpu_backend_context
        };
        self.surfaces.retain(|surface| fits(surface) || surface.is_busy());

        let can_take_next_content = |surface: &PresentableSurface| fits(surface) && !surface.is_busy();
        if let Some(latest) = self.surfaces.last()
            && can_take_next_content(latest)
        {
            return Some(latest.clone());
        }
        if let Some(index) = self.surfaces.iter().position(|surface| can_take_next_content(surface)) {
            let surface = self.surfaces.remove(index);
            self.surfaces.push(surface.clone());
            return Some(surface);
        }

        if self.surfaces.len() >= MAXIMUM_SURFACE_COUNT_OF_POOL {
            return None;
        }
        let surface = create()?;
        self.surfaces.push(surface.clone());
        Some(surface)
    }
}

/// A new surface over the painting surface, whose one reference the caller owns and gives up with
/// `compositor_presentable_surface_release()`. It takes over the owner of the window server's part, if there is one.
///
/// # Safety
///
/// `painting_surface` must be a live surface. The window server's part must be of its kind, and a non-null owner of it
/// must be live and destroyed by its destroy function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_presentable_surface_create(
    painting_surface: *const PaintingSurface,
    content_is_upside_down: bool,
    window_server_surface: FfiWindowServerSurface,
) -> *const PresentableSurface {
    // SAFETY: The caller passes a live surface, which the presentable surface holds.
    let painting_surface = unsafe { PaintingSurface::retained_from_ffi(painting_surface) };
    // SAFETY: The caller hands over the owner, and vouches for the handle.
    let window_server_surface = unsafe {
        let owner = CppObject::adopt(window_server_surface.owner);
        WindowServerSurface::from_ffi(window_server_surface.kind, window_server_surface.handle, owner)
    };
    Rc::into_raw(Rc::new(PresentableSurface {
        painting_surface,
        content_is_upside_down,
        window_server_surface,
        was_handed_to_window_server: Cell::new(false),
        lease_count: Cell::new(0),
    }))
}

/// Whether the window server can show the surfaces it is given shared with it as they are, as textures shared between
/// the Skia context on the adapter, an `IDXGIAdapter1`, and itself.
///
/// # Safety
///
/// `adapter` must be a live `IDXGIAdapter1`.
#[cfg(windows)]
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_window_server_can_show_surfaces_shared_with_it(adapter: *mut c_void) -> bool {
    // SAFETY: The caller passes a live adapter.
    unsafe { crate::window_server::can_show_surfaces_shared_with_it(adapter) }
}

/// # Safety
///
/// `surface` must be a live surface.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_presentable_surface_retain(surface: *const PresentableSurface) {
    assert!(!surface.is_null(), "the C++ side passes a surface");
    // SAFETY: The caller passes the PresentableSurface of a live Rc.
    unsafe { Rc::increment_strong_count(surface) };
}

/// # Safety
///
/// `surface` must be a live surface of which the caller owns a reference.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_presentable_surface_release(surface: *const PresentableSurface) {
    assert!(!surface.is_null(), "the C++ side passes a surface");
    // SAFETY: The caller gives up one of its references to the PresentableSurface of a live Rc.
    unsafe { Rc::decrement_strong_count(surface) };
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_presentable_surface_painting_surface(
    surface: &PresentableSurface,
) -> *const PaintingSurface {
    surface.painting_surface.as_ffi()
}

#[unsafe(no_mangle)]
pub extern "C" fn compositor_presentable_surface_is_busy(surface: &PresentableSurface) -> bool {
    surface.is_busy()
}

pub struct WeakPresentableSurface(Weak<PresentableSurface>);

/// A weak reference to the surface, which `compositor_weak_presentable_surface_destroy()` gives up.
///
/// # Safety
///
/// `surface` must be a live surface.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_presentable_surface_downgrade(
    surface: *const PresentableSurface,
) -> *mut WeakPresentableSurface {
    // SAFETY: The caller passes the PresentableSurface of a live Rc, which this borrows only to downgrade it.
    let surface = std::mem::ManuallyDrop::new(unsafe { Rc::from_raw(surface) });
    Box::into_raw(Box::new(WeakPresentableSurface(Rc::downgrade(&surface))))
}

/// A reference to the surface that the caller owns, or null once the surface is gone.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_weak_presentable_surface_upgrade(
    weak: &WeakPresentableSurface,
) -> *const PresentableSurface {
    weak.0.upgrade().map_or(std::ptr::null(), Rc::into_raw)
}

/// # Safety
///
/// `weak` must be null or a weak reference from `compositor_presentable_surface_downgrade()` that nothing uses
/// afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_weak_presentable_surface_destroy(weak: *mut WeakPresentableSurface) {
    if !weak.is_null() {
        // SAFETY: The caller hands over the weak reference.
        drop(unsafe { Box::from_raw(weak) });
    }
}

/// A pool of surfaces for C++ that presents content from one, which `compositor_presentable_surface_pool_destroy()`
/// destroys.
#[unsafe(no_mangle)]
pub extern "C" fn compositor_presentable_surface_pool_create() -> *mut PresentableSurfacePool {
    Box::into_raw(Box::default())
}

/// # Safety
///
/// `pool` must be null or a pool from `compositor_presentable_surface_pool_create()` that nothing uses afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_presentable_surface_pool_destroy(pool: *mut PresentableSurfacePool) {
    if !pool.is_null() {
        // SAFETY: The caller hands over the pool.
        drop(unsafe { Box::from_raw(pool) });
    }
}

/// A surface of the pool for the next content, with one reference that the caller takes over, or null when the content
/// has to wait. A new surface comes from `create`, which hands over one reference to it or answers null.
///
/// # Safety
///
/// `create` must answer as described while the call runs, and must not use the pool.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn compositor_presentable_surface_pool_surface_for_next_content(
    pool: &mut PresentableSurfacePool,
    size: IntSize,
    gpu_backend_context: *const c_void,
    create: unsafe extern "C" fn(*mut c_void) -> *const PresentableSurface,
    create_context: *mut c_void,
) -> *const PresentableSurface {
    pool.surface_for_next_content(size, gpu_backend_context, || {
        // SAFETY: The caller's `create` hands over a new surface or answers null.
        unsafe { PresentableSurface::adopted_from_ffi(create(create_context)) }
    })
    .map_or(std::ptr::null(), Rc::into_raw)
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::surfaces::painting_surface::testing::surface_for_testing;

    fn presentable_surface(size: IntSize) -> Rc<PresentableSurface> {
        let painting_surface = surface_for_testing(size, 0x10);
        // SAFETY: The painting surface is live, and there is no window server's part.
        let surface = unsafe {
            compositor_presentable_surface_create(
                painting_surface.as_ffi(),
                false,
                FfiWindowServerSurface {
                    kind: WindowServerSurfaceKind::None,
                    handle: std::ptr::null_mut(),
                    owner: FfiCppObject::default(),
                },
            )
        };
        // SAFETY: The surface was just made with one reference, which this takes over.
        unsafe { PresentableSurface::adopted_from_ffi(surface) }.expect("a new surface")
    }

    const SIZE: IntSize = IntSize { width: 8, height: 8 };
    const BACKEND: *const c_void = 0x10 as *const c_void;

    #[test]
    fn pool_reuses_its_latest_surface_while_nothing_holds_it() {
        let mut pool = PresentableSurfacePool::default();
        let first = pool
            .surface_for_next_content(SIZE, BACKEND, || Some(presentable_surface(SIZE)))
            .unwrap();
        let again = pool
            .surface_for_next_content(SIZE, BACKEND, || panic!("no new surface"))
            .unwrap();
        assert!(Rc::ptr_eq(&first, &again));
    }

    #[test]
    fn pool_adds_a_surface_while_the_others_are_leased() {
        let mut pool = PresentableSurfacePool::default();
        let first = pool
            .surface_for_next_content(SIZE, BACKEND, || Some(presentable_surface(SIZE)))
            .unwrap();
        let lease = first.lease();
        let second = pool
            .surface_for_next_content(SIZE, BACKEND, || Some(presentable_surface(SIZE)))
            .unwrap();
        assert!(!Rc::ptr_eq(&first, &second));
        drop(lease);
        let reused = pool
            .surface_for_next_content(SIZE, BACKEND, || panic!("no new surface"))
            .unwrap();
        assert!(Rc::ptr_eq(&second, &reused));
    }

    #[test]
    fn pool_stops_growing_when_every_surface_is_busy() {
        let mut pool = PresentableSurfacePool::default();
        let mut leases = Vec::new();
        for _ in 0..MAXIMUM_SURFACE_COUNT_OF_POOL {
            let surface = pool
                .surface_for_next_content(SIZE, BACKEND, || Some(presentable_surface(SIZE)))
                .unwrap();
            leases.push(surface.lease());
        }
        assert!(
            pool.surface_for_next_content(SIZE, BACKEND, || panic!("no new surface"))
                .is_none()
        );
    }

    #[test]
    fn pool_drops_idle_surfaces_of_another_size() {
        let mut pool = PresentableSurfacePool::default();
        let small = pool
            .surface_for_next_content(SIZE, BACKEND, || Some(presentable_surface(SIZE)))
            .unwrap();
        let weak = Rc::downgrade(&small);
        drop(small);
        let larger = IntSize { width: 16, height: 16 };
        pool.surface_for_next_content(larger, BACKEND, || Some(presentable_surface(larger)))
            .unwrap();
        assert!(weak.upgrade().is_none());
    }
}
