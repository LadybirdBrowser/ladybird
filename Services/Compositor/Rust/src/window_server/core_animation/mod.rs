/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The window server of macOS, driven through CoreAnimation: each remote layer is a layer tree in a `CAContext`, which a
//! `CALayerHost` of the presentation shows by the context's ID.

mod frameworks;
mod objc;
mod path;
mod quartz_core;
mod remote_layer;
mod video;

use super::{RemoteLayer, WindowServer};
use crate::raster_host::FrameHandOff;
use libcompositing_rust::display_list::layers::WindowServerCompositing;
use libgfx_rust::IntSize;
use objc::AutoreleasePool;
use std::cell::Cell;
use std::rc::Rc;
use std::time::Instant;

pub use frameworks::IOSurfaceRef;

/// Makes the changes in a transaction of their own, which reaches the window server once they are made.
pub(super) fn commit_layer_changes<R>(change: impl FnOnce() -> R) -> R {
    let _pool = AutoreleasePool::push();
    quartz_core::transaction::begin();
    quartz_core::transaction::set_disable_actions(true);
    let result = change();
    quartz_core::transaction::commit();
    quartz_core::transaction::flush();
    result
}

// While the window servers hold changes off screen, the transactions of each change nest in outer ones until those are
// committed, so none of them reaches the window server before then. Every window server shares the transactions of the
// thread, so they share this as well.
thread_local! {
    static OUTER_TRANSACTIONS_HOLDING_CHANGES: Cell<u32> = const { Cell::new(0) };
    static CHANGES_LAST_RELEASED_ON_SCREEN: Cell<Option<Instant>> = const { Cell::new(None) };
}

pub(super) fn changes_are_held_off_screen() -> bool {
    OUTER_TRANSACTIONS_HOLDING_CHANGES.get() > 0
}

pub(super) fn changes_last_released_on_screen() -> Option<Instant> {
    CHANGES_LAST_RELEASED_ON_SCREEN.get()
}

pub(super) struct CoreAnimationWindowServer;

impl CoreAnimationWindowServer {
    /// None where the layer trees that another process shows by the ID of their context are not available.
    pub(super) fn create() -> Option<Rc<dyn WindowServer>> {
        if !quartz_core::contexts_are_available() {
            return None;
        }
        Some(Rc::new(CoreAnimationWindowServer))
    }
}

impl WindowServer for CoreAnimationWindowServer {
    fn compositing(&self) -> WindowServerCompositing {
        WindowServerCompositing {
            shows_tiles_of_painted_layers: true,
            filters_what_is_below_layers: quartz_core::backdrop_filters_are_available(),
        }
    }

    fn frame_hand_off(&self) -> FrameHandOff {
        FrameHandOff::OnceGpuWorkIsScheduled
    }

    fn create_remote_layer(&self) -> Option<Rc<dyn RemoteLayer>> {
        remote_layer::CoreAnimationRemoteLayer::create().map(|layer| Rc::new(layer) as Rc<dyn RemoteLayer>)
    }

    fn hold_changes_on_screen(&self) {
        quartz_core::transaction::begin();
        OUTER_TRANSACTIONS_HOLDING_CHANGES.set(OUTER_TRANSACTIONS_HOLDING_CHANGES.get() + 1);
    }

    fn release_changes_held_off_screen(&self) {
        let holding = OUTER_TRANSACTIONS_HOLDING_CHANGES.get();
        if holding == 0 {
            return;
        }
        quartz_core::transaction::commit();
        quartz_core::transaction::flush();
        OUTER_TRANSACTIONS_HOLDING_CHANGES.set(holding - 1);
        CHANGES_LAST_RELEASED_ON_SCREEN.set(Some(Instant::now()));
    }
}

pub(super) fn size_of_io_surface(surface: IOSurfaceRef) -> IntSize {
    // SAFETY: The surface is live.
    unsafe {
        IntSize {
            width: i32::try_from(frameworks::IOSurfaceGetWidth(surface)).unwrap_or(i32::MAX),
            height: i32::try_from(frameworks::IOSurfaceGetHeight(surface)).unwrap_or(i32::MAX),
        }
    }
}

/// An IOSurface of which this holds one reference.
pub struct IOSurface(frameworks::CfRetained);

impl IOSurface {
    /// # Safety
    ///
    /// A non-null surface must be a live IOSurface.
    pub unsafe fn retain(surface: IOSurfaceRef) -> Option<Self> {
        // SAFETY: The caller passes a live IOSurface.
        unsafe { frameworks::CfRetained::retain(surface.cast_const()) }.map(Self)
    }

    pub fn as_ptr(&self) -> IOSurfaceRef {
        self.0.as_mut_ptr()
    }

    /// Whether the surface is still read by the window server, or by anything else that shares it.
    pub fn is_in_use(&self) -> bool {
        // SAFETY: The surface is live.
        unsafe { frameworks::IOSurfaceIsInUse(self.as_ptr()) != 0 }
    }
}
