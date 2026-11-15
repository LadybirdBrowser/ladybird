/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What a display list draws besides its tape: fonts, image frames, video sinks and the display lists it nests, which
//! the list holds for as long as anything holds the list. The compositor is sent the ones it lacks ahead of the list.

use crate::painting::display_list::builder::RecordedDisplayList;
use crate::painting::display_list::ids::allocate_display_list_id;
use crate::painting::display_list::storage::DisplayListStorage;
use crate::painting::display_list::summary::{DisplayListSummary, summarize};
use crate::painting::visual_context::VisualContextTree;
use libcompositing_rust::ipc_types::{Compositing_DisplayListProperties, NestedDisplayListResource};
use libgfx_rust::font::FontHandle;
use libgfx_rust::image_frame::ImageFrameHandle;
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

/// The resources a display list draws, each under the id its commands name it by.
#[derive(Clone, Default)]
pub(crate) struct DisplayListResources {
    pub(crate) fonts: HashMap<u64, FontHandle>,
    pub(crate) image_frames: HashMap<u64, ImageFrameHandle>,
    /// The handle of each video sink.
    pub(crate) video_sinks: HashMap<u64, u64>,
    pub(crate) nested_display_lists: HashMap<u64, Arc<NestedDisplayList>>,
}

impl DisplayListResources {
    /// Takes what `other` holds that this does not.
    pub(crate) fn include(&mut self, other: &Self) {
        for (id, font) in &other.fonts {
            self.fonts.entry(*id).or_insert_with(|| font.clone());
        }
        for (id, frame) in &other.image_frames {
            self.image_frames.entry(*id).or_insert_with(|| frame.clone());
        }
        for (id, handle) in &other.video_sinks {
            self.video_sinks.entry(*id).or_insert(*handle);
        }
        for (id, display_list) in &other.nested_display_lists {
            self.nested_display_lists
                .entry(*id)
                .or_insert_with(|| display_list.clone());
        }
    }

    /// These and what the display lists they nest draw, at any depth: everything the compositor needs to replay a list
    /// that draws these.
    pub(crate) fn with_nested_resources(&self) -> Self {
        let mut closure = self.clone();
        let mut pending: Vec<Arc<NestedDisplayList>> = self.nested_display_lists.values().cloned().collect();
        let mut visited: HashSet<u64> = closure.nested_display_lists.keys().copied().collect();
        while let Some(display_list) = pending.pop() {
            closure.include_own(&display_list.resources);
            for (id, nested) in &display_list.resources.nested_display_lists {
                if visited.insert(*id) {
                    closure.nested_display_lists.insert(*id, nested.clone());
                    pending.push(nested.clone());
                }
            }
        }
        closure
    }

    fn include_own(&mut self, other: &Self) {
        let nested = std::mem::take(&mut self.nested_display_lists);
        self.include(&Self {
            nested_display_lists: HashMap::new(),
            ..other.clone()
        });
        self.nested_display_lists = nested;
    }

    /// Takes what `source` holds of what `summary` names and this lacks: what a recording copied from the one it read
    /// paint commands from draws.
    pub(crate) fn complete_from(&mut self, summary: &DisplayListSummary, source: &Self) {
        for id in &summary.font_ids {
            if let Some(font) = source.fonts.get(id) {
                self.fonts.entry(*id).or_insert_with(|| font.clone());
            }
        }
        for id in &summary.image_frame_ids {
            if let Some(frame) = source.image_frames.get(id) {
                self.image_frames.entry(*id).or_insert_with(|| frame.clone());
            }
        }
        for id in &summary.video_sink_ids {
            if let Some(handle) = source.video_sinks.get(id) {
                self.video_sinks.entry(*id).or_insert(*handle);
            }
        }
        for id in &summary.display_list_ids {
            if let Some(display_list) = source.nested_display_lists.get(id) {
                self.nested_display_lists
                    .entry(*id)
                    .or_insert_with(|| display_list.clone());
            }
        }
    }

    pub(crate) fn ids(&self) -> ResourceIds {
        ResourceIds {
            fonts: self.fonts.keys().copied().collect(),
            image_frames: self.image_frames.keys().copied().collect(),
            video_sinks: self.video_sinks.keys().copied().collect(),
            display_lists: self.nested_display_lists.keys().copied().collect(),
        }
    }
}

/// The ids of resources a receiver holds.
#[derive(Clone, Default, Debug, PartialEq, Eq)]
pub(crate) struct ResourceIds {
    pub(crate) fonts: HashSet<u64>,
    pub(crate) image_frames: HashSet<u64>,
    pub(crate) video_sinks: HashSet<u64>,
    pub(crate) display_lists: HashSet<u64>,
}

/// A display list another one nests, such as an SVG image or an image paint: what the compositor stores under its id,
/// and what it draws.
pub(crate) struct NestedDisplayList {
    pub(crate) resource: NestedDisplayListResource,
    pub(crate) resources: DisplayListResources,
}

impl NestedDisplayList {
    /// The list `display_list` recorded against `visual_context_tree`, under a new id, drawing `resources`.
    pub(crate) fn recorded(
        display_list: Arc<RecordedDisplayList>,
        visual_context_tree: Arc<VisualContextTree>,
        root_group_color: libgfx_rust::Color,
        resources: DisplayListResources,
    ) -> Arc<Self> {
        let storage = DisplayListStorage::recorded(display_list, visual_context_tree.clone());
        Self::new(storage, visual_context_tree, root_group_color, resources)
    }

    /// An empty list, which stands in for an SVG image that has nothing to show.
    pub(crate) fn empty() -> Arc<Self> {
        let storage = DisplayListStorage::empty();
        let visual_context_tree = storage
            .visual_context_tree()
            .cloned()
            .expect("an empty list holds the tree it was recorded against");
        Self::new(
            storage,
            visual_context_tree,
            libgfx_rust::Color::TRANSPARENT,
            DisplayListResources::default(),
        )
    }

    fn new(
        storage: Arc<DisplayListStorage>,
        visual_context_tree: Arc<VisualContextTree>,
        root_group_color: libgfx_rust::Color,
        resources: DisplayListResources,
    ) -> Arc<Self> {
        Arc::new(Self {
            resource: NestedDisplayListResource {
                properties: Compositing_DisplayListProperties {
                    id: allocate_display_list_id(),
                    compatible_visual_context_tree_structural_epoch: visual_context_tree.structural_epoch,
                    surface_clear_color: None,
                    root_group_color,
                },
                storage,
                tree: visual_context_tree,
            },
            resources,
        })
    }

    pub(crate) fn id(&self) -> u64 {
        self.resource.properties.id
    }
}

/// The ids of the resources the commands of `display_list` name.
pub(crate) fn summary_of(display_list: &RecordedDisplayList) -> DisplayListSummary {
    summarize(&display_list.bytes, &display_list.command_runs)
}
