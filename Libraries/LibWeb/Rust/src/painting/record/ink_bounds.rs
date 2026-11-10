/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::PaintRecorder;
use super::inputs::{CaretTarget, RecordingInputs};
use super::order_tree::ProducerKind;
use super::trace::Observer;
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::display_list::builder::for_each_command;
use crate::painting::display_list::commands::DisplayListCommandType;
use crate::painting::ink_overflow::{InkOverflow, own_ink_overflow};
use crate::painting::paint_read::GeometryRead;
use crate::painting::{node_painting, style_queries};
use std::ops::Range;
use std::sync::OnceLock;

struct Verification {
    enabled: bool,
    counts: bool,
    report: bool,
}

fn verification() -> &'static Verification {
    static VERIFICATION: OnceLock<Verification> = OnceLock::new();
    VERIFICATION.get_or_init(|| {
        let environment_enabled = std::env::var_os("LIBWEB_VERIFY_PAINT_INK_BOUNDS").is_some();
        Verification {
            enabled: cfg!(debug_assertions) || environment_enabled,
            counts: environment_enabled,
            report: std::env::var_os("LIBWEB_REPORT_PAINT_INK_BOUNDS").is_some(),
        }
    })
}

#[derive(Clone, Copy)]
enum Count {
    Producers,
    Checked,
    Unbounded,
    Svg,
    Preamble,
    Viewport,
    RootElement,
    FixedBackground,
    LocalBackground,
    OtherSpatialNode,
    MetadataOrCanvas,
    InputDriven,
    Empty,
}

const COUNT_NAMES: [&str; 13] = [
    "producers",
    "checked",
    "unbounded",
    "svg",
    "preamble",
    "viewport",
    "root-element",
    "fixed-background",
    "local-background",
    "other-spatial-node",
    "metadata-or-canvas",
    "input-driven",
    "empty",
];

pub(super) struct InkBoundsAudit {
    pub enabled: bool,
    counts: Option<[u64; COUNT_NAMES.len()]>,
    estimates: crate::fast_hash::FastMap<NodeSlotId, InkOverflow>,
}

impl InkBoundsAudit {
    pub fn new() -> Self {
        let verification = verification();
        Self {
            enabled: verification.enabled,
            counts: verification.counts.then_some([0; COUNT_NAMES.len()]),
            estimates: Default::default(),
        }
    }

    fn count(&mut self, count: Count) {
        if let Some(counts) = &mut self.counts {
            counts[count as usize] += 1;
        }
    }

    pub fn report(&self) {
        if verification().report
            && let Some(counts) = self.counts
        {
            let summary: Vec<_> = COUNT_NAMES
                .iter()
                .zip(counts)
                .map(|(name, count)| format!("{name}={count}"))
                .collect();
            eprintln!("Paint ink bounds audit: {}", summary.join(" "));
        }
    }
}

/// Ink supplied by the host rather than by the row snapshot, in absolute layout CSS pixels.
pub(crate) fn inputs_driven_ink(
    inputs: &RecordingInputs,
    owner: NodeSlotId,
    is_viewport: bool,
) -> Option<CssPixelRect> {
    let mut rect = inputs
        .caret
        .filter(|caret| match caret.target {
            CaretTarget::InBlock { block, owner: inline } => inline.unwrap_or(block) == owner,
            CaretTarget::EmptyInline(inline) => inline == owner,
        })
        .map(|caret| caret.rect);
    if is_viewport && let Some(origin) = inputs.uncaptured.middle_button_scroll_origin {
        let radius = CssPixels::nearest_value_for(17.0 / inputs.device_pixels_per_css_pixel);
        let indicator = CssPixelRect::new(origin.x - radius, origin.y - radius, radius * 2, radius * 2);
        rect = Some(rect.map_or(indicator, |mut rect| {
            rect.unite(indicator);
            rect
        }));
    }
    rect
}

impl<O: Observer> PaintRecorder<'_, O> {
    pub(super) fn verify_producer_ink(&mut self, owner: NodeSlotId, kind: ProducerKind, bytes: Range<usize>) {
        self.ink_bounds_audit.count(Count::Producers);
        let is_viewport = self.source.node_kind_if_live(owner) == Some(NodeKind::Viewport);
        let exempt = match kind {
            ProducerKind::Svg => Some(Count::Svg),
            ProducerKind::ScopePreamble => Some(Count::Preamble),
            _ if is_viewport => Some(Count::Viewport),
            _ if style_queries::node_is_root_element(self.source, owner) => Some(Count::RootElement),
            _ => None,
        };
        if let Some(reason) = exempt {
            if is_viewport && kind == ProducerKind::DrawOverlay && inputs_driven_ink(self.inputs, owner, true).is_some()
            {
                self.ink_bounds_audit.count(Count::InputDriven);
            }
            self.ink_bounds_audit.count(reason);
            return;
        }
        if bytes.is_empty() {
            self.ink_bounds_audit.count(Count::Empty);
            return;
        }
        let background_node = match kind {
            ProducerKind::DrawBackground => Some(owner),
            ProducerKind::InlinePiece(index) => {
                let root = if node_painting::has_lines(self.source, owner) {
                    owner
                } else {
                    self.data(owner).containing_block
                };
                Some(self.source.committed_side_data(root).inline_box_pieces()[index as usize].node)
            }
            _ => None,
        };
        if let Some(node) = background_node {
            let facts = self.base_paint_facts(node);
            if facts.has_scroll_offset_dependent_background {
                self.ink_bounds_audit.count(Count::LocalBackground);
                return;
            }
            if facts.has_fixed_background {
                self.ink_bounds_audit.count(Count::FixedBackground);
                return;
            }
        }
        let estimate = *self
            .ink_bounds_audit
            .estimates
            .entry(owner)
            .or_insert_with(|| own_ink_overflow(self.source, self.paint_state, owner));
        if estimate.unbounded {
            self.ink_bounds_audit.count(Count::Unbounded);
            return;
        }
        let estimate_device = self
            .converter
            .enclosing_device_rect(estimate.rect)
            .inflated_edges(2, 2, 2, 2);
        let inputs_ink = inputs_driven_ink(self.inputs, owner, is_viewport)
            .map(|rect| self.converter.enclosing_device_rect(rect).inflated_edges(2, 2, 2, 2));
        let own_spatial = self.own_context(owner).spatial;
        let descendants_spatial = self.for_descendants_context(owner).spatial;
        let runs = self.recorder.command_runs();
        let mut run_index = runs.partition_point(|run| (run.offset + run.size) as usize <= bytes.start);
        let mut counts = [false; COUNT_NAMES.len()];
        for_each_command(&self.recorder.bytes()[bytes.clone()], |header, offset, _| {
            let absolute_offset = bytes.start + offset;
            while absolute_offset >= (runs[run_index].offset + runs[run_index].size) as usize {
                run_index += 1;
            }
            let spatial = runs[run_index].context.spatial;
            let reason = if header.command_type.is_compositor_metadata() || !header.has_bounding_rect {
                Count::MetadataOrCanvas
            } else if spatial != own_spatial && spatial != descendants_spatial {
                Count::OtherSpatialNode
            } else if inputs_ink.is_some_and(|rect| rect.contains_rect(header.bounding_rect))
                && (header.command_type == DisplayListCommandType::PaintCaret
                    || (header.command_type == DisplayListCommandType::FillRect
                        && self.inputs.caret.is_some_and(|caret| {
                            caret.target == CaretTarget::EmptyInline(owner)
                                && self.converter.rounded_device_rect(caret.rect) == header.bounding_rect
                        })))
            {
                Count::InputDriven
            } else if !header.bounding_rect.is_empty() && !estimate_device.contains_rect(header.bounding_rect) {
                panic!(
                    "Paint ink bounds violation: {kind:?} of {:?} {owner:?}, border box {:?}, recorded {} {:?} outside own estimate {:?} (device pixels, including 2px comparison slack; CSS estimate {:?})",
                    self.source.node_kind_if_live(owner),
                    crate::painting::paintable_geometry::absolute_border_box_rect(self.source, owner),
                    header.command_type.name(),
                    header.bounding_rect,
                    estimate_device,
                    estimate.rect
                );
            } else {
                Count::Checked
            };
            counts[reason as usize] = true;
        });
        for (index, present) in counts.into_iter().enumerate() {
            if present && let Some(counts) = &mut self.ink_bounds_audit.counts {
                counts[index] += 1;
            }
        }
    }
}
