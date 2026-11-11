/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::{PaintPhase, PaintRecorder};
use crate::layout::node_data::NodeSlotId;
use crate::painting::hit_test::HitTestItem;
pub(crate) use crate::painting::hit_test::items::{HitTestFacts, hit_test_facts};
use crate::painting::hit_test::items::{HitTestItemSink, hit_test_items_of_phase};
use crate::painting::record::trace::Observer;

impl<O: Observer> HitTestItemSink for PaintRecorder<'_, O> {
    fn hit_test_facts(&mut self, paintable: NodeSlotId) -> HitTestFacts {
        self.paintable_facts(paintable)
    }

    fn own_accumulated_2d_scale(&self, paintable: NodeSlotId) -> libgfx_rust::FloatSize {
        PaintRecorder::own_accumulated_2d_scale(self, paintable)
    }

    fn append(&mut self, item: HitTestItem) {
        self.list.append(item);
    }
}

impl<O: Observer> PaintRecorder<'_, O> {
    pub(crate) fn record_hit_test_items(&mut self, paintable: NodeSlotId, phase: PaintPhase) {
        let rows = self.source;
        hit_test_items_of_phase(rows, self, paintable, phase);
    }
}
