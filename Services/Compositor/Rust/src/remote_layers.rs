/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The remote layers that root contexts are shown in, which the window server of the presentation that made them shows
//! in views of their own, and the window resizes that hold what the window servers show until the presentation commits
//! them. The window servers and the layers are C++ objects, which the raster host works on.

use crate::cpp_object::{CppObject, FfiCppObject};
use crate::host::PresentationId;
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::ipc_types::{Compositing_RemoteLayerHostHandle, Compositing_RemoteLayerHostKind};
use libgfx_rust::IntSize;
use std::collections::HashMap;
use std::rc::Rc;

/// A `Compositor::WindowServer`, of which the core owns a reference.
pub struct WindowServerObject(CppObject);

impl WindowServerObject {
    /// # Safety
    ///
    /// A non-null object must be a live `Compositor::WindowServer` that is handed over, with the function that gives up
    /// the reference.
    pub unsafe fn adopt(object: FfiCppObject) -> Option<Self> {
        // SAFETY: The caller hands over the window server.
        unsafe { CppObject::adopt(object) }.map(Self)
    }

    pub fn as_ptr(&self) -> *mut std::ffi::c_void {
        self.0.as_ptr()
    }
}

/// A `Compositor::RemoteLayer`, which the core owns.
pub struct RemoteLayerObject(CppObject);

impl RemoteLayerObject {
    /// # Safety
    ///
    /// A non-null object must be a live `Compositor::RemoteLayer` that is handed over, with the function that destroys
    /// it.
    pub unsafe fn adopt(object: FfiCppObject) -> Option<Self> {
        // SAFETY: The caller hands over the layer.
        unsafe { CppObject::adopt(object) }.map(Self)
    }

    pub fn as_ptr(&self) -> *mut std::ffi::c_void {
        self.0.as_ptr()
    }
}

/// What is done to the window servers and the layers.
pub trait RemoteLayerHost {
    fn create_window_server(&self) -> Option<WindowServerObject>;
    fn create_remote_layer(
        &self,
        window_server: &WindowServerObject,
        remote_layer_id: u64,
    ) -> Option<(RemoteLayerObject, Compositing_RemoteLayerHostHandle)>;
    fn place_frame(&self, remote_layer: &RemoteLayerObject, fit_within: Option<IntSize>);
    fn hold_changes_on_screen(&self, window_server: &WindowServerObject);
    fn release_changes_held_off_screen(&self, window_server: &WindowServerObject);
}

/// A resize of a remote layer that the presentation is to hear of.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ResizeNotice {
    pub presentation: PresentationId,
    pub remote_layer_id: u64,
    pub token: u64,
}

/// The contexts that a change of what a remote layer shows starts or stops showing in one.
#[derive(Debug, Default, PartialEq, Eq)]
pub struct ShownContextChange {
    pub stopped: Option<CompositorContextId>,
    pub started: Option<CompositorContextId>,
}

/// The layer that shows a context, and the window server it is in.
#[derive(Clone)]
pub struct ShowingLayer {
    pub layer: Rc<RemoteLayerObject>,
    pub window_server: Rc<WindowServerObject>,
}

pub struct RemoteLayer {
    presentation: PresentationId,
    layer: Rc<RemoteLayerObject>,
    window_server: Rc<WindowServerObject>,
    shown_context: Option<CompositorContextId>,
    fit_within: Option<IntSize>,
    // The size of the frame shown, where the page laid it out at that size.
    size_of_shown_frame_laid_out_by_page: Option<IntSize>,
}

impl RemoteLayer {
    pub fn showing_layer(&self) -> ShowingLayer {
        ShowingLayer {
            layer: self.layer.clone(),
            window_server: self.window_server.clone(),
        }
    }

    pub fn presentation(&self) -> PresentationId {
        self.presentation
    }

    pub fn fit_within(&self) -> Option<IntSize> {
        self.fit_within
    }
}

struct SynchronizedResize {
    remote_layer_id: u64,
    token: u64,
    awaited_size: Option<IntSize>,
    frame_is_held: bool,
}

#[derive(Default)]
pub struct RemoteLayers {
    window_servers: HashMap<PresentationId, Rc<WindowServerObject>>,
    layers: HashMap<u64, RemoteLayer>,
    layer_showing_context: HashMap<CompositorContextId, u64>,
    synchronized_resizes: Vec<SynchronizedResize>,
    window_servers_holding_changes: Vec<Rc<WindowServerObject>>,
}

fn no_host_handle() -> Compositing_RemoteLayerHostHandle {
    Compositing_RemoteLayerHostHandle {
        kind: Compositing_RemoteLayerHostKind::None,
        id: 0,
    }
}

impl RemoteLayers {
    pub fn register_presentation(&mut self, presentation: PresentationId, host: &dyn RemoteLayerHost) {
        if let Some(window_server) = host.create_window_server() {
            self.window_servers.insert(presentation, Rc::new(window_server));
        }
    }

    /// Destroys the layers of the presentation, answering the contexts that were shown in them.
    pub fn unregister_presentation(
        &mut self,
        presentation: PresentationId,
        host: &dyn RemoteLayerHost,
    ) -> Vec<CompositorContextId> {
        let mut remote_layer_ids: Vec<u64> = self
            .layers
            .iter()
            .filter(|(_, layer)| layer.presentation == presentation)
            .map(|(&id, _)| id)
            .collect();
        remote_layer_ids.sort_unstable();
        let stopped = remote_layer_ids
            .into_iter()
            .filter_map(|remote_layer_id| self.destroy_remote_layer(remote_layer_id, host))
            .collect();
        if let Some(window_server) = self.window_servers.remove(&presentation)
            && let Some(index) = self
                .window_servers_holding_changes
                .iter()
                .position(|holding| Rc::ptr_eq(holding, &window_server))
        {
            self.window_servers_holding_changes.remove(index);
            host.release_changes_held_off_screen(&window_server);
        }
        stopped
    }

    /// The host handle that the presentation shows the layer by, whose kind is none where the presentation has no
    /// remote layers or the layer exists already.
    pub fn create_remote_layer(
        &mut self,
        presentation: PresentationId,
        remote_layer_id: u64,
        host: &dyn RemoteLayerHost,
    ) -> Compositing_RemoteLayerHostHandle {
        if self.layers.contains_key(&remote_layer_id) {
            return no_host_handle();
        }
        let Some(window_server) = self.window_servers.get(&presentation).cloned() else {
            return no_host_handle();
        };
        let Some((layer, host_handle)) = host.create_remote_layer(&window_server, remote_layer_id) else {
            return no_host_handle();
        };
        self.layers.insert(
            remote_layer_id,
            RemoteLayer {
                presentation,
                layer: Rc::new(layer),
                window_server,
                shown_context: None,
                fit_within: None,
                size_of_shown_frame_laid_out_by_page: None,
            },
        );
        host_handle
    }

    /// Answers the context that the layer showed.
    pub fn destroy_remote_layer(
        &mut self,
        remote_layer_id: u64,
        host: &dyn RemoteLayerHost,
    ) -> Option<CompositorContextId> {
        let stopped = self
            .show_context_in_remote_layer(remote_layer_id, None, |_| false)
            .stopped;
        self.layers.remove(&remote_layer_id);
        let had_synchronized_resizes = !self.synchronized_resizes.is_empty();
        self.synchronized_resizes
            .retain(|resize| resize.remote_layer_id != remote_layer_id);
        if had_synchronized_resizes && self.synchronized_resizes.is_empty() {
            self.release_synchronized_resize_hold(host);
        }
        stopped
    }

    /// Shows the context in the layer instead of what it showed, where `can_be_shown` lets it. A context moves from
    /// any other layer it was shown in.
    pub fn show_context_in_remote_layer(
        &mut self,
        remote_layer_id: u64,
        context_id: Option<CompositorContextId>,
        can_be_shown: impl FnOnce(CompositorContextId) -> bool,
    ) -> ShownContextChange {
        let mut change = ShownContextChange::default();
        let Some(layer) = self.layers.get_mut(&remote_layer_id) else {
            return change;
        };
        if layer.shown_context == context_id {
            return change;
        }
        layer.size_of_shown_frame_laid_out_by_page = None;
        if let Some(shown_context) = layer.shown_context.take() {
            self.layer_showing_context.remove(&shown_context);
            change.stopped = Some(shown_context);
        }
        let Some(context_id) = context_id else {
            return change;
        };
        if !can_be_shown(context_id) {
            return change;
        }
        if let Some(other_remote_layer_id) = self.layer_showing_context.get(&context_id).copied()
            && let Some(other_layer) = self.layers.get_mut(&other_remote_layer_id)
        {
            other_layer.shown_context = None;
            other_layer.size_of_shown_frame_laid_out_by_page = None;
        }
        if let Some(layer) = self.layers.get_mut(&remote_layer_id) {
            layer.shown_context = Some(context_id);
        }
        self.layer_showing_context.insert(context_id, remote_layer_id);
        change.started = Some(context_id);
        change
    }

    /// The layer keeps the last frame of a context that is gone until another context is shown in it.
    pub fn context_destroyed(&mut self, context_id: CompositorContextId) {
        if let Some(remote_layer_id) = self.layer_showing_context.remove(&context_id)
            && let Some(layer) = self.layers.get_mut(&remote_layer_id)
        {
            layer.shown_context = None;
        }
    }

    /// The context is no longer shown in the layer that showed it, as when the layer could not show its frame.
    pub fn stop_showing_context(&mut self, context_id: CompositorContextId) {
        if let Some(remote_layer_id) = self.layer_showing_context.remove(&context_id)
            && let Some(layer) = self.layers.get_mut(&remote_layer_id)
        {
            layer.shown_context = None;
            layer.size_of_shown_frame_laid_out_by_page = None;
        }
    }

    pub fn layer_showing(&self, context_id: CompositorContextId) -> Option<&RemoteLayer> {
        self.layers.get(self.layer_showing_context.get(&context_id)?)
    }

    pub fn set_remote_layer_fit(
        &mut self,
        remote_layer_id: u64,
        fit_within: Option<IntSize>,
        host: &dyn RemoteLayerHost,
    ) {
        let Some(layer) = self.layers.get_mut(&remote_layer_id) else {
            return;
        };
        if layer.fit_within == fit_within {
            return;
        }
        layer.fit_within = fit_within;
        host.place_frame(&layer.layer, fit_within);
    }

    pub fn is_holding_changes(&self) -> bool {
        !self.window_servers_holding_changes.is_empty()
    }

    pub fn begin_synchronized_resize(&mut self, remote_layer_id: u64, token: u64, host: &dyn RemoteLayerHost) {
        let Some(layer) = self.layers.get(&remote_layer_id) else {
            return;
        };
        if !self
            .window_servers_holding_changes
            .iter()
            .any(|holding| Rc::ptr_eq(holding, &layer.window_server))
        {
            self.window_servers_holding_changes.push(layer.window_server.clone());
            host.hold_changes_on_screen(&layer.window_server);
        }
        self.synchronized_resizes.push(SynchronizedResize {
            remote_layer_id,
            token,
            awaited_size: None,
            frame_is_held: false,
        });
    }

    pub fn await_synchronized_resize_frame(
        &mut self,
        remote_layer_id: u64,
        token: u64,
        size: IntSize,
    ) -> Vec<ResizeNotice> {
        for resize in &mut self.synchronized_resizes {
            if resize.remote_layer_id == remote_layer_id && resize.token == token {
                resize.awaited_size = Some(size);
            }
        }
        self.held_synchronized_resize_frames(remote_layer_id)
    }

    /// The resizes of the layer whose frame it now shows, which the presentation is told of once.
    fn held_synchronized_resize_frames(&mut self, remote_layer_id: u64) -> Vec<ResizeNotice> {
        let mut held = Vec::new();
        let Some(layer) = self.layers.get(&remote_layer_id) else {
            return held;
        };
        let shown_size = layer.size_of_shown_frame_laid_out_by_page;
        for resize in &mut self.synchronized_resizes {
            if resize.remote_layer_id != remote_layer_id
                || resize.frame_is_held
                || resize.awaited_size.is_none()
                || resize.awaited_size != shown_size
            {
                continue;
            }
            resize.frame_is_held = true;
            held.push(ResizeNotice {
                presentation: layer.presentation,
                remote_layer_id,
                token: resize.token,
            });
        }
        held
    }

    pub fn commit_synchronized_resize(
        &mut self,
        remote_layer_id: u64,
        token: u64,
        host: &dyn RemoteLayerHost,
    ) -> Option<ResizeNotice> {
        self.synchronized_resizes
            .retain(|resize| !(resize.remote_layer_id == remote_layer_id && resize.token == token));
        if self.synchronized_resizes.is_empty() {
            self.release_synchronized_resize_hold(host);
        }
        let layer = self.layers.get(&remote_layer_id)?;
        Some(ResizeNotice {
            presentation: layer.presentation,
            remote_layer_id,
            token,
        })
    }

    pub fn release_synchronized_resize_hold(&mut self, host: &dyn RemoteLayerHost) {
        self.synchronized_resizes.clear();
        for window_server in std::mem::take(&mut self.window_servers_holding_changes) {
            host.release_changes_held_off_screen(&window_server);
        }
    }

    /// The layer that shows the context showed a frame of it, which the page laid out at the size it was painted at
    /// where `laid_out_by_page` says so. Answers the resizes whose frame the layer now shows.
    pub fn did_show_frame(
        &mut self,
        context_id: CompositorContextId,
        painted_size: IntSize,
        laid_out_by_page: bool,
    ) -> Vec<ResizeNotice> {
        let Some(remote_layer_id) = self.layer_showing_context.get(&context_id).copied() else {
            return Vec::new();
        };
        if let Some(layer) = self.layers.get_mut(&remote_layer_id) {
            layer.size_of_shown_frame_laid_out_by_page = laid_out_by_page.then_some(painted_size);
        }
        self.held_synchronized_resize_frames(remote_layer_id)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::cpp_object::testing::object_for_testing;
    use std::cell::{Cell, RefCell};

    #[derive(Default)]
    struct RecordingHost {
        next_address: Cell<usize>,
        held: RefCell<Vec<usize>>,
        released: RefCell<Vec<usize>>,
        placed: RefCell<Vec<Option<IntSize>>>,
    }

    impl RecordingHost {
        fn next_object(&self) -> CppObject {
            self.next_address.set(self.next_address.get() + 0x10);
            object_for_testing(self.next_address.get())
        }
    }

    impl RemoteLayerHost for RecordingHost {
        fn create_window_server(&self) -> Option<WindowServerObject> {
            Some(WindowServerObject(self.next_object()))
        }

        fn create_remote_layer(
            &self,
            _: &WindowServerObject,
            remote_layer_id: u64,
        ) -> Option<(RemoteLayerObject, Compositing_RemoteLayerHostHandle)> {
            Some((
                RemoteLayerObject(self.next_object()),
                Compositing_RemoteLayerHostHandle {
                    kind: Compositing_RemoteLayerHostKind::CoreAnimationContext,
                    id: remote_layer_id,
                },
            ))
        }

        fn place_frame(&self, _: &RemoteLayerObject, fit_within: Option<IntSize>) {
            self.placed.borrow_mut().push(fit_within);
        }

        fn hold_changes_on_screen(&self, window_server: &WindowServerObject) {
            self.held.borrow_mut().push(window_server.as_ptr() as usize);
        }

        fn release_changes_held_off_screen(&self, window_server: &WindowServerObject) {
            self.released.borrow_mut().push(window_server.as_ptr() as usize);
        }
    }

    const PRESENTATION: PresentationId = PresentationId(1);
    const CONTEXT: CompositorContextId = CompositorContextId(7);
    const SIZE: IntSize = IntSize {
        width: 800,
        height: 600,
    };

    fn layers_with_one_layer(host: &RecordingHost) -> RemoteLayers {
        let mut layers = RemoteLayers::default();
        layers.register_presentation(PRESENTATION, host);
        let handle = layers.create_remote_layer(PRESENTATION, 1, host);
        assert_eq!(handle.id, 1);
        layers
    }

    #[test]
    fn a_layer_is_created_once_per_id() {
        let host = RecordingHost::default();
        let mut layers = layers_with_one_layer(&host);
        assert_eq!(
            layers.create_remote_layer(PRESENTATION, 1, &host).kind,
            Compositing_RemoteLayerHostKind::None
        );
        assert_eq!(
            layers.create_remote_layer(PresentationId(2), 2, &host).kind,
            Compositing_RemoteLayerHostKind::None
        );
    }

    #[test]
    fn a_context_moves_between_layers() {
        let host = RecordingHost::default();
        let mut layers = layers_with_one_layer(&host);
        layers.create_remote_layer(PRESENTATION, 2, &host);
        assert_eq!(
            layers.show_context_in_remote_layer(1, Some(CONTEXT), |_| true),
            ShownContextChange {
                stopped: None,
                started: Some(CONTEXT)
            }
        );
        layers.show_context_in_remote_layer(2, Some(CONTEXT), |_| true);
        assert_eq!(layers.layers[&1].shown_context, None);
        assert_eq!(
            layers.layer_showing(CONTEXT).map(|layer| layer.presentation()),
            Some(PRESENTATION)
        );
        assert_eq!(layers.destroy_remote_layer(2, &host), Some(CONTEXT));
        assert!(layers.layer_showing(CONTEXT).is_none());
    }

    #[test]
    fn a_context_that_cannot_be_shown_is_ignored() {
        let host = RecordingHost::default();
        let mut layers = layers_with_one_layer(&host);
        let change = layers.show_context_in_remote_layer(1, Some(CONTEXT), |_| false);
        assert_eq!(change, ShownContextChange::default());
        assert!(layers.layer_showing(CONTEXT).is_none());
    }

    #[test]
    fn a_resize_is_held_until_the_frame_of_its_size_is_shown_and_released_when_committed() {
        let host = RecordingHost::default();
        let mut layers = layers_with_one_layer(&host);
        layers.show_context_in_remote_layer(1, Some(CONTEXT), |_| true);
        layers.begin_synchronized_resize(1, 5, &host);
        assert!(layers.is_holding_changes());
        assert_eq!(host.held.borrow().len(), 1);
        assert!(layers.await_synchronized_resize_frame(1, 5, SIZE).is_empty());
        assert!(layers.did_show_frame(CONTEXT, SIZE, false).is_empty());
        let held = layers.did_show_frame(CONTEXT, SIZE, true);
        assert_eq!(held.len(), 1);
        assert_eq!(held[0].token, 5);
        assert!(layers.did_show_frame(CONTEXT, SIZE, true).is_empty());
        let committed = layers.commit_synchronized_resize(1, 5, &host);
        assert_eq!(committed.map(|notice| notice.token), Some(5));
        assert!(!layers.is_holding_changes());
        assert_eq!(host.released.borrow().len(), 1);
    }

    #[test]
    fn destroying_the_presentation_releases_what_its_window_server_holds() {
        let host = RecordingHost::default();
        let mut layers = layers_with_one_layer(&host);
        layers.show_context_in_remote_layer(1, Some(CONTEXT), |_| true);
        layers.begin_synchronized_resize(1, 5, &host);
        assert_eq!(layers.unregister_presentation(PRESENTATION, &host), vec![CONTEXT]);
        assert!(!layers.is_holding_changes());
        assert_eq!(host.released.borrow().len(), 1);
    }

    #[test]
    fn the_frame_is_placed_again_only_when_the_fit_changes() {
        let host = RecordingHost::default();
        let mut layers = layers_with_one_layer(&host);
        layers.set_remote_layer_fit(1, Some(SIZE), &host);
        layers.set_remote_layer_fit(1, Some(SIZE), &host);
        assert_eq!(*host.placed.borrow(), vec![Some(SIZE)]);
    }
}
