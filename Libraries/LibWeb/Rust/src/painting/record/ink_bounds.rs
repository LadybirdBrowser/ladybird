/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::PaintRecorder;
use super::assemble::{AssemblyEvent, ScopeAction};
use super::inputs::{CaretTarget, RecordingInputs};
use super::order_tree::ProducerKind;
use super::trace::Observer;
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::fast_hash::FastMap;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::display_list::builder::for_each_command;
use crate::painting::display_list::commands::{
    ClipMode, ClipNodeIndex, ContextRef, DisplayListCommandHeader, DisplayListCommandType, SpatialNodeIndex,
};
use crate::painting::display_list::device_pixels::DevicePixelConverter;
use crate::painting::ink_overflow::{InkOverflow, ScopeInkBound, own_ink_overflow, scope_ink_bound};
use crate::painting::paint_order_plan::PaintScope;
use crate::painting::paint_read::GeometryRead;
use crate::painting::visual_context::{ClipNodeData, VisualContextTree, WorldTransforms};
use crate::painting::{node_painting, style_queries};
use libgfx_rust::{FloatRect, IntRect};
use smallvec::SmallVec;
use std::ops::Range;
use std::sync::OnceLock;

pub(crate) struct Verification {
    pub(crate) enabled: bool,
    pub(crate) counts: bool,
    pub(crate) report: bool,
}

pub(crate) fn verification() -> &'static Verification {
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
    ScopesBounded,
    ScopesUnbounded,
    ScopesNotCovering,
    CheckedAgainstScopes,
}

const COUNT_NAMES: [&str; 17] = [
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
    "scopes-bounded",
    "scopes-unbounded",
    "scopes-not-covering",
    "checked-against-scopes",
];

const COMPARISON_SLACK_IN_DEVICE_PIXELS: i32 = 2;

struct AuditedScopeBound {
    css_rect: CssPixelRect,
    /// In the space of the owner's own box, with the comparison slack.
    device_rect: FloatRect,
    owner_space: SpatialNodeIndex,
    /// The clip of the owner's own box and the clips above it. They cut the scope from outside, and whoever
    /// skips the scope by its bound has not applied them, so a command stops gathering clips when it meets one.
    clips_outside_scope: SmallVec<[ClipNodeIndex; 8]>,
}

struct AuditedScope {
    scope: PaintScope,
    bound: Option<AuditedScopeBound>,
}

/// Where a command draws in the space of a scope's owner, after the clips it meets inside the scope. `None` when a
/// transform on the way is not a two-dimensional affine one. The spaces are the tree's with every scroller at rest:
/// where a scroller is scrolled to makes no difference to whether a bound holds, as what scrolls is cut to the
/// scrollport by a clip the comparison applies.
fn drawn_rect_in_scope_owner_space(
    tree: &VisualContextTree,
    spaces_at_rest: &WorldTransforms,
    rect: FloatRect,
    context: ContextRef,
    bound: &AuditedScopeBound,
) -> Option<FloatRect> {
    let mut rect = rect;
    let mut space = context.spatial;
    let mut clip = context.clip;
    while !clip.is_none() && !bound.clips_outside_scope.contains(&clip) {
        let node = &tree.clip_nodes[clip.0 as usize];
        let clip_rect = match &node.data {
            ClipNodeData::Rect(data) if data.mode == ClipMode::Intersect => Some(data.rect),
            ClipNodeData::Path(path) => Some(path.bounding_rect.to_float()),
            _ => None,
        };
        if let Some(clip_rect) = clip_rect {
            rect = spaces_at_rest
                .map_rect_between_spaces(rect, space, node.spatial)?
                .intersected(clip_rect);
            if rect.is_empty() {
                return Some(rect);
            }
            space = node.spatial;
        }
        clip = node.parent;
    }
    spaces_at_rest.map_rect_between_spaces(rect, space, bound.owner_space)
}

fn command_draws_inputs_driven_ink(
    inputs: &RecordingInputs,
    converter: DevicePixelConverter,
    owner: NodeSlotId,
    inputs_ink_in_device_pixels: Option<IntRect>,
    header: &DisplayListCommandHeader,
) -> bool {
    inputs_ink_in_device_pixels.is_some_and(|rect| rect.contains_rect(header.bounding_rect))
        && (header.command_type == DisplayListCommandType::PaintCaret
            || (header.command_type == DisplayListCommandType::FillRect
                && inputs.caret.is_some_and(|caret| {
                    caret.target == CaretTarget::EmptyInline(owner)
                        && converter.rounded_device_rect(caret.rect) == header.bounding_rect
                })))
}

pub(super) struct InkBoundsAudit {
    pub enabled: bool,
    counts: Option<[u64; COUNT_NAMES.len()]>,
    estimates: FastMap<NodeSlotId, InkOverflow>,
    open_scopes: Vec<AuditedScope>,
    open_scopes_with_a_bound: usize,
}

impl InkBoundsAudit {
    pub fn new() -> Self {
        let verification = verification();
        Self {
            enabled: verification.enabled,
            counts: verification.counts.then_some([0; COUNT_NAMES.len()]),
            estimates: Default::default(),
            open_scopes: Vec::new(),
            open_scopes_with_a_bound: 0,
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
        let estimate = *self
            .ink_bounds_audit
            .estimates
            .entry(owner)
            .or_insert_with(|| own_ink_overflow(self.source, self.paint_state, owner));
        let stored = self.source.stored_ink_overflow(owner);
        let absolute = self.source.absolute_ink_overflow_rect(owner);
        assert!(
            absolute.is_none_or(|stored_rect| !estimate.unbounded && stored_rect.contains_rect(estimate.rect)),
            "Stored ink overflow misses own estimate: {:?} {owner:?}, stored {stored:?}, recomputed {estimate:?}, stamp {}",
            self.source.node_kind_if_live(owner),
            self.source.ink_overflow_stamp()
        );
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
        if let Some(reason) = self.background_placed_in_another_space(owner, kind) {
            self.ink_bounds_audit.count(reason);
            return;
        }
        if estimate.unbounded {
            self.ink_bounds_audit.count(Count::Unbounded);
            return;
        }
        let slack = COMPARISON_SLACK_IN_DEVICE_PIXELS;
        let estimate_device = self
            .converter
            .enclosing_device_rect(estimate.rect)
            .inflated_edges(slack, slack, slack, slack);
        let inputs_ink = self.inputs_driven_ink_in_device_pixels(owner, is_viewport);
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
            } else if command_draws_inputs_driven_ink(self.inputs, self.converter, owner, inputs_ink, header) {
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

    /// A fixed background is placed against the viewport and a local one scrolls with the box's content, so
    /// neither is recorded where the box's own ink is.
    fn background_placed_in_another_space(&mut self, owner: NodeSlotId, kind: ProducerKind) -> Option<Count> {
        let node = match kind {
            ProducerKind::DrawBackground => owner,
            ProducerKind::InlinePiece(index) => {
                let root = if node_painting::has_lines(self.source, owner) {
                    owner
                } else {
                    self.data(owner).containing_block
                };
                self.source.committed_side_data(root).inline_box_pieces()[index as usize].node
            }
            _ => return None,
        };
        let facts = self.base_paint_facts(node);
        if facts.has_scroll_offset_dependent_background {
            Some(Count::LocalBackground)
        } else if facts.has_fixed_background {
            Some(Count::FixedBackground)
        } else {
            None
        }
    }

    fn inputs_driven_ink_in_device_pixels(&self, owner: NodeSlotId, is_viewport: bool) -> Option<IntRect> {
        let slack = COMPARISON_SLACK_IN_DEVICE_PIXELS;
        inputs_driven_ink(self.inputs, owner, is_viewport).map(|rect| {
            self.converter
                .enclosing_device_rect(rect)
                .inflated_edges(slack, slack, slack, slack)
        })
    }

    pub(super) fn audit_scope_event(&mut self, event: AssemblyEvent) {
        match event {
            AssemblyEvent::ScopeBegin(scope, action) => {
                let bound = if action == ScopeAction::Inactive || self.is_recording_svg_resource_content() {
                    None
                } else {
                    self.audited_scope_bound(scope)
                };
                self.ink_bounds_audit.open_scopes_with_a_bound += usize::from(bound.is_some());
                self.ink_bounds_audit.open_scopes.push(AuditedScope { scope, bound });
            }
            AssemblyEvent::ScopeEnd(scope) => {
                let closed = self
                    .ink_bounds_audit
                    .open_scopes
                    .pop()
                    .expect("a scope ends after it began");
                assert!(
                    closed.scope == scope,
                    "scopes end in the reverse of the order they began in"
                );
                self.ink_bounds_audit.open_scopes_with_a_bound -= usize::from(closed.bound.is_some());
            }
            AssemblyEvent::ScopeCopied(_) | AssemblyEvent::ProducerCopied(..) => {}
        }
    }

    fn audited_scope_bound(&mut self, scope: PaintScope) -> Option<AuditedScopeBound> {
        let css_rect = match scope_ink_bound(self.source, scope) {
            ScopeInkBound::Rect(rect) => rect,
            ScopeInkBound::Unbounded => {
                self.ink_bounds_audit.count(Count::ScopesUnbounded);
                return None;
            }
            ScopeInkBound::NotCovering => {
                self.ink_bounds_audit.count(Count::ScopesNotCovering);
                return None;
            }
        };
        self.ink_bounds_audit.count(Count::ScopesBounded);
        let tree = self.paint_state.visual_context_tree.as_ref()?;
        let own_context = self.own_context(scope.owner);
        let mut clips_outside_scope = SmallVec::new();
        let mut clip = own_context.clip;
        while !clip.is_none() {
            clips_outside_scope.push(clip);
            clip = tree.clip_nodes[clip.0 as usize].parent;
        }
        let slack = COMPARISON_SLACK_IN_DEVICE_PIXELS;
        Some(AuditedScopeBound {
            css_rect,
            device_rect: self
                .converter
                .enclosing_device_rect(css_rect)
                .inflated_edges(slack, slack, slack, slack)
                .to_float(),
            owner_space: own_context.spatial,
            clips_outside_scope,
        })
    }

    /// Requires every command and hit-test item the producer just recorded to lie inside the bound of each scope it
    /// was recorded in: a walk that skipped one of those scopes because its bound missed a region or a point would
    /// otherwise have dropped something that draws or can be hit there.
    pub(super) fn verify_producer_against_open_scopes(
        &mut self,
        owner: NodeSlotId,
        kind: ProducerKind,
        bytes: Range<usize>,
        hit_test_items: Range<usize>,
    ) {
        if self.ink_bounds_audit.open_scopes_with_a_bound == 0
            || (bytes.is_empty() && hit_test_items.is_empty())
            || kind == ProducerKind::ScopePreamble
            || self.background_placed_in_another_space(owner, kind).is_some()
        {
            return;
        }
        let Some(tree) = self.paint_state.visual_context_tree.clone() else {
            return;
        };
        let is_viewport = self.source.node_kind_if_live(owner) == Some(NodeKind::Viewport);
        let inputs_ink = self.inputs_driven_ink_in_device_pixels(owner, is_viewport);
        let owner_kind = self.source.node_kind_if_live(owner);
        let inputs = self.inputs;
        let converter = self.converter;
        let spaces_at_rest = self.paint_state.unscrolled_world_transforms();
        let open_scopes = &self.ink_bounds_audit.open_scopes;
        let mut checked = false;
        let mut require_inside_open_scopes = |what: &dyn std::fmt::Debug, rect: IntRect, context: ContextRef| {
            for audited in open_scopes {
                let Some(bound) = &audited.bound else {
                    continue;
                };
                let Some(placed) =
                    drawn_rect_in_scope_owner_space(&tree, spaces_at_rest, rect.to_float(), context, bound)
                else {
                    continue;
                };
                checked = true;
                assert!(
                    placed.is_empty() || bound.device_rect.contains_rect(placed),
                    "Paint scope ink bound violation: {kind:?} of {owner_kind:?} {owner:?} recorded {what:?} {rect:?} under {context:?}, which lies at {placed:?} in the space of {:?}, outside its bound {:?} (device pixels, including {COMPARISON_SLACK_IN_DEVICE_PIXELS}px comparison slack; CSS bound {:?})",
                    audited.scope,
                    bound.device_rect,
                    bound.css_rect
                );
            }
        };
        let runs = self.recorder.command_runs();
        let mut run_index = runs.partition_point(|run| (run.offset + run.size) as usize <= bytes.start);
        for_each_command(&self.recorder.bytes()[bytes.clone()], |header, offset, _| {
            let absolute_offset = bytes.start + offset;
            while absolute_offset >= (runs[run_index].offset + runs[run_index].size) as usize {
                run_index += 1;
            }
            if header.command_type.is_compositor_metadata()
                || !header.has_bounding_rect
                || header.bounding_rect.is_empty()
                || command_draws_inputs_driven_ink(inputs, converter, owner, inputs_ink, header)
            {
                return;
            }
            require_inside_open_scopes(
                &header.command_type.name(),
                header.bounding_rect,
                runs[run_index].context,
            );
        });
        for item in &self.list.items[hit_test_items] {
            if !item.rect.is_empty() {
                require_inside_open_scopes(&item.kind, converter.enclosing_device_rect(item.rect), item.context);
            }
        }
        if checked {
            self.ink_bounds_audit.count(Count::CheckedAgainstScopes);
        }
    }
}
