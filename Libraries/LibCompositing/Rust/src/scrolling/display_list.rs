/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Everything about a list that travels alongside its tape when it is sent to the Compositor.

use super::ContextRef;
use crate::display_list::builder::{bounding_rect_computed_from_payload, read_command};
use crate::display_list::commands::{DisplayListCommandType, PaintCaret};
use crate::display_list::storage::DisplayListStorage;
use crate::display_list::summary::{for_each_compositor_metadata, for_each_indexed_record};
use crate::display_list::window::RecordedWindow;
use libgfx_rust::{Color, IntRect};
use std::sync::Arc;

pub use crate::ipc_types::Compositing_AsyncScrollingMetadata as AsyncScrollingMetadata;

pub struct DisplayList {
    // One strong reference to the Rust storage, which owns the tape.
    pub(crate) storage: Arc<DisplayListStorage>,
    id: u64,
    compatible_visual_context_tree_structural_epoch: u64,
    async_scrolling_metadata: Option<AsyncScrollingMetadata>,
    surface_clear_color: Option<Color>,
    root_group_color: Color,
    window: Option<RecordedWindow>,
}

impl DisplayList {
    pub fn new(
        storage: Arc<DisplayListStorage>,
        id: u64,
        compatible_visual_context_tree_structural_epoch: u64,
        async_scrolling_metadata: Option<AsyncScrollingMetadata>,
        surface_clear_color: Option<Color>,
        root_group_color: Color,
    ) -> Self {
        Self {
            storage,
            id,
            compatible_visual_context_tree_structural_epoch,
            async_scrolling_metadata,
            surface_clear_color,
            root_group_color,
            window: None,
        }
    }

    /// The list, which holds what shows in `window` rather than the whole document where it has one.
    pub fn with_window(self, window: Option<RecordedWindow>) -> Self {
        Self { window, ..self }
    }

    pub fn window(&self) -> Option<&RecordedWindow> {
        self.window.as_ref()
    }

    pub fn storage(&self) -> &Arc<DisplayListStorage> {
        &self.storage
    }

    // What the surface a frame of the list is painted into is cleared to first, none for transparent.
    pub fn surface_clear_color(&self) -> Option<Color> {
        self.surface_clear_color
    }

    // What the backdrop of an effect directly under the root group reads, outside every layer.
    pub fn root_group_color(&self) -> Color {
        self.root_group_color
    }

    pub fn compatible_visual_context_tree_structural_epoch(&self) -> u64 {
        self.compatible_visual_context_tree_structural_epoch
    }

    pub fn id(&self) -> u64 {
        self.id
    }

    pub fn async_scrolling_metadata(&self) -> Option<&AsyncScrollingMetadata> {
        self.async_scrolling_metadata.as_ref()
    }

    // Visits the compositor metadata commands with the context of their run and their payload, in tape order.
    pub fn for_each_compositor_metadata(&self, visit: impl FnMut(ContextRef, DisplayListCommandType, &[u8])) {
        for_each_compositor_metadata(self.storage.bytes(), self.storage.command_runs(), visit);
    }

    // Visits the carets the list draws outside of any group, with the context of their run and their bounding rect.
    pub fn for_each_caret(&self, mut visit: impl FnMut(ContextRef, IntRect, &PaintCaret)) {
        for_each_indexed_record(
            self.storage.bytes(),
            self.storage.command_runs(),
            &self.storage.summary().carets,
            |context, header, payload| {
                let command = read_command::<PaintCaret>(payload);
                visit(
                    context,
                    bounding_rect_computed_from_payload(header, payload).unwrap_or_default(),
                    &command,
                );
            },
        );
    }
}
