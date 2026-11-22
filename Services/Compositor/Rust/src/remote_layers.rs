/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The remote layers that root contexts are shown in, which the window server of the presentation that made them shows
//! in views of their own, and the window resizes that hold what the window servers show until the presentation commits
//! them.

use crate::host::PresentationId;
use crate::window_server::{RemoteLayer as WindowServerLayer, WindowServer};
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::ipc_types::{Compositing_RemoteLayerHostHandle, Compositing_RemoteLayerHostKind};
use libgfx_rust::IntSize;
use std::collections::HashMap;
use std::rc::Rc;

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
    pub layer: Rc<dyn WindowServerLayer>,
    pub window_server: Rc<dyn WindowServer>,
}

pub struct RemoteLayer {
    presentation: PresentationId,
    layer: Rc<dyn WindowServerLayer>,
    window_server: Rc<dyn WindowServer>,
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
    window_servers: HashMap<PresentationId, Rc<dyn WindowServer>>,
    layers: HashMap<u64, RemoteLayer>,
    layer_showing_context: HashMap<CompositorContextId, u64>,
    synchronized_resizes: Vec<SynchronizedResize>,
    window_servers_holding_changes: Vec<Rc<dyn WindowServer>>,
}

fn no_host_handle() -> Compositing_RemoteLayerHostHandle {
    Compositing_RemoteLayerHostHandle {
        kind: Compositing_RemoteLayerHostKind::None,
        id: 0,
    }
}

impl RemoteLayers {
    /// Registers the presentation with its window server, which it has none of where frames reach the screen only
    /// through it.
    pub fn register_presentation(&mut self, presentation: PresentationId, window_server: Option<Rc<dyn WindowServer>>) {
        if let Some(window_server) = window_server {
            self.window_servers.insert(presentation, window_server);
        }
    }

    /// Destroys the layers of the presentation, answering the contexts that were shown in them.
    pub fn unregister_presentation(&mut self, presentation: PresentationId) -> Vec<CompositorContextId> {
        let mut remote_layer_ids: Vec<u64> = self
            .layers
            .iter()
            .filter(|(_, layer)| layer.presentation == presentation)
            .map(|(&id, _)| id)
            .collect();
        remote_layer_ids.sort_unstable();
        let stopped = remote_layer_ids
            .into_iter()
            .filter_map(|remote_layer_id| self.destroy_remote_layer(remote_layer_id))
            .collect();
        if let Some(window_server) = self.window_servers.remove(&presentation)
            && let Some(index) = self
                .window_servers_holding_changes
                .iter()
                .position(|holding| Rc::ptr_eq(holding, &window_server))
        {
            self.window_servers_holding_changes.remove(index);
            window_server.release_changes_held_off_screen();
        }
        stopped
    }

    /// The host handle that the presentation shows the layer by, whose kind is none where the presentation has no
    /// remote layers or the layer exists already.
    pub fn create_remote_layer(
        &mut self,
        presentation: PresentationId,
        remote_layer_id: u64,
    ) -> Compositing_RemoteLayerHostHandle {
        if self.layers.contains_key(&remote_layer_id) {
            return no_host_handle();
        }
        let Some(window_server) = self.window_servers.get(&presentation).cloned() else {
            return no_host_handle();
        };
        let Some(layer) = window_server.create_remote_layer() else {
            return no_host_handle();
        };
        let host_handle = layer.host_handle();
        self.layers.insert(
            remote_layer_id,
            RemoteLayer {
                presentation,
                layer,
                window_server,
                shown_context: None,
                fit_within: None,
                size_of_shown_frame_laid_out_by_page: None,
            },
        );
        host_handle
    }

    /// Answers the context that the layer showed.
    pub fn destroy_remote_layer(&mut self, remote_layer_id: u64) -> Option<CompositorContextId> {
        let stopped = self
            .show_context_in_remote_layer(remote_layer_id, None, |_| false)
            .stopped;
        self.layers.remove(&remote_layer_id);
        let had_synchronized_resizes = !self.synchronized_resizes.is_empty();
        self.synchronized_resizes
            .retain(|resize| resize.remote_layer_id != remote_layer_id);
        if had_synchronized_resizes && self.synchronized_resizes.is_empty() {
            self.release_synchronized_resize_hold();
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

    pub fn set_remote_layer_fit(&mut self, remote_layer_id: u64, fit_within: Option<IntSize>) {
        let Some(layer) = self.layers.get_mut(&remote_layer_id) else {
            return;
        };
        if layer.fit_within == fit_within {
            return;
        }
        layer.fit_within = fit_within;
        layer.layer.place_frame(fit_within);
    }

    /// Has every layer let go of what it stopped showing once the window server can no longer be reading it, and
    /// answers how soon to ask again, where a layer still holds some.
    pub fn release_content_window_server_let_go_of(&self) -> Option<std::time::Duration> {
        self.layers
            .values()
            .filter_map(|layer| layer.layer.release_content_window_server_let_go_of())
            .min()
    }

    pub fn is_holding_changes(&self) -> bool {
        !self.window_servers_holding_changes.is_empty()
    }

    pub fn begin_synchronized_resize(&mut self, remote_layer_id: u64, token: u64) {
        let Some(layer) = self.layers.get(&remote_layer_id) else {
            return;
        };
        if !self
            .window_servers_holding_changes
            .iter()
            .any(|holding| Rc::ptr_eq(holding, &layer.window_server))
        {
            self.window_servers_holding_changes.push(layer.window_server.clone());
            layer.window_server.hold_changes_on_screen();
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

    pub fn commit_synchronized_resize(&mut self, remote_layer_id: u64, token: u64) -> Option<ResizeNotice> {
        self.synchronized_resizes
            .retain(|resize| !(resize.remote_layer_id == remote_layer_id && resize.token == token));
        if self.synchronized_resizes.is_empty() {
            self.release_synchronized_resize_hold();
        }
        let layer = self.layers.get(&remote_layer_id)?;
        Some(ResizeNotice {
            presentation: layer.presentation,
            remote_layer_id,
            token,
        })
    }

    pub fn release_synchronized_resize_hold(&mut self) {
        self.synchronized_resizes.clear();
        for window_server in std::mem::take(&mut self.window_servers_holding_changes) {
            window_server.release_changes_held_off_screen();
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
    use crate::raster_host::FrameHandOff;
    use crate::window_server::{Frame, MediaToShow};
    use libcompositing_rust::display_list::layers::WindowServerCompositing;
    use libcompositing_rust::display_list::window_server_placement::WindowServerPlacement;
    use std::cell::{Cell, RefCell};

    #[derive(Default)]
    struct Recording {
        next_host_handle_id: Cell<u64>,
        holds: Cell<usize>,
        releases: Cell<usize>,
        placed: RefCell<Vec<Option<IntSize>>>,
    }

    struct RecordingWindowServer(Rc<Recording>);

    struct RecordingLayer {
        recording: Rc<Recording>,
        host_handle_id: u64,
    }

    impl WindowServer for RecordingWindowServer {
        fn compositing(&self) -> WindowServerCompositing {
            WindowServerCompositing::default()
        }

        fn frame_hand_off(&self) -> FrameHandOff {
            FrameHandOff::OnceGpuWorkIsScheduled
        }

        fn create_remote_layer(&self) -> Option<Rc<dyn WindowServerLayer>> {
            let host_handle_id = self.0.next_host_handle_id.get() + 1;
            self.0.next_host_handle_id.set(host_handle_id);
            Some(Rc::new(RecordingLayer {
                recording: self.0.clone(),
                host_handle_id,
            }))
        }

        fn hold_changes_on_screen(&self) {
            self.0.holds.set(self.0.holds.get() + 1);
        }

        fn release_changes_held_off_screen(&self) {
            self.0.releases.set(self.0.releases.get() + 1);
        }
    }

    impl WindowServerLayer for RecordingLayer {
        fn host_handle(&self) -> Compositing_RemoteLayerHostHandle {
            Compositing_RemoteLayerHostHandle {
                kind: Compositing_RemoteLayerHostKind::CoreAnimationContext,
                id: self.host_handle_id,
            }
        }

        fn can_show(&self, _: &WindowServerPlacement, _: MediaToShow<'_>) -> bool {
            true
        }

        fn show_frame(&self, _: Frame) -> bool {
            true
        }

        fn place_frame(&self, fit_within: Option<IntSize>) {
            self.recording.placed.borrow_mut().push(fit_within);
        }
    }

    const PRESENTATION: PresentationId = PresentationId(1);
    const CONTEXT: CompositorContextId = CompositorContextId(7);
    const SIZE: IntSize = IntSize {
        width: 800,
        height: 600,
    };

    fn layers_with_one_layer(recording: &Rc<Recording>) -> RemoteLayers {
        let mut layers = RemoteLayers::default();
        layers.register_presentation(PRESENTATION, Some(Rc::new(RecordingWindowServer(recording.clone()))));
        let handle = layers.create_remote_layer(PRESENTATION, 1);
        assert_eq!(handle.id, 1);
        layers
    }

    #[test]
    fn a_layer_is_created_once_per_id() {
        let recording = Rc::new(Recording::default());
        let mut layers = layers_with_one_layer(&recording);
        assert_eq!(
            layers.create_remote_layer(PRESENTATION, 1).kind,
            Compositing_RemoteLayerHostKind::None
        );
        assert_eq!(
            layers.create_remote_layer(PresentationId(2), 2).kind,
            Compositing_RemoteLayerHostKind::None
        );
    }

    #[test]
    fn a_context_moves_between_layers() {
        let recording = Rc::new(Recording::default());
        let mut layers = layers_with_one_layer(&recording);
        layers.create_remote_layer(PRESENTATION, 2);
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
        assert_eq!(layers.destroy_remote_layer(2), Some(CONTEXT));
        assert!(layers.layer_showing(CONTEXT).is_none());
    }

    #[test]
    fn a_context_that_cannot_be_shown_is_ignored() {
        let recording = Rc::new(Recording::default());
        let mut layers = layers_with_one_layer(&recording);
        let change = layers.show_context_in_remote_layer(1, Some(CONTEXT), |_| false);
        assert_eq!(change, ShownContextChange::default());
        assert!(layers.layer_showing(CONTEXT).is_none());
    }

    #[test]
    fn a_resize_is_held_until_the_frame_of_its_size_is_shown_and_released_when_committed() {
        let recording = Rc::new(Recording::default());
        let mut layers = layers_with_one_layer(&recording);
        layers.show_context_in_remote_layer(1, Some(CONTEXT), |_| true);
        layers.begin_synchronized_resize(1, 5);
        assert!(layers.is_holding_changes());
        assert_eq!(recording.holds.get(), 1);
        assert!(layers.await_synchronized_resize_frame(1, 5, SIZE).is_empty());
        assert!(layers.did_show_frame(CONTEXT, SIZE, false).is_empty());
        let held = layers.did_show_frame(CONTEXT, SIZE, true);
        assert_eq!(held.len(), 1);
        assert_eq!(held[0].token, 5);
        assert!(layers.did_show_frame(CONTEXT, SIZE, true).is_empty());
        let committed = layers.commit_synchronized_resize(1, 5);
        assert_eq!(committed.map(|notice| notice.token), Some(5));
        assert!(!layers.is_holding_changes());
        assert_eq!(recording.releases.get(), 1);
    }

    #[test]
    fn destroying_the_presentation_releases_what_its_window_server_holds() {
        let recording = Rc::new(Recording::default());
        let mut layers = layers_with_one_layer(&recording);
        layers.show_context_in_remote_layer(1, Some(CONTEXT), |_| true);
        layers.begin_synchronized_resize(1, 5);
        assert_eq!(layers.unregister_presentation(PRESENTATION), vec![CONTEXT]);
        assert!(!layers.is_holding_changes());
        assert_eq!(recording.releases.get(), 1);
    }

    #[test]
    fn the_frame_is_placed_again_only_when_the_fit_changes() {
        let recording = Rc::new(Recording::default());
        let mut layers = layers_with_one_layer(&recording);
        layers.set_remote_layer_fit(1, Some(SIZE));
        layers.set_remote_layer_fit(1, Some(SIZE));
        assert_eq!(*recording.placed.borrow(), vec![Some(SIZE)]);
    }
}
