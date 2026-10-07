/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};

use crate::css::computed_value_types::ComputedResolvedTransform;
use crate::layout::node_data::NodeSlotId;
use crate::painting::host::{FfiSvgGradientDescription, FfiSvgPatternDescription};
use crate::painting::svg_filter::SvgFilterPrimitive;
use libgfx_rust::Color;

#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub(crate) enum SvgPaintResourceKind {
    Filter,
    BackdropFilter,
    Fill,
    Stroke,
}

impl SvgPaintResourceKind {
    pub(crate) const ALL: [Self; 4] = [Self::Filter, Self::BackdropFilter, Self::Fill, Self::Stroke];

    pub(crate) const fn bit(self) -> u8 {
        1 << self as u8
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct PublishedSvgGradientStop {
    pub color: Color,
    pub position: f32,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PublishedSvgGradient {
    pub description: FfiSvgGradientDescription,
    pub stops: Vec<PublishedSvgGradientStop>,
}

#[derive(Clone, Debug, PartialEq)]
pub(crate) struct PublishedSvgPattern {
    pub description: FfiSvgPatternDescription,
    pub css_transform: Vec<ComputedResolvedTransform>,
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) enum PublishedSvgPaintServer {
    #[default]
    None,
    Gradient(PublishedSvgGradient),
    Pattern(PublishedSvgPattern),
}

#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct PublishedSvgFilter {
    pub failed: bool,
    pub primitives: Vec<SvgFilterPrimitive>,
}

#[derive(Clone, Default)]
pub(crate) struct SvgPaintResourceRow {
    enrolled_kinds: u8,
    filter: Option<Arc<PublishedSvgFilter>>,
    backdrop_filter: Option<Arc<PublishedSvgFilter>>,
    fill: Option<Arc<PublishedSvgPaintServer>>,
    stroke: Option<Arc<PublishedSvgPaintServer>>,
}

impl SvgPaintResourceRow {
    fn published_filter(&self, kind: SvgPaintResourceKind) -> Option<Arc<PublishedSvgFilter>> {
        match kind {
            SvgPaintResourceKind::Filter => self.filter.clone(),
            SvgPaintResourceKind::BackdropFilter => self.backdrop_filter.clone(),
            SvgPaintResourceKind::Fill | SvgPaintResourceKind::Stroke => unreachable!(),
        }
    }

    fn published_paint_server(&self, kind: SvgPaintResourceKind) -> Option<Arc<PublishedSvgPaintServer>> {
        match kind {
            SvgPaintResourceKind::Fill => self.fill.clone(),
            SvgPaintResourceKind::Stroke => self.stroke.clone(),
            SvgPaintResourceKind::Filter | SvgPaintResourceKind::BackdropFilter => unreachable!(),
        }
    }

    fn filter_entry(&mut self, kind: SvgPaintResourceKind) -> &mut Option<Arc<PublishedSvgFilter>> {
        match kind {
            SvgPaintResourceKind::Filter => &mut self.filter,
            SvgPaintResourceKind::BackdropFilter => &mut self.backdrop_filter,
            SvgPaintResourceKind::Fill | SvgPaintResourceKind::Stroke => unreachable!(),
        }
    }

    fn paint_server_entry(&mut self, kind: SvgPaintResourceKind) -> &mut Option<Arc<PublishedSvgPaintServer>> {
        match kind {
            SvgPaintResourceKind::Fill => &mut self.fill,
            SvgPaintResourceKind::Stroke => &mut self.stroke,
            SvgPaintResourceKind::Filter | SvgPaintResourceKind::BackdropFilter => unreachable!(),
        }
    }

    fn forget_published(&mut self, kinds: u8) {
        for kind in SvgPaintResourceKind::ALL {
            if kinds & kind.bit() == 0 {
                continue;
            }
            match kind {
                SvgPaintResourceKind::Filter | SvgPaintResourceKind::BackdropFilter => *self.filter_entry(kind) = None,
                SvgPaintResourceKind::Fill | SvgPaintResourceKind::Stroke => *self.paint_server_entry(kind) = None,
            }
        }
    }
}

/// Each enrolled row's published SVG paint resources. A publication shares the table, so a write
/// copies it only while a publication still holds it.
pub(crate) type SvgPaintResourceRows = HashMap<NodeSlotId, SvgPaintResourceRow>;

#[derive(Clone, Default)]
pub(crate) struct SvgPaintResources {
    rows: RefCell<Arc<SvgPaintResourceRows>>,
    needs_sync: Cell<bool>,
    /// Whether any row enrolled a resource, which the document's host reads beside a frame in flight. A row the frame
    /// enrolls asks for its resources to be resolved again itself.
    enrolled: Arc<AtomicBool>,
}

/// The published filter of a kind in a slot's row of `rows`.
pub(crate) fn published_filter_in(
    rows: &SvgPaintResourceRows,
    slot: NodeSlotId,
    kind: SvgPaintResourceKind,
) -> Option<Arc<PublishedSvgFilter>> {
    rows.get(&slot)?.published_filter(kind)
}

/// The image frames of the published SVG filters in `rows`, once each.
pub(crate) fn published_filter_image_frames_in(
    rows: &SvgPaintResourceRows,
) -> Vec<libgfx_rust::image_frame::ImageFrameHandle> {
    let mut frames: Vec<libgfx_rust::image_frame::ImageFrameHandle> = Vec::new();
    let mut known_ids = std::collections::HashSet::new();
    for row in rows.values() {
        for filter in [&row.filter, &row.backdrop_filter].into_iter().flatten() {
            for frame in filter
                .primitives
                .iter()
                .filter_map(|primitive| primitive.image_frame.as_ref())
            {
                if known_ids.insert(frame.id()) {
                    frames.push(frame.clone());
                }
            }
        }
    }
    frames
}

/// The published paint server of a kind in a slot's row of `rows`.
pub(crate) fn published_paint_server_in(
    rows: &SvgPaintResourceRows,
    slot: NodeSlotId,
    kind: SvgPaintResourceKind,
) -> Option<Arc<PublishedSvgPaintServer>> {
    rows.get(&slot)?.published_paint_server(kind)
}

impl SvgPaintResources {
    fn rows_mut(&self) -> std::cell::RefMut<'_, SvgPaintResourceRows> {
        std::cell::RefMut::map(self.rows.borrow_mut(), Arc::make_mut)
    }

    /// The table as it is now, for a recording to read.
    pub(crate) fn publish(&self) -> Arc<SvgPaintResourceRows> {
        self.rows.borrow().clone()
    }

    /// Where the table is, which moves when a write copies the one a publication shares.
    pub(crate) fn address(&self) -> usize {
        Arc::as_ptr(&self.rows.borrow()).addr()
    }

    pub(crate) fn set_enrolled_kinds(&self, slot: NodeSlotId, kinds: u8) {
        if kinds == 0 {
            self.forget_slot(slot);
            return;
        }
        self.needs_sync.set(true);
        if self
            .rows
            .borrow()
            .get(&slot)
            .is_some_and(|row| row.enrolled_kinds == kinds)
        {
            return;
        }
        let mut rows = self.rows_mut();
        let row = rows.entry(slot).or_default();
        let previous_kinds = std::mem::replace(&mut row.enrolled_kinds, kinds);
        row.forget_published(previous_kinds & !kinds);
        self.enrolled.store(true, Ordering::Relaxed);
    }

    pub(crate) fn withdraw(&self, slot: NodeSlotId, kind: SvgPaintResourceKind) {
        if !self.rows.borrow().contains_key(&slot) {
            return;
        }
        let mut rows = self.rows_mut();
        let Some(row) = rows.get_mut(&slot) else {
            return;
        };
        row.enrolled_kinds &= !kind.bit();
        row.forget_published(kind.bit());
        if row.enrolled_kinds == 0 {
            rows.remove(&slot);
            self.enrolled.store(!rows.is_empty(), Ordering::Relaxed);
        }
    }

    pub(crate) fn forget_slot(&self, slot: NodeSlotId) {
        if self.rows.borrow().contains_key(&slot) {
            let mut rows = self.rows_mut();
            rows.remove(&slot);
            self.enrolled.store(!rows.is_empty(), Ordering::Relaxed);
        }
    }

    /// Raises `flag`, which the document's host reads, while any row enrolled a resource, rather than a flag of its
    /// own. No row has enrolled one yet.
    pub(crate) fn share_enrolled_flag(&mut self, flag: Arc<AtomicBool>) {
        debug_assert!(!self.enrolled.load(Ordering::Relaxed));
        self.enrolled = flag;
    }

    pub(crate) fn has_enrolled_entries(&self) -> bool {
        !self.rows.borrow().is_empty()
    }

    pub(crate) fn note_changed(&self) -> bool {
        if !self.has_enrolled_entries() {
            return false;
        }
        self.needs_sync.set(true);
        true
    }

    /// Whether the host is to resolve the enrolled resources again.
    pub(crate) fn needs_sync(&self) -> bool {
        self.needs_sync.get()
    }

    pub(crate) fn take_needs_sync(&self) -> bool {
        self.needs_sync.replace(false)
    }

    pub(crate) fn enrolled_entries(&self) -> Vec<(NodeSlotId, SvgPaintResourceKind)> {
        self.rows
            .borrow()
            .iter()
            .flat_map(|(slot, row)| {
                SvgPaintResourceKind::ALL
                    .into_iter()
                    .filter(move |kind| row.enrolled_kinds & kind.bit() != 0)
                    .map(move |kind| (*slot, kind))
            })
            .collect()
    }

    pub(crate) fn published_filter(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
    ) -> Option<Arc<PublishedSvgFilter>> {
        published_filter_in(&self.rows.borrow(), slot, kind)
    }

    pub(crate) fn published_paint_server(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
    ) -> Option<Arc<PublishedSvgPaintServer>> {
        published_paint_server_in(&self.rows.borrow(), slot, kind)
    }

    pub(crate) fn publish_paint_server(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
        paint_server: PublishedSvgPaintServer,
    ) -> bool {
        match self.rows.borrow().get(&slot) {
            None => return false,
            Some(row)
                if row
                    .published_paint_server(kind)
                    .is_some_and(|previous| *previous == paint_server) =>
            {
                return false;
            }
            Some(_) => {}
        }
        // Only a change reaches the table, so a write that changes nothing never copies it.
        *self
            .rows_mut()
            .get_mut(&slot)
            .expect("the row was checked above")
            .paint_server_entry(kind) = Some(Arc::new(paint_server));
        true
    }

    pub(crate) fn publish_filter(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
        filter: PublishedSvgFilter,
    ) -> bool {
        match self.rows.borrow().get(&slot) {
            None => return false,
            Some(row) if row.published_filter(kind).is_some_and(|previous| *previous == filter) => return false,
            Some(_) => {}
        }
        *self
            .rows_mut()
            .get_mut(&slot)
            .expect("the row was checked above")
            .filter_entry(kind) = Some(Arc::new(filter));
        true
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_published_table_keeps_what_it_held_when_a_row_changes() {
        let resources = SvgPaintResources::default();
        let slot = NodeSlotId::new(1, 1);
        let filter = |failed| PublishedSvgFilter {
            failed,
            primitives: Vec::new(),
        };
        resources.set_enrolled_kinds(slot, SvgPaintResourceKind::Filter.bit());
        assert!(resources.publish_filter(slot, SvgPaintResourceKind::Filter, filter(false)));
        let published = resources.publish();
        // Publishing what the row already holds leaves the shared table alone.
        assert!(!resources.publish_filter(slot, SvgPaintResourceKind::Filter, filter(false)));
        assert!(Arc::ptr_eq(&published, &resources.publish()));

        assert!(resources.publish_filter(slot, SvgPaintResourceKind::Filter, filter(true)));
        assert!(
            !published_filter_in(&published, slot, SvgPaintResourceKind::Filter)
                .unwrap()
                .failed
        );
        assert!(
            resources
                .published_filter(slot, SvgPaintResourceKind::Filter)
                .unwrap()
                .failed
        );
        resources.forget_slot(slot);
        assert!(published_filter_in(&published, slot, SvgPaintResourceKind::Filter).is_some());
        assert!(!resources.has_enrolled_entries());
    }
}
