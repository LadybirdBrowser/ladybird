/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::cell::{Cell, RefCell};
use std::collections::HashMap;
use std::rc::Rc;

use crate::layout::node_data::NodeSlotId;
use crate::painting::host::FfiSvgGradientDescription;
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

/// What a fill or stroke url() resolved to. A pattern's geometry still comes from the host at
/// record time.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) enum PublishedSvgPaintServer {
    #[default]
    None,
    Gradient(PublishedSvgGradient),
    Pattern,
}

/// What the last url() of a filter list resolved to: `failed` when it named nothing usable as an
/// SVG filter, which drops the whole filter list, otherwise the referenced filter's primitives in
/// document order.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct PublishedSvgFilter {
    pub failed: bool,
    pub primitives: Vec<SvgFilterPrimitive>,
}

#[derive(Default)]
struct SvgPaintResourceRow {
    enrolled_kinds: u8,
    filter: Option<Rc<PublishedSvgFilter>>,
    backdrop_filter: Option<Rc<PublishedSvgFilter>>,
    fill: Option<Rc<PublishedSvgPaintServer>>,
    stroke: Option<Rc<PublishedSvgPaintServer>>,
}

impl SvgPaintResourceRow {
    fn published_filter(&self, kind: SvgPaintResourceKind) -> Option<Rc<PublishedSvgFilter>> {
        match kind {
            SvgPaintResourceKind::Filter => self.filter.clone(),
            SvgPaintResourceKind::BackdropFilter => self.backdrop_filter.clone(),
            SvgPaintResourceKind::Fill | SvgPaintResourceKind::Stroke => unreachable!(),
        }
    }

    fn published_paint_server(&self, kind: SvgPaintResourceKind) -> Option<Rc<PublishedSvgPaintServer>> {
        match kind {
            SvgPaintResourceKind::Fill => self.fill.clone(),
            SvgPaintResourceKind::Stroke => self.stroke.clone(),
            SvgPaintResourceKind::Filter | SvgPaintResourceKind::BackdropFilter => unreachable!(),
        }
    }

    fn filter_entry(&mut self, kind: SvgPaintResourceKind) -> &mut Option<Rc<PublishedSvgFilter>> {
        match kind {
            SvgPaintResourceKind::Filter => &mut self.filter,
            SvgPaintResourceKind::BackdropFilter => &mut self.backdrop_filter,
            SvgPaintResourceKind::Fill | SvgPaintResourceKind::Stroke => unreachable!(),
        }
    }

    fn paint_server_entry(&mut self, kind: SvgPaintResourceKind) -> &mut Option<Rc<PublishedSvgPaintServer>> {
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

fn publish<T: PartialEq>(entry: &mut Option<Rc<T>>, value: T) -> bool {
    if entry.as_ref().is_some_and(|previous| **previous == value) {
        return false;
    }
    *entry = Some(Rc::new(value));
    true
}

#[derive(Default)]
pub(crate) struct SvgPaintResources {
    rows: RefCell<HashMap<NodeSlotId, SvgPaintResourceRow>>,
    needs_sync: Cell<bool>,
}

impl SvgPaintResources {
    pub(crate) fn set_enrolled_kinds(&self, slot: NodeSlotId, kinds: u8) {
        let mut rows = self.rows.borrow_mut();
        if kinds == 0 {
            rows.remove(&slot);
            return;
        }
        self.needs_sync.set(true);
        let row = rows.entry(slot).or_default();
        let previous_kinds = std::mem::replace(&mut row.enrolled_kinds, kinds);
        row.forget_published(previous_kinds & !kinds);
    }

    pub(crate) fn withdraw(&self, slot: NodeSlotId, kind: SvgPaintResourceKind) {
        let mut rows = self.rows.borrow_mut();
        let Some(row) = rows.get_mut(&slot) else {
            return;
        };
        row.enrolled_kinds &= !kind.bit();
        row.forget_published(kind.bit());
        if row.enrolled_kinds == 0 {
            rows.remove(&slot);
        }
    }

    pub(crate) fn forget_slot(&self, slot: NodeSlotId) {
        self.rows.borrow_mut().remove(&slot);
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
    ) -> Option<Rc<PublishedSvgFilter>> {
        self.rows.borrow().get(&slot)?.published_filter(kind)
    }

    pub(crate) fn published_filter_image_frames(&self) -> Vec<libgfx_rust::image_frame::ImageFrameHandle> {
        let mut frames: Vec<libgfx_rust::image_frame::ImageFrameHandle> = Vec::new();
        let mut known_ids = std::collections::HashSet::new();
        for row in self.rows.borrow().values() {
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

    pub(crate) fn published_paint_server(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
    ) -> Option<Rc<PublishedSvgPaintServer>> {
        self.rows.borrow().get(&slot)?.published_paint_server(kind)
    }

    pub(crate) fn publish_paint_server(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
        paint_server: PublishedSvgPaintServer,
    ) -> bool {
        let mut rows = self.rows.borrow_mut();
        let Some(row) = rows.get_mut(&slot) else {
            return false;
        };
        publish(row.paint_server_entry(kind), paint_server)
    }

    pub(crate) fn publish_filter(
        &self,
        slot: NodeSlotId,
        kind: SvgPaintResourceKind,
        filter: PublishedSvgFilter,
    ) -> bool {
        let mut rows = self.rows.borrow_mut();
        let Some(row) = rows.get_mut(&slot) else {
            return false;
        };
        publish(row.filter_entry(kind), filter)
    }
}
