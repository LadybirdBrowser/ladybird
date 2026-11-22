/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The sublayers a remote layer shows, each as native layers of its window server, which a frame changes only where it
//! differs from the frame before: a sublayer keeps its native layers from one frame to the next by its identity, and a
//! group, or the frame itself, is given its sublayers again only where they changed.

use super::{FrameSublayer, PlacedContent, SublayerContent};
use crate::surfaces::presentable_surface::PresentableSurfaceLease;
use std::collections::{HashMap, HashSet};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum SublayerKind {
    /// A video frame or a surface, placed and clipped as drawing it into the frame would place and clip it.
    PlacedContent,
    Group,
    Tile,
    Color,
    Backdrop,
}

impl SublayerKind {
    fn of<Surface, Placed>(content: &SublayerContent<Surface, Placed>) -> Self {
        match content {
            SublayerContent::Group(_) => SublayerKind::Group,
            SublayerContent::Tile { .. } => SublayerKind::Tile,
            SublayerContent::Color { .. } => SublayerKind::Color,
            SublayerContent::Backdrop(_) => SublayerKind::Backdrop,
            SublayerContent::Placed { .. } => SublayerKind::PlacedContent,
        }
    }
}

/// The native layers of a window server that show sublayers, and the frame they are shown in.
pub(super) trait NativeSublayers {
    type Native;

    /// New native layers for a sublayer of the kind, or none where they could not be made.
    fn create(&mut self, kind: SublayerKind) -> Option<Self::Native>;
    /// Shows the content in native layers made for its kind.
    fn update(&mut self, native: &mut Self::Native, content: SublayerContent<PresentableSurfaceLease, PlacedContent>);
    /// Stops showing native layers that no sublayer shows anymore. A group has no sublayers by then.
    fn stop_showing(&mut self, native: Self::Native);
    /// Takes the sublayers out of a group, or out of the frame, which is given them again by `add_sublayers_to_*`
    /// once every group whose sublayers changed has been emptied: a native layer can be in only one place at once.
    fn remove_sublayers_of_group(&mut self, group: &Self::Native);
    fn remove_sublayers_of_frame(&mut self);
    /// Gives a group, or the frame, its sublayers, bottom to top, in place of those it had.
    fn add_sublayers_to_group(&mut self, group: &Self::Native, sublayers: &[&Self::Native]);
    fn add_sublayers_to_frame(&mut self, sublayers: &[&Self::Native]);
}

struct ShownSublayer<Native> {
    kind: SublayerKind,
    native: Native,
    /// Whether the native layers are in the group the sublayer is in, or in the frame, rather than in none, as new ones
    /// are, or as those of a group that was emptied are.
    is_in_its_group: bool,
    /// Of a group, the sublayers in it, bottom to top.
    identities_of_sublayers: Vec<u64>,
}

pub(super) struct ShownSublayers<Native> {
    shown: HashMap<u64, ShownSublayer<Native>>,
    /// The sublayers of the frame itself, bottom to top.
    identities_of_sublayers_of_frame: Vec<u64>,
}

impl<Native> Default for ShownSublayers<Native> {
    fn default() -> Self {
        Self {
            shown: HashMap::new(),
            identities_of_sublayers_of_frame: Vec::new(),
        }
    }
}

impl<Native> ShownSublayers<Native> {
    /// Shows the sublayers of a frame, bottom to top, each group with the sublayers in it, in place of those of the
    /// frame before.
    pub(super) fn show(&mut self, natives: &mut impl NativeSublayers<Native = Native>, sublayers: Vec<FrameSublayer>) {
        let count = sublayers.len();
        // Per sublayer, by its index, the identities of the sublayers in it where it is a group, and after those, the
        // identities of the sublayers of the frame itself.
        let index_of_frame = count;
        let mut identities_of_sublayers_by_group_index: Vec<Vec<u64>> = vec![Vec::new(); count + 1];
        let mut group_has_sublayer_out_of_it = vec![false; count + 1];
        let mut kinds = Vec::with_capacity(count);
        let mut identities = Vec::with_capacity(count);
        let mut shown_identities = HashSet::with_capacity(count);
        for (index, sublayer) in sublayers.into_iter().enumerate() {
            let kind = SublayerKind::of(&sublayer.content);
            kinds.push(kind);
            identities.push(sublayer.identity);
            let Some(shown) = self.shown_sublayer_of_kind(natives, sublayer.identity, kind) else {
                continue;
            };
            natives.update(&mut shown.native, sublayer.content);
            let is_in_its_group = shown.is_in_its_group;
            // A group comes before the sublayers in it.
            let group_index = sublayer
                .group_index
                .filter(|&group_index| group_index < index && kinds[group_index] == SublayerKind::Group)
                .unwrap_or(index_of_frame);
            identities_of_sublayers_by_group_index[group_index].push(sublayer.identity);
            if !is_in_its_group {
                group_has_sublayer_out_of_it[group_index] = true;
            }
            shown_identities.insert(sublayer.identity);
        }

        let identities_no_longer_shown: Vec<u64> = self
            .shown
            .keys()
            .filter(|identity| !shown_identities.contains(identity))
            .copied()
            .collect();
        for identity in identities_no_longer_shown {
            if let Some(shown) = self.shown.remove(&identity) {
                self.stop_showing(natives, shown);
            }
        }

        // Every group whose sublayers changed is emptied before any is given its sublayers again.
        let mut indices_of_changed_groups = Vec::new();
        for index in 0..=count {
            let changed = |shown_identities_of_sublayers: &Vec<u64>| {
                group_has_sublayer_out_of_it[index]
                    || *shown_identities_of_sublayers != identities_of_sublayers_by_group_index[index]
            };
            if index == index_of_frame {
                if !changed(&self.identities_of_sublayers_of_frame) {
                    continue;
                }
                natives.remove_sublayers_of_frame();
                let emptied = std::mem::take(&mut self.identities_of_sublayers_of_frame);
                self.mark_out_of_their_group(&emptied);
            } else {
                if kinds[index] != SublayerKind::Group {
                    continue;
                }
                let Some(group) = self
                    .shown
                    .get_mut(&identities[index])
                    .filter(|group| group.kind == SublayerKind::Group)
                else {
                    continue;
                };
                if !changed(&group.identities_of_sublayers) {
                    continue;
                }
                natives.remove_sublayers_of_group(&group.native);
                let emptied = std::mem::take(&mut group.identities_of_sublayers);
                self.mark_out_of_their_group(&emptied);
            }
            indices_of_changed_groups.push(index);
        }
        for index in indices_of_changed_groups {
            let identities_of_sublayers = std::mem::take(&mut identities_of_sublayers_by_group_index[index]);
            {
                let sublayers: Vec<&Native> = identities_of_sublayers
                    .iter()
                    .filter_map(|identity| self.shown.get(identity).map(|shown| &shown.native))
                    .collect();
                if index == index_of_frame {
                    natives.add_sublayers_to_frame(&sublayers);
                } else if let Some(group) = self.shown.get(&identities[index]) {
                    natives.add_sublayers_to_group(&group.native, &sublayers);
                }
            }
            for identity in &identities_of_sublayers {
                if let Some(shown) = self.shown.get_mut(identity) {
                    shown.is_in_its_group = true;
                }
            }
            if index == index_of_frame {
                self.identities_of_sublayers_of_frame = identities_of_sublayers;
            } else if let Some(group) = self.shown.get_mut(&identities[index]) {
                group.identities_of_sublayers = identities_of_sublayers;
            }
        }
    }

    /// Runs `visit` on the native layers of every sublayer shown.
    #[cfg(windows)]
    pub(super) fn for_each_native_mut(&mut self, mut visit: impl FnMut(&mut Native)) {
        for shown in self.shown.values_mut() {
            visit(&mut shown.native);
        }
    }

    /// Stops showing every sublayer, which leaves the frame without any.
    pub(super) fn stop_showing_all(&mut self, natives: &mut impl NativeSublayers<Native = Native>) {
        natives.remove_sublayers_of_frame();
        self.identities_of_sublayers_of_frame.clear();
        let identities: Vec<u64> = self.shown.keys().copied().collect();
        for identity in identities {
            if let Some(shown) = self.shown.remove(&identity) {
                self.stop_showing(natives, shown);
            }
        }
    }

    fn shown_sublayer_of_kind(
        &mut self,
        natives: &mut impl NativeSublayers<Native = Native>,
        identity: u64,
        kind: SublayerKind,
    ) -> Option<&mut ShownSublayer<Native>> {
        if self.shown.get(&identity).is_some_and(|shown| shown.kind != kind)
            && let Some(shown) = self.shown.remove(&identity)
        {
            self.stop_showing(natives, shown);
        }
        match self.shown.entry(identity) {
            std::collections::hash_map::Entry::Occupied(shown) => Some(shown.into_mut()),
            std::collections::hash_map::Entry::Vacant(entry) => Some(entry.insert(ShownSublayer {
                kind,
                native: natives.create(kind)?,
                is_in_its_group: false,
                identities_of_sublayers: Vec::new(),
            })),
        }
    }

    fn stop_showing(&mut self, natives: &mut impl NativeSublayers<Native = Native>, shown: ShownSublayer<Native>) {
        if shown.kind == SublayerKind::Group {
            natives.remove_sublayers_of_group(&shown.native);
            self.mark_out_of_their_group(&shown.identities_of_sublayers);
        }
        natives.stop_showing(shown.native);
    }

    fn mark_out_of_their_group(&mut self, identities: &[u64]) {
        for identity in identities {
            if let Some(shown) = self.shown.get_mut(identity) {
                shown.is_in_its_group = false;
            }
        }
    }
}
