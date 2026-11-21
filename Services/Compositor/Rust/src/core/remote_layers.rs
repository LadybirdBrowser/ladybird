/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the presentations ask of the remote layers that show their root contexts.

use super::CompositorCore;
use crate::host::{PresentationId, Timer};
use libcompositing_rust::display_list::commands::CompositorContextId;
use libcompositing_rust::ipc_types::{Compositing_RemoteLayerHostHandle, compositor_presentation_client};
use libcompositing_rust::time::{Duration, MonotonicTime};
use libgfx_rust::IntSize;
use std::io::Write;

// A presentation that never commits its resize, as one that crashed, would otherwise freeze every window of the browser.
const SYNCHRONIZED_RESIZE_HOLD_LIMIT: Duration = Duration::from_milliseconds(1000);

impl CompositorCore {
    pub fn create_remote_layer(
        &self,
        presentation_id: PresentationId,
        remote_layer_id: u64,
    ) -> Option<Compositing_RemoteLayerHostHandle> {
        let _entered = self.enter();

        if !self.presentations.borrow().contains_key(&presentation_id) {
            return None;
        }
        Some(
            self.remote_layers
                .borrow_mut()
                .create_remote_layer(presentation_id, remote_layer_id, &self.raster),
        )
    }

    pub fn destroy_remote_layer(&self, remote_layer_id: u64, now: MonotonicTime) {
        let _entered = self.enter();

        let was_holding_changes = self.remote_layers.borrow().is_holding_changes();
        let stopped = self
            .remote_layers
            .borrow_mut()
            .destroy_remote_layer(remote_layer_id, &self.raster);
        self.update_synchronized_resize_hold_timer(was_holding_changes);
        if let Some(context_id) = stopped {
            self.stop_showing_context_in_remote_layer(context_id, now);
        }
        self.finish_entry(now);
    }

    pub fn show_context_in_remote_layer(
        &self,
        remote_layer_id: u64,
        context_id: Option<CompositorContextId>,
        now: MonotonicTime,
    ) {
        let _entered = self.enter();

        let change =
            self.remote_layers
                .borrow_mut()
                .show_context_in_remote_layer(remote_layer_id, context_id, |context_id| {
                    let is_root_context = self
                        .context_if_present(context_id)
                        .is_some_and(|context| context.borrow().presents_to_client());
                    if !is_root_context {
                        let _ = writeln!(
                            std::io::stderr(),
                            "Compositor: Ignoring context {} for remote layer {}, as it is not a root context",
                            context_id.0,
                            remote_layer_id
                        );
                    }
                    is_root_context
                });
        if let Some(stopped) = change.stopped {
            self.stop_showing_context_in_remote_layer(stopped, now);
        }
        if let Some(started) = change.started {
            self.start_showing_context_in_remote_layer(started, now);
        }
        self.finish_entry(now);
    }

    pub fn set_remote_layer_fit(&self, remote_layer_id: u64, fit_within: Option<IntSize>) {
        let _entered = self.enter();

        self.remote_layers
            .borrow_mut()
            .set_remote_layer_fit(remote_layer_id, fit_within, &self.raster);
    }

    pub fn begin_synchronized_resize(&self, remote_layer_id: u64, token: u64) {
        let _entered = self.enter();

        let was_holding_changes = self.remote_layers.borrow().is_holding_changes();
        self.remote_layers
            .borrow_mut()
            .begin_synchronized_resize(remote_layer_id, token, &self.raster);
        self.update_synchronized_resize_hold_timer(was_holding_changes);
    }

    pub fn await_synchronized_resize_frame(&self, remote_layer_id: u64, token: u64, size: IntSize) {
        let _entered = self.enter();

        let held = self
            .remote_layers
            .borrow_mut()
            .await_synchronized_resize_frame(remote_layer_id, token, size);
        self.tell_of_held_synchronized_resize_frames(held);
    }

    pub fn commit_synchronized_resize(&self, remote_layer_id: u64, token: u64) {
        let _entered = self.enter();

        let was_holding_changes = self.remote_layers.borrow().is_holding_changes();
        let committed =
            self.remote_layers
                .borrow_mut()
                .commit_synchronized_resize(remote_layer_id, token, &self.raster);
        self.update_synchronized_resize_hold_timer(was_holding_changes);
        if let Some(notice) = committed {
            self.host.post_to_presentation(
                notice.presentation,
                compositor_presentation_client::messages::DidCommitSynchronizedResize {
                    remote_layer_id: notice.remote_layer_id,
                    token: notice.token,
                },
            );
        }
    }

    pub(super) fn synchronized_resize_hold_timed_out(&self) {
        let _ = writeln!(
            std::io::stderr(),
            "Compositor: Releasing the window resize that was never committed"
        );
        self.remote_layers
            .borrow_mut()
            .release_synchronized_resize_hold(&self.raster);
    }

    pub(super) fn update_synchronized_resize_hold_timer(&self, was_holding_changes: bool) {
        let is_holding_changes = self.remote_layers.borrow().is_holding_changes();
        if is_holding_changes && !was_holding_changes {
            self.host
                .schedule(Timer::SynchronizedResizeHold, SYNCHRONIZED_RESIZE_HOLD_LIMIT);
        } else if !is_holding_changes && was_holding_changes {
            self.host.unschedule(Timer::SynchronizedResizeHold);
        }
    }
}
