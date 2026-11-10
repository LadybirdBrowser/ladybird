/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Everything about a list that travels alongside its tape when it is sent to the Compositor.

use super::ContextRef;
use crate::display_list::builder::read_command;
use crate::display_list::commands::{DisplayListCommandType, PaintCaret};
use crate::display_list::storage::DisplayListStorage;
use crate::display_list::summary::{for_each_compositor_metadata, for_each_indexed_record};
use libgfx_rust::IntRect;
use std::sync::Arc;

pub use crate::ipc_types::Compositing_DisplayList_AsyncScrollingMetadata as AsyncScrollingMetadata;

pub struct DisplayList {
    // One strong reference to the Rust storage, which owns the tape.
    pub(crate) storage: Arc<DisplayListStorage>,
    id: u64,
    compatible_visual_context_tree_structural_epoch: u64,
    async_scrolling_metadata: Option<AsyncScrollingMetadata>,
}

impl DisplayList {
    pub fn new(
        storage: Arc<DisplayListStorage>,
        id: u64,
        compatible_visual_context_tree_structural_epoch: u64,
        async_scrolling_metadata: Option<AsyncScrollingMetadata>,
    ) -> Self {
        Self {
            storage,
            id,
            compatible_visual_context_tree_structural_epoch,
            async_scrolling_metadata,
        }
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
                visit(context, header.bounding_rect, &command);
            },
        );
    }
}
