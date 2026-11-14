/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The display lists that the display lists of a context nest, such as those of its SVG images, which the replays of
//! its frames draw.

use libcompositing_rust::display_list::replay::{NestedDisplayList, NestedDisplayLists};
use libcompositing_rust::display_list::storage::DisplayListStorage;
use libcompositing_rust::display_list::summary::requires_direct_replay_without_nested_lists;
use libcompositing_rust::ipc_types::NestedDisplayListResource;
use libcompositing_rust::visual_context::VisualContextTree;
use libgfx_rust::Color;
use std::cell::RefCell;
use std::collections::{HashMap, HashSet};
use std::sync::Arc;

#[derive(Clone)]
struct StoredDisplayList {
    storage: Arc<DisplayListStorage>,
    tree: Arc<VisualContextTree>,
    root_group_color: Color,
}

/// The fonts and image frames that a display list draws, with those of the lists it nests, and the lists it nests.
#[derive(Default)]
pub struct ReferencedResources {
    pub font_ids: HashSet<u64>,
    pub image_frame_ids: HashSet<u64>,
    pub display_list_ids: HashSet<u64>,
}

#[derive(Clone, Default)]
pub struct NestedDisplayListStore {
    display_lists: HashMap<u64, StoredDisplayList>,
    // Whether a raster of a list can differ from replaying it, which is intrinsic to the list and the lists it nests.
    requires_direct_replay: RefCell<HashMap<u64, bool>>,
}

impl NestedDisplayListStore {
    /// A list that the store holds already keeps the tape it was added with.
    pub fn add(&mut self, resource: NestedDisplayListResource) {
        if self.display_lists.contains_key(&resource.properties.id) {
            return;
        }
        self.display_lists.insert(
            resource.properties.id,
            StoredDisplayList {
                storage: resource.storage,
                tree: resource.tree,
                root_group_color: resource.properties.root_group_color,
            },
        );
        self.requires_direct_replay.get_mut().clear();
    }

    /// Adds the lists, answering false, with none of them added, where one of them would reach itself, as replay follows
    /// nested lists with no depth limit.
    pub fn add_all(&mut self, resources: Vec<NestedDisplayListResource>) -> bool {
        let mut added = Vec::new();
        for resource in resources {
            let display_list_id = resource.properties.id;
            if self.display_lists.contains_key(&display_list_id) {
                continue;
            }
            self.add(resource);
            added.push(display_list_id);
        }
        // NB: A list held already does not reach itself, so only a new list can close a loop.
        if !added
            .iter()
            .any(|&display_list_id| self.reaches_itself(display_list_id))
        {
            return true;
        }
        for display_list_id in added {
            self.remove(display_list_id);
        }
        false
    }

    fn reaches_itself(&self, display_list_id: u64) -> bool {
        let stored = &self.display_lists[&display_list_id];
        let mut reached = ReferencedResources::default();
        self.collect_referenced_resources(&stored.storage, &stored.tree, &mut reached);
        reached.display_list_ids.contains(&display_list_id)
    }

    pub fn remove(&mut self, display_list_id: u64) {
        if self.display_lists.remove(&display_list_id).is_some() {
            self.requires_direct_replay.get_mut().clear();
        }
    }

    pub fn contains(&self, display_list_id: u64) -> bool {
        self.display_lists.contains_key(&display_list_id)
    }

    /// Adds what the list draws, and what the lists it nests that the store holds draw.
    pub fn collect_referenced_resources(
        &self,
        storage: &DisplayListStorage,
        tree: &VisualContextTree,
        resources: &mut ReferencedResources,
    ) {
        let summary = storage.summary();
        resources.font_ids.extend(&summary.font_ids);
        resources.image_frame_ids.extend(&summary.image_frame_ids);
        tree.for_each_effects_filter_image_frame_id(&mut |id| {
            resources.image_frame_ids.insert(id);
        });
        for &display_list_id in &summary.display_list_ids {
            if !resources.display_list_ids.insert(display_list_id) {
                continue;
            }
            if let Some(nested) = self.display_lists.get(&display_list_id) {
                self.collect_referenced_resources(&nested.storage, &nested.tree, resources);
            }
        }
    }

    /// Whether the lists that a list nests are all held, and none of them reaches itself, as replay follows nested lists
    /// with no depth limit.
    pub fn nested_display_lists_are_replayable(&self, resources: &ReferencedResources) -> bool {
        resources.display_list_ids.iter().all(|&display_list_id| {
            self.display_lists.contains_key(&display_list_id) && !self.reaches_itself(display_list_id)
        })
    }

    // A raster of a list can differ from replaying it in place when a destination-reading operation can see what was
    // painted before the list began, or when the list draws live content that changes under its immutable commands.
    // A nested list's live content matters at any depth, and its unisolated destination reads matter when its command
    // is not enclosed in a layer; recursing unconditionally is slightly conservative for the latter.
    fn list_requires_direct_replay(&self, display_list_id: u64, visited: &mut HashSet<u64>) -> bool {
        if let Some(&memoized) = self.requires_direct_replay.borrow().get(&display_list_id) {
            return memoized;
        }
        visited.insert(display_list_id);
        let stored = &self.display_lists[&display_list_id];
        let mut requires_direct_replay = requires_direct_replay_without_nested_lists(
            stored.storage.bytes(),
            stored.storage.command_runs(),
            &stored.tree,
        );
        for &nested_id in &stored.storage.summary().display_list_ids {
            if requires_direct_replay {
                break;
            }
            if !visited.insert(nested_id) {
                continue;
            }
            if self.display_lists.contains_key(&nested_id) {
                requires_direct_replay = self.list_requires_direct_replay(nested_id, visited);
            }
        }
        self.requires_direct_replay
            .borrow_mut()
            .insert(display_list_id, requires_direct_replay);
        requires_direct_replay
    }
}

impl NestedDisplayLists for NestedDisplayListStore {
    fn nested_display_list(&self, display_list_id: u64) -> Option<NestedDisplayList<'_>> {
        let stored = self.display_lists.get(&display_list_id)?;
        Some(NestedDisplayList {
            storage: &stored.storage,
            tree: &stored.tree,
            root_group_color: stored.root_group_color,
            requires_direct_replay: self.list_requires_direct_replay(display_list_id, &mut HashSet::new()),
        })
    }
}
