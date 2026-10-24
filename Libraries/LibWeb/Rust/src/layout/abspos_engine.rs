/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use crate::css::style::tree::StyleNodeID;
use crate::painting::paint_read::{GeometryRead, PaintRead};

pub(super) fn axis_modes(style: StyleValues) -> (abspos_inputs::AbsposAxisMode, abspos_inputs::AbsposAxisMode) {
    (
        if style.inset_left().is_auto() && style.inset_right().is_auto() {
            abspos_inputs::AbsposAxisMode::StaticPosition
        } else {
            abspos_inputs::AbsposAxisMode::InsetFromRect
        },
        if style.inset_top().is_auto() && style.inset_bottom().is_auto() {
            abspos_inputs::AbsposAxisMode::StaticPosition
        } else {
            abspos_inputs::AbsposAxisMode::InsetFromRect
        },
    )
}

pub(crate) fn aligned_static_offset(
    static_position_rect: abspos_inputs::StaticPositionRect,
    margin_box_inline_size: CssPixels,
    margin_box_block_size: CssPixels,
) -> geometry::LogicalOffset {
    let mut offset = static_position_rect.rect.offset;
    match static_position_rect.inline_alignment {
        StaticPositionAlignment::Start => {}
        StaticPositionAlignment::Center => {
            offset.inline_offset += (static_position_rect.rect.size.inline_size - margin_box_inline_size) / 2;
        }
        StaticPositionAlignment::End => {
            offset.inline_offset += static_position_rect.rect.size.inline_size - margin_box_inline_size;
        }
    }
    match static_position_rect.block_alignment {
        StaticPositionAlignment::Start => {}
        StaticPositionAlignment::Center => {
            offset.block_offset += (static_position_rect.rect.size.block_size - margin_box_block_size) / 2;
        }
        StaticPositionAlignment::End => {
            offset.block_offset += static_position_rect.rect.size.block_size - margin_box_block_size;
        }
    }
    offset
}

/// The static position rect relative to the containing block rect that insets are measured from, as the sizing
/// rules take it.
/// https://www.w3.org/TR/CSS22/visudet.html#abs-non-replaced-width
/// The static position for 'left' is the distance from the left edge of the containing block to the left margin edge
/// of a hypothetical box that would have been the first box of the element if its 'position' property had been
/// 'static' and 'float' had been 'none'.
/// NB: An axis that is not placed at its static position adds the containing block rect offset to its used inset, so
///     its static position rect already stands relative to the containing block rect.
fn static_position_rect_for_sizing(inputs: &abspos_inputs::AbsposLayoutInputs) -> abspos_inputs::StaticPositionRect {
    let info = &inputs.containing_block_info;
    let translation = |mode: abspos_inputs::AbsposAxisMode, origin: CssPixels| {
        if mode == abspos_inputs::AbsposAxisMode::StaticPosition {
            CssPixels::default() - origin
        } else {
            CssPixels::default()
        }
    };
    formatting_context::translate_static_position_rect(
        inputs.static_position_rect,
        FfiCssPixelPoint {
            x: translation(info.inline_axis_mode, info.rect.offset.inline_offset),
            y: translation(info.block_axis_mode, info.rect.offset.block_offset),
        },
    )
}

fn out_of_flow_root_space(inputs: abspos_inputs::AbsposLayoutInputs) -> (AvailableSpace, ContainingBlockConstraints) {
    let containing_block_size = geometry::LogicalSize {
        inline_size: clamp_to_max_dimension_value(inputs.containing_block_info.rect.size.inline_size),
        block_size: clamp_to_max_dimension_value(inputs.containing_block_info.rect.size.block_size),
    };
    (
        AvailableSpace {
            inline_size: AvailableSize::definite(containing_block_size.inline_size),
            block_size: AvailableSize::definite(containing_block_size.block_size),
        },
        ContainingBlockConstraints {
            percentage_basis_inline_size: Some(containing_block_size.inline_size),
            percentage_basis_block_size: Some(containing_block_size.block_size),
            quirks_mode_percentage_basis_block_size: None,
        },
    )
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ContainingBlockGeometry {
    pub(crate) content_origin_in_entry_space: FfiCssPixelPoint,
    pub(crate) padding_left: CssPixels,
    pub(crate) padding_right: CssPixels,
    pub(crate) padding_top: CssPixels,
    pub(crate) padding_bottom: CssPixels,
    pub(crate) content_inline_size: CssPixels,
    pub(crate) content_block_size: CssPixels,
}

impl ContainingBlockGeometry {
    pub(crate) fn from_used_values(used: &UsedValues, content_origin_in_entry_space: FfiCssPixelPoint) -> Self {
        Self {
            content_origin_in_entry_space,
            padding_left: used.padding_left.get(),
            padding_right: used.padding_right.get(),
            padding_top: used.padding_top.get(),
            padding_bottom: used.padding_bottom.get(),
            content_inline_size: used.content_inline_size.get(),
            content_block_size: used.content_block_size.get(),
        }
    }

    pub(crate) fn padding_box_inline_size(&self) -> CssPixels {
        self.content_inline_size + self.padding_left + self.padding_right
    }

    pub(crate) fn padding_box_block_size(&self) -> CssPixels {
        self.content_block_size + self.padding_top + self.padding_bottom
    }
}

pub(crate) struct AbsposEngine<'pass> {
    purpose: formatting_context::LayoutPurpose,
    records: &'pass RunRecords<'pass>,
    callbacks: LayoutPass<'pass>,
    fragments: Option<std::rc::Rc<fragment_tree::RunFragmentBuilder>>,
}

impl<'pass> AbsposEngine<'pass> {
    pub(crate) fn for_run(run: &FormattingContextRun<'pass>) -> Self {
        Self {
            purpose: run.purpose,
            records: run.records,
            callbacks: run.callbacks,
            fragments: run.fragments.clone(),
        }
    }

    fn containing_block_geometry_for_pending_child(
        &self,
        entry: &abspos_inputs::PendingAbsposChild,
    ) -> ContainingBlockGeometry {
        let containing_block = entry.containing_block();
        let Some(containing_block_used) = self.records.used_values_if_owned(containing_block) else {
            panic!(
                "no owned record for the containing block of slot {} (cb slot {})",
                entry.child_box.slot_index(),
                containing_block.slot_index()
            );
        };
        let content_origin_in_entry_space =
            self.origin_of_containing_block_in_entry_space(containing_block, entry.coordinate_space_box);
        ContainingBlockGeometry::from_used_values(containing_block_used, content_origin_in_entry_space)
    }

    fn origin_of_containing_block_in_entry_space(&self, containing_block: Node, entry_space: Node) -> FfiCssPixelPoint {
        if entry_space == containing_block {
            return FfiCssPixelPoint::default();
        }
        let mut origins_by_chain_box = Vec::new();
        let mut chain_box = containing_block;
        let mut origin = FfiCssPixelPoint::default();
        loop {
            origins_by_chain_box.push((chain_box, origin));
            let Some(used) = self
                .records
                .used_values_if_owned(chain_box)
                .filter(|used| used.has_content_offset.get())
            else {
                break;
            };
            origin = formatting_context::point_add(origin, used.content_offset.get());
            chain_box = self.placement_containing_block(chain_box);
            if chain_box.is_invalid() {
                break;
            }
        }
        let mut space_box = entry_space;
        let mut space_origin = FfiCssPixelPoint::default();
        loop {
            if let Some((_, origin)) = origins_by_chain_box
                .iter()
                .find(|(chain_box, _)| *chain_box == space_box)
            {
                return formatting_context::point_sub(*origin, space_origin);
            }
            let used = self
                .records
                .used_values_if_owned(space_box)
                .filter(|used| used.has_content_offset.get())
                .unwrap_or_else(|| {
                    panic!(
                        "an entry's space chain crossed an unplaced or foreign record before meeting its containing block's chain (slot {})",
                        space_box.slot_index()
                    )
                });
            space_origin = formatting_context::point_add(space_origin, used.content_offset.get());
            space_box = self.placement_containing_block(space_box);
            assert!(
                !space_box.is_invalid(),
                "an entry's space chain ended before meeting its containing block's chain"
            );
        }
    }

    fn translation_between_payload_resting_spaces(&self, from_space: Node, to_space: Node) -> FfiCssPixelPoint {
        if from_space == to_space || from_space.is_invalid() || to_space.is_invalid() {
            return FfiCssPixelPoint::default();
        }
        let mut merge_point = from_space;
        while merge_point != to_space && !self.callbacks.is_ancestor(merge_point, to_space, self.records.root()) {
            merge_point = self.placement_containing_block(merge_point);
            assert!(!merge_point.is_invalid());
        }
        let offset_relative_to_merge_point = |descendant: Node| {
            let mut offset = FfiCssPixelPoint::default();
            let mut current = descendant;
            while current != merge_point {
                let used = self.records.used_values_if_owned(current).unwrap_or_else(|| {
                    panic!(
                        "a payload resting space's chain crossed a record this run does not own (slot {})",
                        current.slot_index()
                    )
                });
                offset = formatting_context::point_add(offset, used.content_offset.get());
                current = self.placement_containing_block(current);
                assert!(!current.is_invalid());
            }
            offset
        };
        formatting_context::point_sub(
            offset_relative_to_merge_point(from_space),
            offset_relative_to_merge_point(to_space),
        )
    }

    fn placement_containing_block(&self, node: Node) -> Node {
        formatting_context::placement_containing_block(self.records, &self.callbacks, node)
    }

    fn sizing(&self) -> sizing_context::SizingContext<'pass> {
        sizing_context::SizingContext::new(self.purpose, self.records, self.callbacks)
    }

    fn style(&self, node: Node) -> StyleValues<'pass> {
        let style = StyleValues::for_node(&self.callbacks, node);
        if !style.is_absolutely_positioned() {
            return style;
        }
        let anchor_resolved_values = self
            .records
            .used_values_if_owned(node)
            .and_then(|used| used.anchor_resolved_values.get())
            .map(|resolved| &**resolved);
        style.with_anchor_resolved_values(anchor_resolved_values)
    }

    fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.callbacks, node)
    }

    #[track_caller]
    fn used(&self, node: Node) -> &'pass UsedValues {
        self.records.used_values(node)
    }

    fn base_containing_block_info(
        &self,
        node: Node,
        inline_containing_block_rect: Option<CssPixelRect>,
        entry_containing_block_geometry: &ContainingBlockGeometry,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) -> abspos_inputs::AbsposContainingBlockInfo {
        let style = self.style(node).with_resolved_insets(resolved_anchor_insets);
        let (inline_axis_mode, block_axis_mode) = axis_modes(style);
        if let Some(rect) = inline_containing_block_rect {
            return abspos_inputs::AbsposContainingBlockInfo {
                rect: geometry::LogicalRect {
                    offset: geometry::LogicalOffset {
                        inline_offset: rect.x,
                        block_offset: rect.y,
                    },
                    size: geometry::LogicalSize {
                        inline_size: rect.width,
                        block_size: rect.height,
                    },
                },
                inline_axis_mode,
                block_axis_mode,
                inline_alignment: None,
                block_alignment: None,
                derives_from_own_computed_values: false,
            };
        }

        abspos_inputs::AbsposContainingBlockInfo {
            rect: geometry::LogicalRect {
                offset: geometry::LogicalOffset {
                    inline_offset: -entry_containing_block_geometry.padding_left,
                    block_offset: -entry_containing_block_geometry.padding_top,
                },
                size: geometry::LogicalSize {
                    inline_size: entry_containing_block_geometry.padding_box_inline_size(),
                    block_size: entry_containing_block_geometry.padding_box_block_size(),
                },
            },
            inline_axis_mode,
            block_axis_mode,
            inline_alignment: None,
            block_alignment: None,
            derives_from_own_computed_values: false,
        }
    }
}

struct AnchorResolutionState {
    default_anchor_box: Node,
    compensates_for_horizontal_scroll: bool,
    compensates_for_vertical_scroll: bool,
}

impl AnchorResolutionState {
    const NO_SCROLL_SHIFT: Self = Self {
        default_anchor_box: NodeSlotId::INVALID,
        compensates_for_horizontal_scroll: false,
        compensates_for_vertical_scroll: false,
    };
}

/// The interpreted <anchor-side> argument of an anchor() function; the
/// percentage carries its value as a fraction.
#[derive(Clone, Copy, PartialEq)]
enum AnchorSide {
    Invalid,
    Top,
    Right,
    Bottom,
    Left,
    Center,
    Start,
    End,
    SelfStart,
    SelfEnd,
    Inside,
    Outside,
    Percentage(f64),
}

fn anchor_side_from_style_value(side: &crate::css::style_value::StyleValueData) -> AnchorSide {
    use crate::css::style_value::StyleValueData;
    match side {
        StyleValueData::Keyword { keyword } => match *keyword {
            keyword::TOP => AnchorSide::Top,
            keyword::RIGHT => AnchorSide::Right,
            keyword::BOTTOM => AnchorSide::Bottom,
            keyword::LEFT => AnchorSide::Left,
            keyword::CENTER => AnchorSide::Center,
            keyword::START => AnchorSide::Start,
            keyword::END => AnchorSide::End,
            keyword::SELF_START => AnchorSide::SelfStart,
            keyword::SELF_END => AnchorSide::SelfEnd,
            keyword::INSIDE => AnchorSide::Inside,
            keyword::OUTSIDE => AnchorSide::Outside,
            _ => AnchorSide::Invalid,
        },
        StyleValueData::Percentage { value } => AnchorSide::Percentage(value / 100.0),
        _ => AnchorSide::Invalid,
    }
}

#[derive(Clone, Copy)]
struct AnchorValueAxis {
    is_from_end: bool,
    is_horizontal: bool,
    containing_block_extent: CssPixels,
}

#[derive(Clone, Copy)]
struct AnchorCalcCallbackContext<'pass> {
    engine: *const AbsposEngine<'pass>,
    positioned_box: Node,
    containing_block: Node,
    containing_block_geometry: Option<ContainingBlockGeometry>,
    entry_coordinate_space_box: Node,
    is_from_end: bool,
    is_horizontal_axis: bool,
    containing_block_extent: CssPixels,
    resolution_state: *mut AnchorResolutionState,
}

impl AbsposEngine<'_> {
    /// The box `anchor_name` names for `positioned_box`.
    ///
    /// https://drafts.csswg.org/css-shadow-1/#tree-scoped-name
    /// An anchor name is tree-scoped: the search starts in the tree scope the querying element is
    /// in, and when that is a shadow tree and nothing acceptable is found there, it continues in the
    /// tree scope of the shadow host, up to the document tree.
    fn anchor_lookup(&self, positioned_box: Node, anchor_name: usize) -> Option<Node> {
        let arena = self.callbacks.arena();
        // A pseudo-element queries from its generator. Any other anonymous box stands for no element
        // and has no tree scope to search.
        let is_pseudo_element = arena.node_is_generated_for_pseudo_element(positioned_box);
        let querying_element = if is_pseudo_element {
            arena.node_style_node(positioned_box)
        } else {
            arena.dom_node_style_node(positioned_box)
        }?;
        // NB: A pseudo-element is a descendant of its generator, so the generator's anchor-scope applies to it.
        let querier_scope_start = if is_pseudo_element {
            Some(querying_element)
        } else {
            arena.style_node_parent(querying_element)
        };
        let containing_block = arena.node_containing_block_if_live(positioned_box)?;
        let mut scope_host = arena.tree_scope_host(querying_element);
        loop {
            // https://drafts.csswg.org/css-anchor-position-1/#anchor-scope
            // NB: An anchor-scope element both keeps the matching anchor names of its subtree from everything outside
            //     it, and limits lookups from its descendants to anchors inside it. So an anchor is visible exactly
            //     when the nearest element that scopes its name around it also scopes the name around the querier.
            let querier_scope =
                querier_scope_start.and_then(|start| self.nearest_anchor_scope(start, anchor_name, scope_host));
            let found = arena.last_element_with_anchor_name(scope_host, anchor_name, |&element| {
                self.anchor_is_acceptable(arena.bound_row(element), positioned_box, containing_block)
                    && self.nearest_anchor_scope(element, anchor_name, scope_host) == querier_scope
            });
            if let Some(element) = found {
                return Some(arena.bound_row(element));
            }
            scope_host = arena.tree_scope_host(scope_host?);
        }
    }

    /// The nearest inclusive ancestor of `element` whose anchor-scope covers `anchor_name`, among the elements of the
    /// tree scope `scope_host` names.
    /// https://drafts.csswg.org/css-anchor-position-1/#anchor-scope
    fn nearest_anchor_scope(
        &self,
        element: StyleNodeID,
        anchor_name: usize,
        scope_host: Option<StyleNodeID>,
    ) -> Option<StyleNodeID> {
        let arena = self.callbacks.arena();
        let mut candidate = Some(element);
        while let Some(current) = candidate {
            if let Some(payloads) = arena.style_node_style_payloads(current) {
                let anchor = ComputedValuesView::new(&payloads.groups).anchor();
                // The <anchor-name> represents a strictly matched tree-scoped name, i.e. it can only match against
                // anchor names in the same shadow tree.
                if (anchor.anchor_scope_all || anchor.anchor_scope_names.raws().contains(&anchor_name))
                    && arena.tree_scope_host(current) == scope_host
                {
                    return Some(current);
                }
            }
            candidate = arena.style_node_parent(current);
        }
        None
    }

    /// Whether `anchor_box` may act as the anchor of `positioned_box`: it must have used values
    /// from this pass, and its containing block chain must reach the positioned box's own.
    fn anchor_is_acceptable(&self, anchor_box: Node, positioned_box: Node, containing_block: Node) -> bool {
        let arena = self.callbacks.arena();
        if anchor_box == positioned_box
            || !arena
                .node_kind_if_live(anchor_box)
                .is_some_and(super::node_facts::kind_is_box)
            || !self
                .fragments
                .as_deref()
                .is_some_and(|fragments| fragments.find_anchor_candidate(anchor_box).is_some())
        {
            return false;
        }
        let mut ancestor = arena.node_containing_block_if_live(anchor_box);
        while let Some(block) = ancestor {
            if block == containing_block {
                return true;
            }
            ancestor = arena.node_containing_block_if_live(block);
        }
        false
    }

    fn nearest_scroll_container_ancestor(&self, node: Node) -> Node {
        let mut ancestor = self.placement_containing_block(node);
        while !ancestor.is_invalid() {
            if self.facts(ancestor).is_scroll_container() {
                return ancestor;
            }
            if ancestor == self.records.root() {
                break;
            }
            ancestor = self.placement_containing_block(ancestor);
        }
        NodeSlotId::INVALID
    }

    fn anchor_rect(
        &self,
        anchor_box: Node,
        containing_block: Node,
        entry_containing_block_geometry: Option<&ContainingBlockGeometry>,
        entry_coordinate_space_box: Node,
    ) -> CssPixelRect {
        let (rect, coordinate_space_box) = self
            .fragments
            .as_deref()
            .and_then(|fragments| fragments.find_anchor_candidate(anchor_box))
            .expect("an accepted anchor is placed in the draining subtree and propagated here");
        match entry_containing_block_geometry {
            Some(geometry) => {
                let fold_into_entry_space =
                    self.translation_between_payload_resting_spaces(coordinate_space_box, entry_coordinate_space_box);
                CssPixelRect {
                    x: rect.x + fold_into_entry_space.x - geometry.content_origin_in_entry_space.x
                        + geometry.padding_left,
                    y: rect.y + fold_into_entry_space.y - geometry.content_origin_in_entry_space.y
                        + geometry.padding_top,
                    width: rect.width,
                    height: rect.height,
                }
            }
            None => {
                let containing_block_used = self.used(containing_block);
                CssPixelRect {
                    x: rect.x + containing_block_used.padding_left.get(),
                    y: rect.y + containing_block_used.padding_top.get(),
                    width: rect.width,
                    height: rect.height,
                }
            }
        }
    }

    fn anchor_side(
        &self,
        side: AnchorSide,
        rect: CssPixelRect,
        positioned_box: Node,
        containing_block: Node,
        is_from_end: bool,
        is_horizontal_axis: bool,
    ) -> Option<CssPixels> {
        let (axis_start, axis_end) = if is_horizontal_axis {
            (rect.left(), rect.right())
        } else {
            (rect.top(), rect.bottom())
        };
        // https://drafts.csswg.org/css-anchor-position-1/#valdef-anchor-start
        // Refers to one of the sides of the anchor box in the same axis as the inset property it's used in, by
        // resolving the keyword against the writing mode of either the positioned box (for self-start and self-end)
        // or the positioned box's containing block (for start and end).
        let start_and_end_sides = |writing_mode_box: Node| {
            let style = self.style(writing_mode_box);
            if geometry::start_is_left_or_top(style.writing_mode(), style.direction(), is_horizontal_axis) {
                (axis_start, axis_end)
            } else {
                (axis_end, axis_start)
            }
        };
        // https://drafts.csswg.org/css-anchor-position-1/#valdef-anchor-percentage
        // Refers to a position a corresponding percentage between the start and end sides, with 0% being equivalent
        // to start and 100% being equivalent to end.
        let between_start_and_end = |fraction: f64| {
            let (start, end) = start_and_end_sides(containing_block);
            start + CssPixels::nearest_value_for((end - start).to_double() * fraction)
        };
        match side {
            AnchorSide::Invalid => None,
            AnchorSide::Top => (!is_horizontal_axis).then_some(rect.top()),
            AnchorSide::Bottom => (!is_horizontal_axis).then_some(rect.bottom()),
            AnchorSide::Left => is_horizontal_axis.then_some(rect.left()),
            AnchorSide::Right => is_horizontal_axis.then_some(rect.right()),
            // https://drafts.csswg.org/css-anchor-position-1/#valdef-anchor-center
            // center is equivalent to 50%.
            AnchorSide::Center => Some(between_start_and_end(0.5)),
            AnchorSide::Start => Some(start_and_end_sides(containing_block).0),
            AnchorSide::End => Some(start_and_end_sides(containing_block).1),
            AnchorSide::SelfStart => Some(start_and_end_sides(positioned_box).0),
            AnchorSide::SelfEnd => Some(start_and_end_sides(positioned_box).1),
            AnchorSide::Inside | AnchorSide::Outside => {
                let same_side = side == AnchorSide::Inside;
                Some(if is_from_end == same_side { axis_end } else { axis_start })
            }
            AnchorSide::Percentage(fraction) => Some(between_start_and_end(fraction)),
        }
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#anchor-size-fn
    /// The length an anchor-size() function with the given <anchor-size> keyword resolves to against the anchor box
    /// `rect`, used in a property of the horizontal axis or not.
    fn anchor_size(
        &self,
        anchor_size: Option<u8>,
        rect: CssPixelRect,
        positioned_box: Node,
        containing_block: Node,
        is_horizontal_axis: bool,
    ) -> CssPixels {
        use crate::css::css_enums::anchor_size;
        // The logical <anchor-size> keywords (block, inline, self-block, and self-inline) map to one of the physical
        // keywords according to either the writing mode of the box (for self-block and self-inline) or the writing
        // mode of the box's containing block (for block and inline).
        let inline_axis_is_horizontal =
            |writing_mode_box: Node| self.style(writing_mode_box).writing_mode() == writing_mode::HORIZONTAL_TB;
        // If the <anchor-size> keyword is omitted, it defaults to behaving as whatever keyword matches the axis of
        // the property that anchor-size() is used in.
        let measures_width = match anchor_size {
            None => is_horizontal_axis,
            Some(anchor_size::WIDTH) => true,
            Some(anchor_size::HEIGHT) => false,
            Some(anchor_size::INLINE) => inline_axis_is_horizontal(containing_block),
            Some(anchor_size::BLOCK) => !inline_axis_is_horizontal(containing_block),
            Some(anchor_size::SELF_INLINE) => inline_axis_is_horizontal(positioned_box),
            Some(anchor_size::SELF_BLOCK) => !inline_axis_is_horizontal(positioned_box),
            Some(_) => unreachable!("anchor-size() holds an unknown <anchor-size> keyword"),
        };
        if measures_width { rect.width } else { rect.height }
    }

    fn note_resolved_anchor_function(
        &self,
        anchor_box: Node,
        is_horizontal_axis: bool,
        state: &mut AnchorResolutionState,
    ) {
        if state.default_anchor_box.is_invalid() {
            return;
        }
        if anchor_box != state.default_anchor_box
            && self.nearest_scroll_container_ancestor(anchor_box)
                != self.nearest_scroll_container_ancestor(state.default_anchor_box)
        {
            return;
        }
        if is_horizontal_axis {
            state.compensates_for_horizontal_scroll = true;
        } else {
            state.compensates_for_vertical_scroll = true;
        }
    }

    #[allow(clippy::too_many_arguments)]
    fn resolve_anchor_value(
        &self,
        calculated: *const c_void,
        positioned_box: Node,
        containing_block: Node,
        containing_block_geometry: Option<ContainingBlockGeometry>,
        entry_coordinate_space_box: Node,
        axis: AnchorValueAxis,
        resolution_state: &mut AnchorResolutionState,
    ) -> Option<CssPixels> {
        let mut callback_context = AnchorCalcCallbackContext {
            engine: self,
            positioned_box,
            containing_block,
            containing_block_geometry,
            entry_coordinate_space_box,
            is_from_end: axis.is_from_end,
            is_horizontal_axis: axis.is_horizontal,
            containing_block_extent: axis.containing_block_extent,
            resolution_state,
        };
        // SAFETY: The calculated handle is retained by the style cache and
        // all callback state remains live for this synchronous resolution.
        let result = unsafe {
            style_values::resolve_calc_with_external_resolutions(
                calculated,
                axis.containing_block_extent,
                (&raw mut callback_context).cast(),
                Some(resolve_anchor_non_math_function),
            )
        };
        result.resolved.then(|| CssPixels::truncated_value_for(result.value))
    }

    fn resolve_anchor_insets(
        &self,
        node: Node,
        containing_block: Node,
        entry_containing_block_geometry: Option<&ContainingBlockGeometry>,
        entry_coordinate_space_box: Node,
        position_area_anchor_box: Option<Node>,
    ) -> Option<formatting_context::ResolvedAnchorInsets> {
        let (resolved, default_scroll_shift) = self.resolve_anchor_insets_and_default_scroll_shift(
            node,
            containing_block,
            entry_containing_block_geometry,
            entry_coordinate_space_box,
        );
        // https://drafts.csswg.org/css-anchor-position/#scroll
        // A non-none position-area makes the box compensate for its default anchor's scroll in both axes.
        let default_scroll_shift = match position_area_anchor_box {
            Some(anchor_box) => AnchorResolutionState {
                default_anchor_box: anchor_box,
                compensates_for_horizontal_scroll: true,
                compensates_for_vertical_scroll: true,
            },
            None => default_scroll_shift,
        };
        self.callbacks.arena().set_default_scroll_shift(
            node,
            default_scroll_shift.default_anchor_box,
            default_scroll_shift.compensates_for_horizontal_scroll,
            default_scroll_shift.compensates_for_vertical_scroll,
        );
        resolved
    }

    fn resolve_anchor_insets_and_default_scroll_shift(
        &self,
        node: Node,
        containing_block: Node,
        entry_containing_block_geometry: Option<&ContainingBlockGeometry>,
        entry_coordinate_space_box: Node,
    ) -> (Option<formatting_context::ResolvedAnchorInsets>, AnchorResolutionState) {
        let style = self.style(node);
        if ![
            style.inset_top(),
            style.inset_right(),
            style.inset_bottom(),
            style.inset_left(),
        ]
        .into_iter()
        .any(style_values::InsetValue::contains_anchor_function)
        {
            return (None, AnchorResolutionState::NO_SCROLL_SHIFT);
        }

        if containing_block.is_invalid() {
            return (None, AnchorResolutionState::NO_SCROLL_SHIFT);
        }
        let containing_block_geometry = match entry_containing_block_geometry {
            Some(geometry) => *geometry,
            None => ContainingBlockGeometry::from_used_values(self.used(containing_block), FfiCssPixelPoint::default()),
        };
        let default_anchor_box = if style.has_position_anchor() {
            self.anchor_lookup(node, style.position_anchor_name())
                .unwrap_or(NodeSlotId::INVALID)
        } else {
            NodeSlotId::INVALID
        };
        let mut resolution_state = AnchorResolutionState {
            default_anchor_box,
            compensates_for_horizontal_scroll: false,
            compensates_for_vertical_scroll: false,
        };
        let mut resolve_inset = |inset: style_values::InsetValue<'_>, axis: AnchorValueAxis| {
            inset.contains_anchor_function().then(|| {
                self.resolve_anchor_value(
                    inset.anchor_bearing_calculated(),
                    node,
                    containing_block,
                    entry_containing_block_geometry.copied(),
                    entry_coordinate_space_box,
                    axis,
                    &mut resolution_state,
                )
                .map_or(
                    style_values::ResolvedInsetOverride::Auto,
                    style_values::ResolvedInsetOverride::Pixels,
                )
            })
        };
        let resolved = formatting_context::ResolvedAnchorInsets {
            top: resolve_inset(
                style.inset_top(),
                AnchorValueAxis {
                    is_from_end: false,
                    is_horizontal: false,
                    containing_block_extent: containing_block_geometry.padding_box_block_size(),
                },
            ),
            right: resolve_inset(
                style.inset_right(),
                AnchorValueAxis {
                    is_from_end: true,
                    is_horizontal: true,
                    containing_block_extent: containing_block_geometry.padding_box_inline_size(),
                },
            ),
            bottom: resolve_inset(
                style.inset_bottom(),
                AnchorValueAxis {
                    is_from_end: true,
                    is_horizontal: false,
                    containing_block_extent: containing_block_geometry.padding_box_block_size(),
                },
            ),
            left: resolve_inset(
                style.inset_left(),
                AnchorValueAxis {
                    is_from_end: false,
                    is_horizontal: true,
                    containing_block_extent: containing_block_geometry.padding_box_inline_size(),
                },
            ),
        };

        let default_scroll_shift =
            if resolution_state.compensates_for_horizontal_scroll || resolution_state.compensates_for_vertical_scroll {
                resolution_state
            } else {
                AnchorResolutionState::NO_SCROLL_SHIFT
            };
        (Some(resolved), default_scroll_shift)
    }

    /// Resolves the anchor-size() functions in the sizing and margin properties of `node`.
    /// https://drafts.csswg.org/css-anchor-position-1/#anchor-size-fn
    fn resolve_anchor_sizes(
        &self,
        node: Node,
        containing_block: Node,
        containing_block_geometry: &ContainingBlockGeometry,
        entry_coordinate_space_box: Node,
    ) -> style_values::ResolvedAnchorSizes {
        // NB: Read the computed values themselves, not what an earlier resolution made of them.
        let style = *StyleValues::for_node(&self.callbacks, node);
        let mut resolution_state = AnchorResolutionState::NO_SCROLL_SHIFT;
        let mut resolve = |calculated: *const c_void, is_horizontal: bool, containing_block_extent: CssPixels| {
            self.resolve_anchor_value(
                calculated,
                node,
                containing_block,
                Some(*containing_block_geometry),
                entry_coordinate_space_box,
                AnchorValueAxis {
                    is_from_end: false,
                    is_horizontal,
                    containing_block_extent,
                },
                &mut resolution_state,
            )
        };
        let inline_size = containing_block_geometry.padding_box_inline_size();
        let block_size = containing_block_geometry.padding_box_block_size();
        // https://drafts.csswg.org/css-anchor-position-1/#anchor-size-resolution
        // If no fallback value is specified, it makes the declaration referencing it invalid at computed-value time.
        let mut resolve_size = |size: &ComputedSize, is_horizontal: bool, initial: ComputedSizeKind| {
            if !size.is_length_percentage() || !size.length_percentage().contains_anchor_function() {
                return None;
            }
            let extent = if is_horizontal { inline_size } else { block_size };
            Some(
                match resolve(size.length_percentage().calculated_pointer(), is_horizontal, extent) {
                    Some(px) => style_values::ResolvedSizeOverride::Length(px),
                    None => style_values::ResolvedSizeOverride::Keyword(initial),
                },
            )
        };
        let mut resolved = style_values::ResolvedAnchorSizes {
            width: resolve_size(style.width(), true, ComputedSizeKind::Auto),
            min_width: resolve_size(style.min_width(), true, ComputedSizeKind::Auto),
            max_width: resolve_size(style.max_width(), true, ComputedSizeKind::None),
            height: resolve_size(style.height(), false, ComputedSizeKind::Auto),
            min_height: resolve_size(style.min_height(), false, ComputedSizeKind::Auto),
            max_height: resolve_size(style.max_height(), false, ComputedSizeKind::None),
            ..Default::default()
        };
        let mut resolve_margin = |margin: &ComputedLengthPercentageOrAuto, is_horizontal: bool| {
            let length_percentage = margin.length_percentage()?;
            if !length_percentage.contains_anchor_function() {
                return None;
            }
            // NB: Margin percentages resolve against the inline size of the containing block in both axes.
            Some(resolve(length_percentage.calculated_pointer(), is_horizontal, inline_size).unwrap_or_default())
        };
        resolved.margin_top = resolve_margin(style.margin_top(), false);
        resolved.margin_right = resolve_margin(style.margin_right(), true);
        resolved.margin_bottom = resolve_margin(style.margin_bottom(), false);
        resolved.margin_left = resolve_margin(style.margin_left(), true);
        resolved
    }

    /// Makes the resolved anchor sizes of `inputs` visible to the style reads of `node` in this run's records.
    fn install_resolved_anchor_sizes(&self, node: Node, inputs: &abspos_inputs::AbsposLayoutInputs) {
        if inputs.resolved_anchor_sizes.is_empty() {
            return;
        }
        let used = self.used(node);
        if used.anchor_resolved_values.get().is_none() {
            let _ = used
                .anchor_resolved_values
                .set(Box::new(style_values::AnchorResolvedValues::new(
                    &inputs.resolved_anchor_sizes,
                )));
        }
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#anchor-center
    /// If the positioned box has a default anchor box, then it is centered (insofar as possible) over the default
    /// anchor box in the relevant axis.
    /// NB: This handles an axis whose insets are both auto. A position area handles anchor-center itself.
    #[allow(clippy::too_many_arguments)]
    fn apply_anchor_center_alignment(
        &self,
        positioned_box: Node,
        containing_block: Node,
        containing_block_geometry: &ContainingBlockGeometry,
        entry_coordinate_space_box: Node,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
        static_position_rect: &mut abspos_inputs::StaticPositionRect,
        containing_block_info: &mut abspos_inputs::AbsposContainingBlockInfo,
    ) {
        let style = self.style(positioned_box).with_resolved_insets(resolved_anchor_insets);
        let centers_inline = style.justify_self() == justify_self::ANCHOR_CENTER
            && style.inset_left().is_auto()
            && style.inset_right().is_auto();
        let centers_block = style.align_self() == align_self::ANCHOR_CENTER
            && style.inset_top().is_auto()
            && style.inset_bottom().is_auto();
        if (!centers_inline && !centers_block)
            || !style.has_position_anchor()
            || !self.facts(positioned_box).is_absolutely_positioned()
        {
            return;
        }
        let Some(anchor_box) = self.anchor_lookup(positioned_box, style.position_anchor_name()) else {
            return;
        };
        let anchor_rect = self.anchor_rect(
            anchor_box,
            containing_block,
            Some(containing_block_geometry),
            entry_coordinate_space_box,
        );
        // NB: The static position rect and containing block rect are relative to the containing block's content box,
        //     while anchor_rect() is relative to its padding box.
        if centers_inline {
            static_position_rect.rect.offset.inline_offset = anchor_rect.left()
                - containing_block_geometry.padding_left
                - containing_block_info.rect.offset.inline_offset;
            static_position_rect.rect.size.inline_size = anchor_rect.width;
            static_position_rect.inline_alignment = StaticPositionAlignment::Center;
            containing_block_info.inline_axis_mode = abspos_inputs::AbsposAxisMode::InsetFromRect;
            containing_block_info.inline_alignment = Some(AbsposAlignment::AnchorCenter);
        }
        if centers_block {
            static_position_rect.rect.offset.block_offset = anchor_rect.top()
                - containing_block_geometry.padding_top
                - containing_block_info.rect.offset.block_offset;
            static_position_rect.rect.size.block_size = anchor_rect.height;
            static_position_rect.block_alignment = StaticPositionAlignment::Center;
            containing_block_info.block_axis_mode = abspos_inputs::AbsposAxisMode::InsetFromRect;
            containing_block_info.block_alignment = Some(AbsposAlignment::AnchorCenter);
        }
        static_position_rect.is_known = true;
        containing_block_info.derives_from_own_computed_values = true;
    }

    /// https://drafts.csswg.org/css-anchor-position-1/#position-area
    /// The used value of any auto inset properties [...] resolves to 0.
    /// NB: Only a stretched axis needs its auto insets as 0 here: the other alignments size the box to fit its
    ///     contents, and then place it within the position area themselves.
    fn position_area_stretch_insets(
        &self,
        node: Node,
        resolved: Option<formatting_context::ResolvedAnchorInsets>,
        stretches_inline: bool,
        stretches_block: bool,
    ) -> Option<formatting_context::ResolvedAnchorInsets> {
        if !stretches_inline && !stretches_block {
            return resolved;
        }
        let insets = resolved.unwrap_or_default();
        let style = self.style(node).with_resolved_insets(Some(&insets));
        let zero_if_auto = |value: style_values::InsetValue<'_>, stretches: bool, current| {
            if stretches && value.is_auto() {
                Some(style_values::ResolvedInsetOverride::Pixels(CssPixels::default()))
            } else {
                current
            }
        };
        Some(formatting_context::ResolvedAnchorInsets {
            top: zero_if_auto(style.inset_top(), stretches_block, insets.top),
            right: zero_if_auto(style.inset_right(), stretches_inline, insets.right),
            bottom: zero_if_auto(style.inset_bottom(), stretches_block, insets.bottom),
            left: zero_if_auto(style.inset_left(), stretches_inline, insets.left),
        })
    }

    /// The layout inputs a non-none position-area gives `positioned_box`: its static position rect (the default
    /// anchor box, for anchor-center), its containing block info, the geometry of its new containing block, its default
    /// anchor box, and whether each of its axes (inline, then block) stretches to fill its position area.
    #[allow(clippy::type_complexity)]
    fn position_area_layout_inputs(
        &self,
        positioned_box: Node,
        containing_block: Node,
        containing_block_geometry: &ContainingBlockGeometry,
        entry_coordinate_space_box: Node,
        mut containing_block_info: abspos_inputs::AbsposContainingBlockInfo,
    ) -> Option<(
        abspos_inputs::StaticPositionRect,
        abspos_inputs::AbsposContainingBlockInfo,
        ContainingBlockGeometry,
        Node,
        (bool, bool),
    )> {
        let style = self.style(positioned_box);
        let position_area = style.anchor().position_area.as_slice();
        if position_area.is_empty() {
            return None;
        }

        // https://drafts.csswg.org/css-anchor-position-1/#position-area
        // If the box does not have a default anchor box, or is not an absolutely positioned box, this value has no
        // effect.
        if !style.has_position_anchor() || !self.facts(positioned_box).is_absolutely_positioned() {
            return None;
        }
        let containing_block_style = self.style(containing_block);
        let tracks = resolve_position_area_tracks(
            position_area,
            PositionAreaWritingMode {
                writing_mode: containing_block_style.writing_mode(),
                direction: containing_block_style.direction(),
            },
            PositionAreaWritingMode {
                writing_mode: style.writing_mode(),
                direction: style.direction(),
            },
        )?;

        let anchor_box = self.anchor_lookup(positioned_box, style.position_anchor_name())?;
        let anchor_rect = self.anchor_rect(
            anchor_box,
            containing_block,
            Some(containing_block_geometry),
            entry_coordinate_space_box,
        );

        // anchor_rect() is padding-box-relative, while containing_block_info.rect is content-box-relative. Normalize
        // them before comparing their edges.
        let anchor_inline_offset = anchor_rect.left() - containing_block_geometry.padding_left;
        let anchor_block_offset = anchor_rect.top() - containing_block_geometry.padding_top;
        let anchor_inline_end = anchor_rect.right() - containing_block_geometry.padding_left;
        let anchor_block_end = anchor_rect.bottom() - containing_block_geometry.padding_top;
        let containing_block_end = geometry::LogicalOffset {
            inline_offset: containing_block_info.rect.offset.inline_offset
                + containing_block_info.rect.size.inline_size,
            block_offset: containing_block_info.rect.offset.block_offset + containing_block_info.rect.size.block_size,
        };

        // https://drafts.csswg.org/css-anchor-position-1/#position-area-grid-resolution
        // The position-area grid is a 3×3 grid, composed of four grid lines in each axis. In order (using the writing
        // mode of the containing block):
        // - the start edge of the box's pre-modification containing block, or the start edge of the default anchor
        //   box if that is more start-ward
        // - the start edge of the default anchor box
        // - the end edge of the default anchor box
        // - the end edge of the box's pre-modification containing block, or the end edge of the default anchor box
        //   if that is more end-ward.
        // NB: The lines are kept in physical order here; the resolved tracks are physical as well.
        let inline_lines = [
            containing_block_info
                .rect
                .offset
                .inline_offset
                .min(anchor_inline_offset),
            anchor_inline_offset,
            anchor_inline_end,
            containing_block_end.inline_offset.max(anchor_inline_end),
        ];
        let block_lines = [
            containing_block_info.rect.offset.block_offset.min(anchor_block_offset),
            anchor_block_offset,
            anchor_block_end,
            containing_block_end.block_offset.max(anchor_block_end),
        ];
        let inline_offset = inline_lines[tracks.horizontal.first as usize];
        let inline_size = inline_lines[tracks.horizontal.last as usize + 1] - inline_offset;
        let block_offset = block_lines[tracks.vertical.first as usize];
        let block_size = block_lines[tracks.vertical.last as usize + 1] - block_offset;

        // https://drafts.csswg.org/css-anchor-position-1/#position-area
        // Otherwise, selects a region of the position-area grid, and makes that the box's containing block.
        containing_block_info.rect = geometry::LogicalRect {
            offset: geometry::LogicalOffset {
                inline_offset,
                block_offset,
            },
            size: geometry::LogicalSize {
                inline_size,
                block_size,
            },
        };
        containing_block_info.inline_axis_mode = abspos_inputs::AbsposAxisMode::InsetFromRect;
        containing_block_info.block_axis_mode = abspos_inputs::AbsposAxisMode::InsetFromRect;

        // https://drafts.csswg.org/css-anchor-position-1/#position-area-alignment
        // When position-area is not none, the used value of normal self-alignment changes depending on the
        // <position-area> value, to align the box towards the anchor.
        let inline_alignment = self_alignment_for_position_area(
            style.justify_self(),
            tracks.horizontal,
            containing_block_style.direction() == direction::LTR,
            style.direction() == direction::LTR,
        );
        let block_alignment = self_alignment_for_position_area(
            justify_self_value_for_align_self(style.align_self()),
            tracks.vertical,
            true,
            true,
        );
        let stretches = (
            inline_alignment == AbsposAlignment::Stretch,
            block_alignment == AbsposAlignment::Stretch,
        );
        containing_block_info.inline_alignment = Some(inline_alignment);
        containing_block_info.block_alignment = Some(block_alignment);

        containing_block_info.derives_from_own_computed_values = true;

        let position_area_geometry = ContainingBlockGeometry {
            content_origin_in_entry_space: formatting_context::point_add(
                containing_block_geometry.content_origin_in_entry_space,
                FfiCssPixelPoint {
                    x: inline_offset,
                    y: block_offset,
                },
            ),
            padding_left: CssPixels::default(),
            padding_right: CssPixels::default(),
            padding_top: CssPixels::default(),
            padding_bottom: CssPixels::default(),
            content_inline_size: inline_size,
            content_block_size: block_size,
        };

        Some((
            // https://drafts.csswg.org/css-anchor-position-1/#anchor-center
            // Express the anchor in the new containing block's coordinate space so anchor-center can center over it.
            abspos_inputs::StaticPositionRect {
                rect: geometry::LogicalRect {
                    offset: geometry::LogicalOffset {
                        inline_offset: anchor_inline_offset - inline_offset,
                        block_offset: anchor_block_offset - block_offset,
                    },
                    size: geometry::LogicalSize {
                        inline_size: anchor_rect.width,
                        block_size: anchor_rect.height,
                    },
                },
                inline_alignment: StaticPositionAlignment::Center,
                block_alignment: StaticPositionAlignment::Center,
                alignment_derives_from_own_computed_values: true,
                is_known: true,
            },
            containing_block_info,
            position_area_geometry,
            anchor_box,
            stretches,
        ))
    }
}

/// The tracks of the position-area grid a <position-area> selects in one physical axis, counted from the left or top.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct PositionAreaTrackRange {
    first: u8,
    last: u8,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct PositionAreaTracks {
    horizontal: PositionAreaTrackRange,
    vertical: PositionAreaTrackRange,
}

#[derive(Clone, Copy)]
struct PositionAreaWritingMode {
    writing_mode: u8,
    direction: u8,
}

impl PositionAreaWritingMode {
    fn inline_axis_is_horizontal(self) -> bool {
        self.writing_mode == writing_mode::HORIZONTAL_TB
    }

    /// Whether the start side of the given physical axis is its left or top side.
    fn start_is_left_or_top(self, horizontal: bool) -> bool {
        geometry::start_is_left_or_top(self.writing_mode, self.direction, horizontal)
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PositionAreaAxis {
    Horizontal,
    Vertical,
    Block,
    Inline,
    Ambiguous,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PositionAreaReference {
    Physical,
    ContainingBlock,
    OwnWritingMode,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum PositionAreaSelection {
    Start,
    End,
    SpanStart,
    SpanEnd,
    Center,
    All,
}

fn classify_position_area_keyword(code: u8) -> (PositionAreaAxis, PositionAreaSelection, PositionAreaReference) {
    use PositionAreaAxis::*;
    use PositionAreaReference::*;
    use PositionAreaSelection::*;
    match code {
        position_area::LEFT => (Horizontal, Start, Physical),
        position_area::RIGHT => (Horizontal, End, Physical),
        position_area::SPAN_LEFT => (Horizontal, SpanStart, Physical),
        position_area::SPAN_RIGHT => (Horizontal, SpanEnd, Physical),
        position_area::X_START => (Horizontal, Start, ContainingBlock),
        position_area::X_END => (Horizontal, End, ContainingBlock),
        position_area::SPAN_X_START => (Horizontal, SpanStart, ContainingBlock),
        position_area::SPAN_X_END => (Horizontal, SpanEnd, ContainingBlock),
        position_area::SELF_X_START => (Horizontal, Start, OwnWritingMode),
        position_area::SELF_X_END => (Horizontal, End, OwnWritingMode),
        position_area::SPAN_SELF_X_START => (Horizontal, SpanStart, OwnWritingMode),
        position_area::SPAN_SELF_X_END => (Horizontal, SpanEnd, OwnWritingMode),
        position_area::TOP => (Vertical, Start, Physical),
        position_area::BOTTOM => (Vertical, End, Physical),
        position_area::SPAN_TOP => (Vertical, SpanStart, Physical),
        position_area::SPAN_BOTTOM => (Vertical, SpanEnd, Physical),
        position_area::Y_START => (Vertical, Start, ContainingBlock),
        position_area::Y_END => (Vertical, End, ContainingBlock),
        position_area::SPAN_Y_START => (Vertical, SpanStart, ContainingBlock),
        position_area::SPAN_Y_END => (Vertical, SpanEnd, ContainingBlock),
        position_area::SELF_Y_START => (Vertical, Start, OwnWritingMode),
        position_area::SELF_Y_END => (Vertical, End, OwnWritingMode),
        position_area::SPAN_SELF_Y_START => (Vertical, SpanStart, OwnWritingMode),
        position_area::SPAN_SELF_Y_END => (Vertical, SpanEnd, OwnWritingMode),
        position_area::BLOCK_START => (Block, Start, ContainingBlock),
        position_area::BLOCK_END => (Block, End, ContainingBlock),
        position_area::SPAN_BLOCK_START => (Block, SpanStart, ContainingBlock),
        position_area::SPAN_BLOCK_END => (Block, SpanEnd, ContainingBlock),
        position_area::INLINE_START => (Inline, Start, ContainingBlock),
        position_area::INLINE_END => (Inline, End, ContainingBlock),
        position_area::SPAN_INLINE_START => (Inline, SpanStart, ContainingBlock),
        position_area::SPAN_INLINE_END => (Inline, SpanEnd, ContainingBlock),
        position_area::SELF_BLOCK_START => (Block, Start, OwnWritingMode),
        position_area::SELF_BLOCK_END => (Block, End, OwnWritingMode),
        position_area::SPAN_SELF_BLOCK_START => (Block, SpanStart, OwnWritingMode),
        position_area::SPAN_SELF_BLOCK_END => (Block, SpanEnd, OwnWritingMode),
        position_area::SELF_INLINE_START => (Inline, Start, OwnWritingMode),
        position_area::SELF_INLINE_END => (Inline, End, OwnWritingMode),
        position_area::SPAN_SELF_INLINE_START => (Inline, SpanStart, OwnWritingMode),
        position_area::SPAN_SELF_INLINE_END => (Inline, SpanEnd, OwnWritingMode),
        position_area::START => (Ambiguous, Start, ContainingBlock),
        position_area::END => (Ambiguous, End, ContainingBlock),
        position_area::SPAN_START => (Ambiguous, SpanStart, ContainingBlock),
        position_area::SPAN_END => (Ambiguous, SpanEnd, ContainingBlock),
        position_area::SELF_START => (Ambiguous, Start, OwnWritingMode),
        position_area::SELF_END => (Ambiguous, End, OwnWritingMode),
        position_area::SPAN_SELF_START => (Ambiguous, SpanStart, OwnWritingMode),
        position_area::SPAN_SELF_END => (Ambiguous, SpanEnd, OwnWritingMode),
        position_area::SPAN_ALL => (Ambiguous, All, ContainingBlock),
        _ => (Ambiguous, Center, ContainingBlock),
    }
}

/// https://drafts.csswg.org/css-anchor-position-1/#position-area-syntax
fn resolve_position_area_tracks(
    codes: &[u8],
    containing_block: PositionAreaWritingMode,
    own: PositionAreaWritingMode,
) -> Option<PositionAreaTracks> {
    let writing_mode_for = |reference: PositionAreaReference| match reference {
        PositionAreaReference::OwnWritingMode => own,
        PositionAreaReference::Physical | PositionAreaReference::ContainingBlock => containing_block,
    };
    // Whether the keyword specifies the horizontal axis, if it is unambiguous about its axis.
    let axis_is_horizontal = |code: u8| {
        let (axis, _, reference) = classify_position_area_keyword(code);
        match axis {
            PositionAreaAxis::Horizontal => Some(true),
            PositionAreaAxis::Vertical => Some(false),
            PositionAreaAxis::Inline => Some(writing_mode_for(reference).inline_axis_is_horizontal()),
            PositionAreaAxis::Block => Some(!writing_mode_for(reference).inline_axis_is_horizontal()),
            PositionAreaAxis::Ambiguous => None,
        }
    };
    // INTEROP: When both keywords are self-* keywords, other browsers use the block axis of the box's own writing
    //          mode. This keeps a value like self-block-end self-inline-start working once it computes to its short
    //          form, self-end self-start.
    let (_, _, first_reference) = classify_position_area_keyword(codes[0]);
    let block_axis_is_horizontal = !writing_mode_for(first_reference).inline_axis_is_horizontal();

    if codes.is_empty() {
        return None;
    }

    // If only a single keyword is given, it behaves as if the second keyword is span-all if the given keyword is
    // unambiguous about its axis; otherwise, it behaves as if the given keyword was repeated.
    let (first, second) = match *codes {
        [first] if axis_is_horizontal(first).is_some() => (first, position_area::SPAN_ALL),
        [first] => (first, first),
        [first, second] => (first, second),
        _ => return None,
    };

    // If the other keyword is unambiguous about its axis, then the ambiguous keyword is referring to the opposite
    // axis. If both keywords are ambiguous, however, then the first refers to the block axis of the box's
    // containing block, and the second to the inline axis.
    let first_is_horizontal = match (axis_is_horizontal(first), axis_is_horizontal(second)) {
        (Some(first), Some(second)) if first == second => return None,
        (Some(first), _) => first,
        (None, Some(second)) => !second,
        (None, None) => block_axis_is_horizontal,
    };

    let range_for = |code: u8, horizontal: bool| {
        let (_, selection, reference) = classify_position_area_keyword(code);
        let (first, last) = match selection {
            PositionAreaSelection::Start => (0, 0),
            PositionAreaSelection::End => (2, 2),
            PositionAreaSelection::SpanStart => (0, 1),
            PositionAreaSelection::SpanEnd => (1, 2),
            PositionAreaSelection::Center => (1, 1),
            PositionAreaSelection::All => (0, 2),
        };
        let start_is_left_or_top = reference == PositionAreaReference::Physical
            || writing_mode_for(reference).start_is_left_or_top(horizontal);
        if start_is_left_or_top {
            PositionAreaTrackRange { first, last }
        } else {
            PositionAreaTrackRange {
                first: 2 - last,
                last: 2 - first,
            }
        }
    };
    let first_range = range_for(first, first_is_horizontal);
    let second_range = range_for(second, !first_is_horizontal);
    Some(if first_is_horizontal {
        PositionAreaTracks {
            horizontal: first_range,
            vertical: second_range,
        }
    } else {
        PositionAreaTracks {
            horizontal: second_range,
            vertical: first_range,
        }
    })
}

/// The justify-self value with the same meaning as an align-self value.
fn justify_self_value_for_align_self(value: u8) -> u8 {
    // NB: align-self and justify-self share their keyword values, except for left, right and anchor-center.
    match value {
        align_self::ANCHOR_CENTER => justify_self::ANCHOR_CENTER,
        value => value,
    }
}

/// The used self-alignment of a box with a non-none position-area in one physical axis, where Start and End are
/// the left/top and right/bottom sides. The computed value is a justify-self value.
fn self_alignment_for_position_area(
    computed: u8,
    tracks: PositionAreaTrackRange,
    containing_block_start_is_left_or_top: bool,
    own_start_is_left_or_top: bool,
) -> AbsposAlignment {
    let towards_start = |start_is_left_or_top: bool| {
        if start_is_left_or_top {
            AbsposAlignment::Start
        } else {
            AbsposAlignment::End
        }
    };
    let towards_end = |start_is_left_or_top: bool| {
        if start_is_left_or_top {
            AbsposAlignment::End
        } else {
            AbsposAlignment::Start
        }
    };
    match computed {
        justify_self::ANCHOR_CENTER => AbsposAlignment::AnchorCenter,
        justify_self::CENTER => AbsposAlignment::Center,
        justify_self::STRETCH => AbsposAlignment::Stretch,
        justify_self::START | justify_self::FLEX_START | justify_self::BASELINE => {
            towards_start(containing_block_start_is_left_or_top)
        }
        justify_self::END | justify_self::FLEX_END => towards_end(containing_block_start_is_left_or_top),
        justify_self::SELF_START => towards_start(own_start_is_left_or_top),
        justify_self::SELF_END => towards_end(own_start_is_left_or_top),
        justify_self::LEFT => AbsposAlignment::Start,
        justify_self::RIGHT => AbsposAlignment::End,
        // https://drafts.csswg.org/css-anchor-position-1/#position-area-alignment
        // - If the only the center track in an axis is selected, the default alignment in that axis is center.
        // - If all three tracks are selected, the default alignment in that axis is anchor-center.
        // - Otherwise, the default alignment in that axis is toward the non-specified side track: if it's
        //   specifying the “start” track of its axis, the default alignment in that axis is end; etc.
        _ => match (tracks.first, tracks.last) {
            (1, 1) => AbsposAlignment::Center,
            (0, 2) => AbsposAlignment::AnchorCenter,
            (0, _) => AbsposAlignment::End,
            _ => AbsposAlignment::Start,
        },
    }
}

unsafe extern "C" fn resolve_anchor_non_math_function(context: *mut c_void, shell: *const c_void) -> *const c_void {
    use crate::css::style_value::StyleValueData;
    // SAFETY: The CSS calc engine calls this only during resolve_anchor_value,
    // whose stack owns this callback context.
    let context = unsafe { &mut *context.cast::<AnchorCalcCallbackContext>() };
    // SAFETY: The engine pointer is live for the enclosing resolution.
    let engine = unsafe { &*context.engine };
    // SAFETY: `shell` is the live Rust style-value data retained by the CSS
    // calc core's non-math-function node for this synchronous resolution.
    let (has_anchor_name, explicit_anchor_name, fallback_value) = match unsafe { &*shell.cast::<StyleValueData>() } {
        StyleValueData::Anchor {
            has_anchor_name,
            anchor_name,
            fallback_value,
            ..
        }
        | StyleValueData::AnchorSize {
            has_anchor_name,
            anchor_name,
            fallback_value,
            ..
        } => (*has_anchor_name, anchor_name, fallback_value),
        _ => return std::ptr::null(),
    };
    let style = engine.style(context.positioned_box);
    let bound_anchor_name = has_anchor_name.then(|| explicit_anchor_name.to_fly_string());
    let anchor_name = if let Some(name) = &bound_anchor_name {
        Some(name.raw())
    } else if style.has_position_anchor() {
        Some(style.position_anchor_name())
    } else {
        None
    };
    if engine.facts(context.positioned_box).is_absolutely_positioned()
        && let Some(anchor_name) = anchor_name
        && let Some(anchor_box) = engine.anchor_lookup(context.positioned_box, anchor_name)
    {
        let rect = engine.anchor_rect(
            anchor_box,
            context.containing_block,
            context.containing_block_geometry.as_ref(),
            context.entry_coordinate_space_box,
        );
        // SAFETY: As above.
        let resolved = match unsafe { &*shell.cast::<StyleValueData>() } {
            StyleValueData::Anchor { anchor_side, .. } => engine
                .anchor_side(
                    anchor_side_from_style_value(anchor_side.data()),
                    rect,
                    context.positioned_box,
                    context.containing_block,
                    context.is_from_end,
                    context.is_horizontal_axis,
                )
                .map(|side| {
                    // SAFETY: The state pointer is live and uniquely used by this
                    // synchronous resolver.
                    let resolution_state = unsafe { &mut *context.resolution_state };
                    engine.note_resolved_anchor_function(anchor_box, context.is_horizontal_axis, resolution_state);
                    if context.is_from_end {
                        context.containing_block_extent - side
                    } else {
                        side
                    }
                }),
            StyleValueData::AnchorSize {
                has_anchor_size,
                anchor_size,
                ..
            } => Some(engine.anchor_size(
                has_anchor_size.then_some(*anchor_size),
                rect,
                context.positioned_box,
                context.containing_block,
                context.is_horizontal_axis,
            )),
            _ => unreachable!("the shell was matched as an anchor function above"),
        };
        if let Some(resolved) = resolved {
            // SAFETY: This CSS crate export transfers one Arc reference to
            // the external-resolution snapshot, which releases it after
            // calc resolution.
            return style_values::calc_node_create_px_dimension(resolved.to_double());
        }
    }

    // SAFETY: The callback context is live for this synchronous resolution.
    unsafe {
        style_values::anchor_function_fallback_node(
            fallback_value,
            context.containing_block_extent,
            std::ptr::from_mut(context).cast(),
            resolve_anchor_non_math_function,
        )
    }
}

/// https://drafts.csswg.org/css-align-3/#overflow-values
/// The offset of a box that overflows its containing block on one side, shifted so it stays within it, if it fits.
fn shift_into_containing_block(offset: CssPixels, maximum_offset: CssPixels) -> CssPixels {
    if maximum_offset < CssPixels::default() {
        return offset;
    }
    offset.max(CssPixels::default()).min(maximum_offset)
}

type AutoPx = Option<CssPixels>;

fn resolve_or_auto(value: style_values::InsetValue, basis: CssPixels) -> AutoPx {
    (!value.is_auto()).then(|| value.to_px(basis))
}

fn resolve_margin_or_auto(value: &ComputedLengthPercentageOrAuto, basis: CssPixels) -> AutoPx {
    (!value.is_auto()).then(|| value.to_px(basis))
}

fn auto_px_value(value: AutoPx) -> CssPixels {
    value.unwrap_or_default()
}

#[allow(clippy::too_many_arguments)]
pub(crate) fn solve_abspos_axis_for(
    available: CssPixels,
    target: AutoPx,
    clamp_to_zero: bool,
    start: AutoPx,
    margin_start: AutoPx,
    border_start: CssPixels,
    padding_start: CssPixels,
    size: AutoPx,
    padding_end: CssPixels,
    border_end: CssPixels,
    margin_end: AutoPx,
    end: AutoPx,
) -> CssPixels {
    let value = available
        - auto_px_value(start)
        - auto_px_value(margin_start)
        - border_start
        - padding_start
        - auto_px_value(size)
        - padding_end
        - border_end
        - auto_px_value(margin_end)
        - auto_px_value(end)
        + auto_px_value(target);
    if clamp_to_zero {
        value.max(CssPixels::default())
    } else {
        value
    }
}

// The block-axis unknowns of the absolute positioning equation. Each field is `None` while it is
// still unresolved, and holds its used value once solved for.
#[derive(Clone, Copy)]
struct BlockAxisSolution {
    block_size: AutoPx,
    top: AutoPx,
    bottom: AutoPx,
    margin_top: AutoPx,
    margin_bottom: AutoPx,
}

impl BlockAxisSolution {
    fn zero_out_auto_margins(&mut self) {
        self.margin_top = Some(auto_px_value(self.margin_top));
        self.margin_bottom = Some(auto_px_value(self.margin_bottom));
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ReplacedAxisSolution {
    pub(crate) start: CssPixels,
    pub(crate) end: CssPixels,
    pub(crate) margin_start: CssPixels,
    pub(crate) margin_end: CssPixels,
}

#[derive(Clone, Copy)]
pub(crate) struct ReplacedAxisBehavior {
    pub(crate) clear_auto_margins_if_start_is_auto: bool,
    pub(crate) clear_negative_auto_margins: bool,
}

pub(crate) fn solve_replaced_axis(
    available: CssPixels,
    mut start: AutoPx,
    mut end: AutoPx,
    mut margin_start: AutoPx,
    mut margin_end: AutoPx,
    static_offset: CssPixels,
    behavior: ReplacedAxisBehavior,
) -> ReplacedAxisSolution {
    if start.is_none() && end.is_none() {
        start = Some(static_offset);
    }
    if end.is_none() || (behavior.clear_auto_margins_if_start_is_auto && start.is_none()) {
        if margin_start.is_none() {
            margin_start = Some(CssPixels::default());
        }
        if margin_end.is_none() {
            margin_end = Some(CssPixels::default());
        }
    }
    if margin_start.is_none() && margin_end.is_none() {
        let remainder = available - auto_px_value(start) - auto_px_value(end);
        if behavior.clear_negative_auto_margins && remainder < CssPixels::default() {
            // This deliberately matches the C++ inline-axis implementation,
            // which zeroes both margins instead of solving the end margin.
            margin_start = Some(CssPixels::default());
            margin_end = Some(CssPixels::default());
        } else {
            margin_start = Some(remainder / 2);
            margin_end = Some(remainder / 2);
        }
    }
    if start.is_none() {
        start = Some(available - auto_px_value(end) - auto_px_value(margin_start) - auto_px_value(margin_end));
    } else if end.is_none() {
        end = Some(available - auto_px_value(start) - auto_px_value(margin_start) - auto_px_value(margin_end));
    } else if margin_start.is_none() {
        margin_start = Some(available - auto_px_value(start) - auto_px_value(end) - auto_px_value(margin_end));
    } else if margin_end.is_none() {
        margin_end = Some(available - auto_px_value(start) - auto_px_value(margin_start) - auto_px_value(end));
    }
    if CssPixels::default()
        != available
            - auto_px_value(start)
            - auto_px_value(end)
            - auto_px_value(margin_start)
            - auto_px_value(margin_end)
    {
        end = Some(available - auto_px_value(start) - auto_px_value(margin_start) - auto_px_value(margin_end));
    }
    ReplacedAxisSolution {
        start: auto_px_value(start),
        end: auto_px_value(end),
        margin_start: auto_px_value(margin_start),
        margin_end: auto_px_value(margin_end),
    }
}

impl AbsposEngine<'_> {
    fn static_offset(&self, node: Node, rect: abspos_inputs::StaticPositionRect) -> geometry::LogicalOffset {
        let used = self.used(node);
        let collapsed = used.uses_collapsing_borders_model.get();
        aligned_static_offset(
            rect,
            used.margin_box_inline_size(collapsed),
            used.margin_box_block_size(collapsed),
        )
    }

    /// The inner size that a size property gives the box in `axis`. A stretch size fills the inset-modified
    /// containing block, where an auto inset is the static position at the start, or the containing block edge.
    /// https://drafts.csswg.org/css-sizing-3/#stretch-fit-sizing
    #[expect(clippy::too_many_arguments)]
    fn calculate_inner_size(
        &self,
        node: Node,
        axis: SizingAxis,
        size: &ComputedSize,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: abspos_inputs::StaticPositionRect,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) -> CssPixels {
        let sizing = self.sizing();
        if !size.is_stretch() {
            return match axis {
                SizingAxis::Inline => {
                    sizing.calculate_inner_inline_size(node, available_space.inline_size, size, constraints)
                }
                SizingAxis::Block => sizing.calculate_inner_block_size(node, available_space, size, constraints),
            };
        }
        let style = self.style(node).with_resolved_insets(resolved_anchor_insets);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let (start, end, containing_block_size, percentage_basis) = match axis {
            SizingAxis::Inline => (
                style.inset_left(),
                style.inset_right(),
                containing_block_inline_size,
                constraints.percentage_basis_inline_size,
            ),
            SizingAxis::Block => (
                style.inset_top(),
                style.inset_bottom(),
                available_space.block_size.to_px_or_zero(),
                constraints.percentage_basis_block_size,
            ),
        };
        let start_offset = if !start.is_auto() {
            start.to_px(containing_block_size)
        } else if end.is_auto() {
            let static_offset = self.static_offset(node, static_position_rect);
            match axis {
                SizingAxis::Inline => static_offset.inline_offset,
                SizingAxis::Block => static_offset.block_offset,
            }
        } else {
            CssPixels::default()
        };
        let end_offset = if end.is_auto() {
            CssPixels::default()
        } else {
            end.to_px(containing_block_size)
        };
        let stretch_fit_size = sizing.stretch_fit_inner_size(
            node,
            axis,
            containing_block_size - start_offset - end_offset,
            constraints,
        );
        sizing.resolve_calc_size(node, axis, size, stretch_fit_size, percentage_basis)
    }

    #[expect(clippy::too_many_arguments)]
    fn solve_non_replaced_inline_once(
        &self,
        node: Node,
        containing_block_inline_size: CssPixels,
        _available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: abspos_inputs::StaticPositionRect,
        input_inline_size: AutoPx,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) -> (AutoPx, CssPixels, CssPixels, AutoPx, AutoPx) {
        let style = self.style(node).with_resolved_insets(resolved_anchor_insets);
        let used = self.used(node);
        let border_left = style.border_left_width();
        let border_right = style.border_right_width();
        let padding_left = used.padding_left.get();
        let padding_right = used.padding_right.get();
        let computed_left = style.inset_left();
        let computed_right = style.inset_right();
        let mut left = style.inset_left().to_px(containing_block_inline_size);
        let mut right = style.inset_right().to_px(containing_block_inline_size);
        let mut margin_left = resolve_margin_or_auto(style.margin_left(), containing_block_inline_size);
        let mut margin_right = resolve_margin_or_auto(style.margin_right(), containing_block_inline_size);
        let mut inline_size = input_inline_size;

        let solve_for_left = |inline_size: AutoPx, margin_left: AutoPx, margin_right: AutoPx, right: CssPixels| {
            containing_block_inline_size
                - auto_px_value(margin_left)
                - border_left
                - padding_left
                - auto_px_value(inline_size)
                - padding_right
                - border_right
                - auto_px_value(margin_right)
                - right
        };
        let solve_for_inline_size = |left: CssPixels, margin_left: AutoPx, margin_right: AutoPx, right: CssPixels| {
            (containing_block_inline_size
                - left
                - auto_px_value(margin_left)
                - border_left
                - padding_left
                - padding_right
                - border_right
                - auto_px_value(margin_right)
                - right)
                .max(CssPixels::default())
        };
        let solve_for_right = |left: CssPixels, inline_size: AutoPx, margin_left: AutoPx, margin_right: AutoPx| {
            containing_block_inline_size
                - left
                - auto_px_value(margin_left)
                - border_left
                - padding_left
                - auto_px_value(inline_size)
                - padding_right
                - border_right
                - auto_px_value(margin_right)
        };
        let shrink_to_fit = |left: CssPixels, margin_left: AutoPx, margin_right: AutoPx, right: CssPixels| {
            let available = solve_for_inline_size(left, margin_left, margin_right, right);
            let sizing = self.sizing();
            let preferred = sizing.calculate_max_content_inline_size(node, constraints);
            if preferred <= available {
                preferred
            } else {
                let preferred_minimum = sizing.calculate_min_content_inline_size(node, constraints);
                preferred_minimum.max(available).min(preferred)
            }
        };

        if computed_left.is_auto() && inline_size.is_none() && computed_right.is_auto() {
            if margin_left.is_none() {
                margin_left = Some(CssPixels::default());
            }
            if margin_right.is_none() {
                margin_right = Some(CssPixels::default());
            }
            let content_inline_size = shrink_to_fit(left, margin_left, margin_right, right);
            inline_size = Some(content_inline_size);
            self.used(node).set_content_inline_size(content_inline_size);
            left = self.static_offset(node, static_position_rect).inline_offset;
            right = solve_for_right(left, inline_size, margin_left, margin_right);
        }

        if !computed_left.is_auto() && inline_size.is_some() && !computed_right.is_auto() {
            let available_for_margins = containing_block_inline_size
                - border_left
                - padding_left
                - auto_px_value(inline_size)
                - padding_right
                - border_right
                - left
                - right;
            if margin_left.is_none() && margin_right.is_none() {
                margin_left = Some(available_for_margins / 2);
                margin_right = Some(available_for_margins / 2);
                return (inline_size, left, right, margin_left, margin_right);
            }
            if margin_left.is_none() {
                margin_left = Some(available_for_margins);
                return (inline_size, left, right, margin_left, margin_right);
            }
            if margin_right.is_none() {
                margin_right = Some(available_for_margins);
                return (inline_size, left, right, margin_left, margin_right);
            }
            right = solve_for_right(left, inline_size, margin_left, margin_right);
            return (inline_size, left, right, margin_left, margin_right);
        }

        if margin_left.is_none() {
            margin_left = Some(CssPixels::default());
        }
        if margin_right.is_none() {
            margin_right = Some(CssPixels::default());
        }

        if computed_left.is_auto() && inline_size.is_none() && !computed_right.is_auto() {
            inline_size = Some(shrink_to_fit(left, margin_left, margin_right, right));
            left = solve_for_left(inline_size, margin_left, margin_right, right);
        } else if computed_left.is_auto() && computed_right.is_auto() && inline_size.is_some() {
            left = self.static_offset(node, static_position_rect).inline_offset;
            right = solve_for_right(left, inline_size, margin_left, margin_right);
        } else if inline_size.is_none() && computed_right.is_auto() && !computed_left.is_auto() {
            inline_size = Some(shrink_to_fit(left, margin_left, margin_right, right));
            right = solve_for_right(left, inline_size, margin_left, margin_right);
        } else if computed_left.is_auto() && inline_size.is_some() && !computed_right.is_auto() {
            left = solve_for_left(inline_size, margin_left, margin_right, right);
        } else if inline_size.is_none() && !computed_left.is_auto() && !computed_right.is_auto() {
            inline_size = Some(solve_for_inline_size(left, margin_left, margin_right, right));
        } else if computed_right.is_auto() && !computed_left.is_auto() && inline_size.is_some() {
            right = solve_for_right(left, inline_size, margin_left, margin_right);
        }

        (inline_size, left, right, margin_left, margin_right)
    }

    fn compute_inline_size_for_non_replaced(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: abspos_inputs::StaticPositionRect,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) {
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let style = self.style(node);
        let sizing = self.sizing();
        let initial = if self.facts(node).is_table_wrapper() {
            Some(sizing.compute_table_box_inline_size_inside_wrapper(
                node,
                available_space,
                constraints,
                None,
                formatting_context::TableWrapperInlineSizeMode::ClampToAvailableInlineSize,
            ))
        } else if style.width().is_auto() {
            None
        } else {
            Some(self.calculate_inner_size(
                node,
                SizingAxis::Inline,
                style.width(),
                available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            ))
        };
        let (mut used_inline_size, mut left, mut right, mut margin_left, mut margin_right) = self
            .solve_non_replaced_inline_once(
                node,
                containing_block_inline_size,
                available_space,
                constraints,
                static_position_rect,
                initial,
                resolved_anchor_insets,
            );

        // NB: A calc-size() with an auto basis resolves against the inline size auto gives the box.
        if style.width().is_auto() && style.width().calc_size().is_some() {
            let inline_size = sizing.resolve_calc_size(
                node,
                SizingAxis::Inline,
                style.width(),
                auto_px_value(used_inline_size),
                constraints.percentage_basis_inline_size,
            );
            (used_inline_size, left, right, margin_left, margin_right) = self.solve_non_replaced_inline_once(
                node,
                containing_block_inline_size,
                available_space,
                constraints,
                static_position_rect,
                Some(inline_size),
                resolved_anchor_insets,
            );
        }

        if !sizing.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            let max_inline_size = self.calculate_inner_size(
                node,
                SizingAxis::Inline,
                style.max_width(),
                available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            );
            if auto_px_value(used_inline_size) > max_inline_size {
                (used_inline_size, left, right, margin_left, margin_right) = self.solve_non_replaced_inline_once(
                    node,
                    containing_block_inline_size,
                    available_space,
                    constraints,
                    static_position_rect,
                    Some(max_inline_size),
                    resolved_anchor_insets,
                );
            }
        }
        if let Some(limits) = sizing.transferred_inline_size_limits(node, available_space, constraints) {
            let value = auto_px_value(used_inline_size);
            let limited = limits.clamp(value);
            if limited != value {
                (used_inline_size, left, right, margin_left, margin_right) = self.solve_non_replaced_inline_once(
                    node,
                    containing_block_inline_size,
                    available_space,
                    constraints,
                    static_position_rect,
                    Some(limited),
                    resolved_anchor_insets,
                );
            }
        }
        if !crate::layout::sizing_context::is_plain_auto(style.min_width()) {
            let min_inline_size = self.calculate_inner_size(
                node,
                SizingAxis::Inline,
                style.min_width(),
                available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            );
            if auto_px_value(used_inline_size) < min_inline_size {
                (used_inline_size, left, right, margin_left, margin_right) = self.solve_non_replaced_inline_once(
                    node,
                    containing_block_inline_size,
                    available_space,
                    constraints,
                    static_position_rect,
                    Some(min_inline_size),
                    resolved_anchor_insets,
                );
            }
        }

        let used = self.used(node);
        used.set_content_inline_size(auto_px_value(used_inline_size));
        used.inset_left.set(left);
        used.inset_right.set(right);
        used.margin_left.set(auto_px_value(margin_left));
        used.margin_right.set(auto_px_value(margin_right));
    }

    fn compute_inline_size_for_replaced(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: abspos_inputs::StaticPositionRect,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) {
        let sizing = self.sizing();
        // NB: A stretch inline size fills the inset-modified containing block, which replaced element sizing does not
        //     know about.
        let inline_size = if self.style(node).width().is_stretch() {
            self.calculate_inner_size(
                node,
                SizingAxis::Inline,
                self.style(node).width(),
                available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            )
        } else {
            sizing.compute_inline_size_for_replaced_element(node, available_space, constraints)
        };
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let style = self.style(node).with_resolved_insets(resolved_anchor_insets);
        let used = self.used(node);
        let available = containing_block_inline_size
            - inline_size
            - style.border_left_width()
            - used.padding_left.get()
            - used.padding_right.get()
            - style.border_right_width();
        let solution = solve_replaced_axis(
            available,
            resolve_or_auto(style.inset_left(), containing_block_inline_size),
            resolve_or_auto(style.inset_right(), containing_block_inline_size),
            resolve_margin_or_auto(style.margin_left(), containing_block_inline_size),
            resolve_margin_or_auto(style.margin_right(), containing_block_inline_size),
            self.static_offset(node, static_position_rect).inline_offset,
            ReplacedAxisBehavior {
                clear_auto_margins_if_start_is_auto: true,
                clear_negative_auto_margins: true,
            },
        );

        let used = self.used(node);
        used.inset_left.set(solution.start);
        used.inset_right.set(solution.end);
        used.margin_left.set(solution.margin_start);
        used.margin_right.set(solution.margin_end);
        used.set_content_inline_size(inline_size);
    }

    fn compute_inline_size(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: abspos_inputs::StaticPositionRect,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) {
        if self
            .sizing()
            .box_is_sized_as_replaced_element(node, available_space, constraints)
        {
            self.compute_inline_size_for_replaced(
                node,
                available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            );
        } else {
            self.compute_inline_size_for_non_replaced(
                node,
                available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            );
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum BlockSizePass {
    BeforeInsideLayout,
    AfterInsideLayout {
        automatic_content_block_size_of_inside_layout: Option<CssPixels>,
    },
}

impl AbsposEngine<'_> {
    fn apply_min_max_block_size_constraints(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: abspos_inputs::StaticPositionRect,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
        unconstrained: AutoPx,
    ) -> AutoPx {
        let style = self.style(node);
        let mut constrained = unconstrained;
        let calculate_inner_size = |size: &ComputedSize| {
            self.calculate_inner_size(
                node,
                SizingAxis::Block,
                size,
                available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            )
        };
        if !style.max_height().is_none() {
            let maximum = calculate_inner_size(style.max_height());
            if maximum < auto_px_value(constrained) {
                constrained = Some(maximum);
            }
        }
        if !crate::layout::sizing_context::is_plain_auto(style.min_height()) {
            let minimum = calculate_inner_size(style.min_height());
            if minimum > auto_px_value(constrained) {
                constrained = Some(minimum);
            }
        }
        constrained
    }

    fn automatic_block_size(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        pass: BlockSizePass,
    ) -> AutoPx {
        if self.facts(node).creates_block_formatting_context() {
            let BlockSizePass::AfterInsideLayout {
                automatic_content_block_size_of_inside_layout,
            } = pass
            else {
                return None;
            };
            return Some(automatic_content_block_size_of_inside_layout.unwrap_or_default());
        }
        let inner = self
            .used(node)
            .available_inner_space_or_constraints_from(available_space);
        Some(
            self.sizing()
                .calculate_fit_content_size(node, SizingAxis::Block, inner, constraints),
        )
    }

    #[expect(clippy::too_many_arguments)]
    fn solve_non_replaced_block_once(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: abspos_inputs::StaticPositionRect,
        pass: BlockSizePass,
        block_size: AutoPx,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) -> BlockAxisSolution {
        let style = self.style(node).with_resolved_insets(resolved_anchor_insets);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let containing_block_block_size = available_space.block_size.to_px_or_zero();
        let used = self.used(node);
        let padding_top = used.padding_top.get();
        let padding_bottom = used.padding_bottom.get();
        let mut solution = BlockAxisSolution {
            block_size,
            top: resolve_or_auto(style.inset_top(), containing_block_block_size),
            bottom: resolve_or_auto(style.inset_bottom(), containing_block_block_size),
            margin_top: resolve_margin_or_auto(style.margin_top(), containing_block_inline_size),
            margin_bottom: resolve_margin_or_auto(style.margin_bottom(), containing_block_inline_size),
        };

        // Solves the block axis equation for `target`, which must be one of the solution's own
        // fields; every other field contributes its current value.
        let solve_for = |target: AutoPx, clamp_to_zero: bool, solution: BlockAxisSolution| {
            solve_abspos_axis_for(
                containing_block_block_size,
                target,
                clamp_to_zero,
                solution.top,
                solution.margin_top,
                style.border_top_width(),
                padding_top,
                solution.block_size,
                padding_bottom,
                style.border_bottom_width(),
                solution.margin_bottom,
                solution.bottom,
            )
        };

        if solution.top.is_none() && solution.block_size.is_none() && solution.bottom.is_none() {
            solution.zero_out_auto_margins();
            let Some(automatic) = self.automatic_block_size(node, available_space, constraints, pass) else {
                return solution;
            };
            solution.block_size = Some(automatic);
            let constrained = self.apply_min_max_block_size_constraints(
                node,
                available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
                solution.block_size,
            );
            self.used(node).set_content_block_size(auto_px_value(constrained));
            solution.top = Some(self.static_offset(node, static_position_rect).block_offset);
            solution.bottom = Some(solve_for(solution.bottom, false, solution));
        } else if solution.top.is_some() && solution.block_size.is_some() && solution.bottom.is_some() {
            if solution.margin_top.is_none() && solution.margin_bottom.is_none() {
                let total = Some(auto_px_value(solution.margin_top) + auto_px_value(solution.margin_bottom));
                let remainder = solve_for(total, false, solution);
                solution.margin_top = Some(remainder / 2);
                solution.margin_bottom = Some(remainder / 2);
            } else if solution.margin_top.is_none() {
                solution.margin_top = Some(solve_for(solution.margin_top, false, solution));
            } else if solution.margin_bottom.is_none() {
                solution.margin_bottom = Some(solve_for(solution.margin_bottom, false, solution));
            } else {
                solution.bottom = Some(solve_for(solution.bottom, false, solution));
            }
        } else {
            solution.zero_out_auto_margins();

            if solution.top.is_none() && solution.block_size.is_none() && solution.bottom.is_some() {
                let Some(automatic) = self.automatic_block_size(node, available_space, constraints, pass) else {
                    return solution;
                };
                solution.block_size = Some(automatic);
                solution.top = Some(solve_for(solution.top, false, solution));
            } else if solution.top.is_none() && solution.bottom.is_none() && solution.block_size.is_some() {
                solution.top = Some(self.static_offset(node, static_position_rect).block_offset);
                solution.bottom = Some(solve_for(solution.bottom, false, solution));
            } else if solution.block_size.is_none() && solution.bottom.is_none() && solution.top.is_some() {
                let Some(automatic) = self.automatic_block_size(node, available_space, constraints, pass) else {
                    return solution;
                };
                solution.block_size = Some(automatic);
                solution.bottom = Some(solve_for(solution.bottom, false, solution));
            } else if solution.top.is_none() && solution.block_size.is_some() && solution.bottom.is_some() {
                solution.top = Some(solve_for(solution.top, false, solution));
            } else if solution.block_size.is_none() && solution.top.is_some() && solution.bottom.is_some() {
                solution.block_size = Some(solve_for(solution.block_size, true, solution));
            } else if solution.bottom.is_none() && solution.top.is_some() && solution.block_size.is_some() {
                solution.bottom = Some(solve_for(solution.bottom, false, solution));
            }
        }
        solution
    }

    fn compute_block_size_for_non_replaced(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: abspos_inputs::StaticPositionRect,
        pass: BlockSizePass,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) {
        let style = self.style(node);
        let mut intrinsic_available_space = available_space;
        intrinsic_available_space.inline_size = AvailableSize::definite(self.used(node).content_inline_size.get());
        let initial = if self.facts(node).is_table_wrapper() {
            Some(
                self.sizing()
                    .compute_table_box_block_size_inside_wrapper(node, available_space, constraints),
            )
        } else if self
            .sizing()
            .should_treat_block_size_as_auto(node, available_space, constraints)
        {
            None
        } else {
            Some(self.calculate_inner_size(
                node,
                SizingAxis::Block,
                style.height(),
                intrinsic_available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            ))
        };
        let mut solution = self.solve_non_replaced_block_once(
            node,
            available_space,
            constraints,
            static_position_rect,
            pass,
            initial,
            resolved_anchor_insets,
        );

        // NB: As in the inline axis, a calc-size() with an auto basis resolves against the block size auto gives the
        //     box.
        if style.height().is_auto()
            && style.height().calc_size().is_some()
            && let Some(auto_block_size) = solution.block_size
        {
            let block_size = self.sizing().resolve_calc_size(
                node,
                SizingAxis::Block,
                style.height(),
                auto_block_size,
                constraints.percentage_basis_block_size,
            );
            solution = self.solve_non_replaced_block_once(
                node,
                available_space,
                constraints,
                static_position_rect,
                pass,
                Some(block_size),
                resolved_anchor_insets,
            );
        }

        if solution.block_size.is_some() && !style.max_height().is_none() {
            let max_block_size = self.calculate_inner_size(
                node,
                SizingAxis::Block,
                style.max_height(),
                intrinsic_available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            );
            if auto_px_value(solution.block_size) > max_block_size {
                solution = self.solve_non_replaced_block_once(
                    node,
                    available_space,
                    constraints,
                    static_position_rect,
                    pass,
                    Some(max_block_size),
                    resolved_anchor_insets,
                );
            }
        }
        if solution.block_size.is_some() && !crate::layout::sizing_context::is_plain_auto(style.min_height()) {
            let min_block_size = self.calculate_inner_size(
                node,
                SizingAxis::Block,
                style.min_height(),
                intrinsic_available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            );
            if auto_px_value(solution.block_size) < min_block_size {
                solution = self.solve_non_replaced_block_once(
                    node,
                    available_space,
                    constraints,
                    static_position_rect,
                    pass,
                    Some(min_block_size),
                    resolved_anchor_insets,
                );
            }
        }
        if solution.block_size.is_none() {
            solution.block_size = self.apply_min_max_block_size_constraints(
                node,
                available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
                solution.block_size,
            );
        }

        let used = self.used(node);
        used.set_content_block_size(auto_px_value(solution.block_size));
        if style.height().is_auto() && pass == BlockSizePass::BeforeInsideLayout {
            return;
        }
        if !style.height().is_intrinsic_sizing_constraint() {
            used.has_definite_block_size.set(true);
        }
        used.inset_top.set(auto_px_value(solution.top));
        used.inset_bottom.set(auto_px_value(solution.bottom));
        // NOTE: solve_non_replaced_block_once() already resolved these against the bases the C++ layout code used:
        //       the insets against the containing block's block size, but the margins against its inline size.
        used.margin_top.set(auto_px_value(solution.margin_top));
        used.margin_bottom.set(auto_px_value(solution.margin_bottom));
    }

    fn compute_block_size_for_replaced(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: abspos_inputs::StaticPositionRect,
        pass: BlockSizePass,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) {
        // NB: As for the inline axis, a stretch block size fills the inset-modified containing block.
        let mut block_size = if self.style(node).height().is_stretch() {
            self.calculate_inner_size(
                node,
                SizingAxis::Block,
                self.style(node).height(),
                available_space,
                constraints,
                static_position_rect,
                resolved_anchor_insets,
            )
        } else {
            self.sizing()
                .compute_block_size_for_replaced_element(node, available_space, constraints)
        };
        if let BlockSizePass::AfterInsideLayout {
            automatic_content_block_size_of_inside_layout,
        } = pass
            && self
                .sizing()
                .block_size_is_ratio_dependent(node, available_space, constraints)
        {
            // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-minimum
            // NB: A box that is only sized like a replaced element through its aspect ratio grows to fit its content.
            // INTEROP: CSS Positioned Layout says "The automatic minimum size of an absolutely-positioned box is always
            //          zero", but Chrome applies the aspect-ratio minimum to absolutely positioned boxes too.
            let content_block_size = automatic_content_block_size_of_inside_layout.and_then(|size| {
                formatting_context::content_block_size_for_aspect_ratio_minimum(
                    self.records,
                    &self.callbacks,
                    node,
                    size,
                )
            });
            if let Some(minimum) = self.sizing().automatic_minimum_size_from_aspect_ratio(
                node,
                SizingAxis::Block,
                self.used(node).content_inline_size.get(),
                block_size,
                available_space,
                constraints,
                content_block_size,
            ) {
                block_size = block_size.max(minimum);
            }
        }
        let containing_block_block_size = available_space.block_size.to_px_or_zero();
        let style = self.style(node).with_resolved_insets(resolved_anchor_insets);
        let used = self.used(node);
        let available = containing_block_block_size
            - block_size
            - style.border_top_width()
            - used.padding_top.get()
            - used.padding_bottom.get()
            - style.border_bottom_width();
        // Deliberately pass false for `clear_auto_margins_if_start_is_auto`:
        // this matches the C++ condition, which tests only the end inset.
        let solution = solve_replaced_axis(
            available,
            resolve_or_auto(style.inset_top(), containing_block_block_size),
            resolve_or_auto(style.inset_bottom(), containing_block_block_size),
            resolve_margin_or_auto(style.margin_top(), containing_block_block_size),
            resolve_margin_or_auto(style.margin_bottom(), containing_block_block_size),
            self.static_offset(node, static_position_rect).block_offset,
            ReplacedAxisBehavior {
                clear_auto_margins_if_start_is_auto: false,
                clear_negative_auto_margins: false,
            },
        );

        let used = self.used(node);
        used.set_content_block_size(block_size);
        if style.height().is_auto() && pass == BlockSizePass::BeforeInsideLayout {
            // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
            // A block size transferred through the preferred aspect ratio from a definite inline size is definite, so
            // the content resolves percentages against it.
            if !self
                .sizing()
                .should_treat_block_size_as_auto(node, available_space, constraints)
            {
                used.has_definite_block_size.set(true);
            }
            return;
        }
        if !style.height().is_intrinsic_sizing_constraint() {
            used.has_definite_block_size.set(true);
        }
        used.inset_top.set(solution.start);
        used.inset_bottom.set(solution.end);
        used.margin_top.set(solution.margin_start);
        used.margin_bottom.set(solution.margin_end);
    }

    fn compute_block_size(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: abspos_inputs::StaticPositionRect,
        pass: BlockSizePass,
        resolved_anchor_insets: Option<&formatting_context::ResolvedAnchorInsets>,
    ) {
        if self
            .sizing()
            .box_is_sized_as_replaced_element(node, available_space, constraints)
        {
            self.compute_block_size_for_replaced(
                node,
                available_space,
                constraints,
                static_position_rect,
                pass,
                resolved_anchor_insets,
            );
        } else {
            self.compute_block_size_for_non_replaced(
                node,
                available_space,
                constraints,
                static_position_rect,
                pass,
                resolved_anchor_insets,
            );
        }
    }
}

impl<'pass> AbsposEngine<'pass> {
    // Run-prelude sizing for an absolutely positioned root: box-model
    // metrics, the inset-aware inline solve, the pre-inside-layout block
    // pass, and the definiteness overrides insets and aspect ratios provide.
    pub(crate) fn dimension_out_of_flow_root(&self, node: Node, inputs: abspos_inputs::AbsposLayoutInputs) {
        self.install_resolved_anchor_sizes(node, &inputs);
        let (available_space, constraints) = out_of_flow_root_space(inputs);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let resolved = inputs.resolved_anchor_insets.as_ref();
        debug_assert!(
            resolved.is_some()
                || !(self.style(node).inset_top().contains_anchor_function()
                    || self.style(node).inset_right().contains_anchor_function()
                    || self.style(node).inset_bottom().contains_anchor_function()
                    || self.style(node).inset_left().contains_anchor_function()),
            "an out-of-flow root with anchor() insets must arrive with their resolutions"
        );
        let style = self.style(node).with_resolved_insets(resolved);
        {
            let used = self.used(node);
            used.border_left.set(style.border_left_width());
            used.border_right.set(style.border_right_width());
            used.border_top.set(style.border_top_width());
            used.border_bottom.set(style.border_bottom_width());
            used.padding_left
                .set(style.padding_left().to_px(containing_block_inline_size));
            used.padding_right
                .set(style.padding_right().to_px(containing_block_inline_size));
            used.padding_top
                .set(style.padding_top().to_px(containing_block_inline_size));
            used.padding_bottom
                .set(style.padding_bottom().to_px(containing_block_inline_size));
        }

        self.compute_inline_size(
            node,
            available_space,
            constraints,
            static_position_rect_for_sizing(&inputs),
            resolved,
        );
        self.compute_block_size(
            node,
            available_space,
            constraints,
            static_position_rect_for_sizing(&inputs),
            BlockSizePass::BeforeInsideLayout,
            resolved,
        );

        {
            let used = self.used(node);
            if !style.inset_left().is_auto() && !style.inset_right().is_auto() {
                used.has_definite_inline_size.set(true);
            }
            if !style.inset_top().is_auto()
                && !style.inset_bottom().is_auto()
                && (style.height().is_auto() || !style.height().is_intrinsic_sizing_constraint())
            {
                used.has_definite_block_size.set(true);
            }
        }
        if !self.facts(node).creates_block_formatting_context() {
            let block_size_resolved_from_aspect_ratio = style.height().is_auto()
                && self.facts(node).has_preferred_aspect_ratio()
                && self.used(node).has_definite_inline_size();
            let used = self.used(node);
            used.has_definite_inline_size.set(true);
            if (!style.height().is_auto() && !style.height().is_intrinsic_sizing_constraint())
                || block_size_resolved_from_aspect_ratio
            {
                used.has_definite_block_size.set(true);
            }
        }

        self.sizing().make_form_control_content_box_definite(
            node,
            LayoutMode::Normal,
            available_space,
            constraints,
            None,
        );
    }

    pub(crate) fn finalize_out_of_flow_root_after_inside_layout(
        &self,
        node: Node,
        inputs: abspos_inputs::AbsposLayoutInputs,
        automatic_content_block_size_of_inside_layout: Option<CssPixels>,
    ) {
        self.install_resolved_anchor_sizes(node, &inputs);
        let (available_space, constraints) = out_of_flow_root_space(inputs);
        let containing_block_size = geometry::LogicalSize {
            inline_size: available_space.inline_size.to_px_or_zero(),
            block_size: available_space.block_size.to_px_or_zero(),
        };
        let resolved = inputs.resolved_anchor_insets.as_ref();
        let style = self.style(node).with_resolved_insets(resolved);
        if style.height().is_auto() {
            self.compute_block_size(
                node,
                available_space,
                constraints,
                static_position_rect_for_sizing(&inputs),
                BlockSizePass::AfterInsideLayout {
                    automatic_content_block_size_of_inside_layout,
                },
                resolved,
            );
        }

        {
            let used = self.used(node);
            let collapsed = used.uses_collapsing_borders_model.get();
            // https://drafts.csswg.org/css-anchor-position-1/#position-area-alignment
            // NB: A position area aligns the box within its inset-modified containing block even when both insets
            //     in an axis are specified.
            let aligns_with_insets = inputs.containing_block_info.derives_from_own_computed_values;
            let alignment_insets = |start: style_values::InsetValue<'_>, end: style_values::InsetValue<'_>, basis| {
                if start.is_auto() && end.is_auto() {
                    Some((CssPixels::default(), CssPixels::default()))
                } else if aligns_with_insets && !start.is_auto() && !end.is_auto() {
                    Some((start.to_px(basis), end.to_px(basis)))
                } else {
                    None
                }
            };
            if let Some(inline_alignment) = inputs.containing_block_info.inline_alignment
                && let Some((start_inset, end_inset)) = alignment_insets(
                    style.inset_left(),
                    style.inset_right(),
                    containing_block_size.inline_size,
                )
            {
                let margin_box_size = used.margin_box_inline_size(collapsed);
                let available = containing_block_size.inline_size - start_inset - end_inset - margin_box_size;
                let inset = match inline_alignment {
                    // NB: By default, an anchor-centered box shifts to stay within its containing block.
                    AbsposAlignment::AnchorCenter => Some(shift_into_containing_block(
                        self.static_offset(node, inputs.static_position_rect).inline_offset,
                        containing_block_size.inline_size - margin_box_size,
                    )),
                    AbsposAlignment::Center => Some(start_inset + available / 2),
                    AbsposAlignment::Start => Some(start_inset),
                    AbsposAlignment::End => Some(start_inset + available),
                    _ => None,
                };
                if let Some(inset) = inset {
                    used.inset_left.set(inset);
                    used.inset_right
                        .set(containing_block_size.inline_size - inset - margin_box_size);
                }
            }
            if let Some(block_alignment) = inputs.containing_block_info.block_alignment
                && let Some((start_inset, end_inset)) = alignment_insets(
                    style.inset_top(),
                    style.inset_bottom(),
                    containing_block_size.block_size,
                )
            {
                let margin_box_size = used.margin_box_block_size(collapsed);
                let available = containing_block_size.block_size - start_inset - end_inset - margin_box_size;
                let inset = match block_alignment {
                    AbsposAlignment::AnchorCenter => Some(shift_into_containing_block(
                        self.static_offset(node, inputs.static_position_rect).block_offset,
                        containing_block_size.block_size - margin_box_size,
                    )),
                    AbsposAlignment::Center => Some(start_inset + available / 2),
                    AbsposAlignment::Start | AbsposAlignment::SelfStart => Some(start_inset),
                    AbsposAlignment::End | AbsposAlignment::SelfEnd => Some(start_inset + available),
                    _ => None,
                };
                if let Some(inset) = inset {
                    used.inset_top.set(inset);
                    used.inset_bottom
                        .set(containing_block_size.block_size - inset - margin_box_size);
                }
            }
        }
    }

    fn layout_element(&self, run: &FormattingContextRun<'pass>, node: Node, inputs: abspos_inputs::AbsposLayoutInputs) {
        assert!(!self.facts(node).is_svg_box());
        let (available_space, constraints) = out_of_flow_root_space(inputs);

        match formatting_context::layout_inside_child(
            run,
            None,
            None,
            node,
            LayoutMode::Normal,
            LayoutInput::new(
                available_space,
                constraints,
                ParticipationInParentFormattingContext::AbsolutelyPositioned(inputs),
            ),
            false,
        ) {
            ChildLayoutOutcome::Created(_) | ChildLayoutOutcome::Skipped => {}
            ChildLayoutOutcome::ReenterCurrent => {
                unreachable!("abspos child with contents did not establish a formatting context")
            }
        }

        let static_offset = self.static_offset(node, inputs.static_position_rect);
        let used = self.used(node);
        let collapsed = used.uses_collapsing_borders_model.get();
        let mut used_offset = geometry::LogicalOffset {
            inline_offset: if inputs.containing_block_info.inline_axis_mode
                == abspos_inputs::AbsposAxisMode::StaticPosition
            {
                static_offset.inline_offset
            } else {
                inputs.containing_block_info.rect.offset.inline_offset + used.inset_left.get()
            },
            block_offset: if inputs.containing_block_info.block_axis_mode
                == abspos_inputs::AbsposAxisMode::StaticPosition
            {
                static_offset.block_offset
            } else {
                inputs.containing_block_info.rect.offset.block_offset + used.inset_top.get()
            },
        };
        used_offset.inline_offset += used.margin_left.get() + used.border_box_left(collapsed);
        used_offset.block_offset += used.margin_top.get() + used.border_box_top(collapsed);
        let is_measurement = self.purpose.is_measurement();
        if !is_measurement {
            self.used(node).rare_data_mut().abspos_layout_inputs = Some(inputs);
        }
        formatting_context::place_child_in_containing_block(
            run,
            node,
            inputs.containing_block,
            FfiCssPixelPoint {
                x: used_offset.inline_offset,
                y: used_offset.block_offset,
            },
            None,
        );
    }

    pub(crate) fn layout_pending_child(
        &self,
        run: &FormattingContextRun<'pass>,
        mut child: abspos_inputs::PendingAbsposChild,
    ) {
        debug_assert!(!self.purpose.is_measurement());
        let child_box = child.child_box;
        self.records
            .create_used_values(&self.callbacks, child_box, ContainingBlockConstraints::default());
        let containing_block_geometry = self.containing_block_geometry_for_pending_child(&child);
        let inline_containing_block_rect =
            self.inline_containing_block_rect_for_pending_child(&child, &containing_block_geometry);
        let translation_into_containing_block_space = formatting_context::point_sub(
            FfiCssPixelPoint::default(),
            containing_block_geometry.content_origin_in_entry_space,
        );
        child.static_position_rect = formatting_context::translate_static_position_rect(
            child.static_position_rect,
            translation_into_containing_block_space,
        );
        let containing_block_info_override = child.containing_block_info_override.or_else(|| {
            self.fragments
                .as_deref()
                .and_then(|fragments| fragments.find_abspos_containing_block_info(child_box))
        });
        let unresolved_containing_block_info = containing_block_info_override.unwrap_or_else(|| {
            self.base_containing_block_info(
                child_box,
                inline_containing_block_rect,
                &containing_block_geometry,
                None,
            )
        });
        let position_area_inputs = self.position_area_layout_inputs(
            child_box,
            child.containing_block(),
            &containing_block_geometry,
            child.coordinate_space_box,
            unresolved_containing_block_info,
        );
        let (containing_block_info, resolved, resolved_anchor_sizes) = if let Some((
            static_position_rect,
            position_area_containing_block_info,
            position_area_geometry,
            anchor_box,
            (stretches_inline, stretches_block),
        )) = position_area_inputs
        {
            child.static_position_rect = static_position_rect;
            let resolved_anchor_sizes = self.resolve_anchor_sizes(
                child_box,
                child.containing_block(),
                &position_area_geometry,
                child.coordinate_space_box,
            );
            let resolved = self.resolve_anchor_insets(
                child_box,
                child.containing_block(),
                Some(&position_area_geometry),
                child.coordinate_space_box,
                Some(anchor_box),
            );
            let resolved = self.position_area_stretch_insets(child_box, resolved, stretches_inline, stretches_block);
            (position_area_containing_block_info, resolved, resolved_anchor_sizes)
        } else {
            let resolved_anchor_sizes = self.resolve_anchor_sizes(
                child_box,
                child.containing_block(),
                &containing_block_geometry,
                child.coordinate_space_box,
            );
            let resolved = self.resolve_anchor_insets(
                child_box,
                child.containing_block(),
                Some(&containing_block_geometry),
                child.coordinate_space_box,
                None,
            );
            let mut containing_block_info = containing_block_info_override.unwrap_or_else(|| {
                self.base_containing_block_info(
                    child_box,
                    inline_containing_block_rect,
                    &containing_block_geometry,
                    resolved.as_ref(),
                )
            });
            self.apply_anchor_center_alignment(
                child_box,
                child.containing_block(),
                &containing_block_geometry,
                child.coordinate_space_box,
                resolved.as_ref(),
                &mut child.static_position_rect,
                &mut containing_block_info,
            );
            (containing_block_info, resolved, resolved_anchor_sizes)
        };
        let inputs = abspos_inputs::AbsposLayoutInputs {
            containing_block: child.containing_block(),
            inline_containing_block: child.inline_containing_block(),
            static_position_rect: child.static_position_rect,
            containing_block_info,
            resolved_anchor_insets: resolved,
            resolved_anchor_sizes,
        };
        self.install_resolved_anchor_sizes(child_box, &inputs);
        self.layout_element(run, child_box, inputs);
    }

    /// Joins the entry against the inline containing block rect its IFC
    /// contributed, folding the payload's resting space into the containing
    /// block's content-box space (no padding term, unlike anchor rects).
    fn inline_containing_block_rect_for_pending_child(
        &self,
        child: &abspos_inputs::PendingAbsposChild,
        containing_block_geometry: &ContainingBlockGeometry,
    ) -> Option<CssPixelRect> {
        if child.inline_containing_block().is_invalid() {
            return None;
        }
        let fragments = self.fragments.as_deref()?;
        let (rect, payload_space) = fragments.find_inline_containing_block_rect(child.inline_containing_block())?;
        let fold_into_entry_space =
            self.translation_between_payload_resting_spaces(payload_space, child.coordinate_space_box);
        Some(CssPixelRect {
            x: rect.x + fold_into_entry_space.x - containing_block_geometry.content_origin_in_entry_space.x,
            y: rect.y + fold_into_entry_space.y - containing_block_geometry.content_origin_in_entry_space.y,
            width: rect.width,
            height: rect.height,
        })
    }

    pub(super) fn replay(&self, run: &FormattingContextRun<'pass>, node: Node) {
        let mut inputs = self
            .callbacks
            .saved_abspos_layout_inputs(node)
            .expect("abspos relayout root must have committed layout inputs");
        if !inputs.containing_block_info.derives_from_own_computed_values {
            let (inline, block) = axis_modes(
                self.style(node)
                    .with_resolved_insets(inputs.resolved_anchor_insets.as_ref()),
            );
            inputs.containing_block_info.inline_axis_mode = inline;
            inputs.containing_block_info.block_axis_mode = block;
        }
        // Partial relayout uses a fresh state and creates the replay root
        // exactly once.
        self.records
            .create_used_values(&self.callbacks, node, ContainingBlockConstraints::default());
        self.layout_element(run, node, inputs);
    }

    fn compute_inset(
        &self,
        node: Node,
        containing_block_size: geometry::LogicalSize,
        formatting_context_root: Node,
        treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
    ) {
        // Most boxes are neither relatively positioned nor carry anchor()
        // insets, and both facts come straight from the node's style payload.
        let initial_style = self.style(node);
        let has_anchor_insets = initial_style.inset_top().contains_anchor_function()
            || initial_style.inset_right().contains_anchor_function()
            || initial_style.inset_bottom().contains_anchor_function()
            || initial_style.inset_left().contains_anchor_function();
        if !has_anchor_insets && initial_style.position() != positioning::RELATIVE {
            return;
        }
        // Anchor resolution also refreshes the box's default scroll shift, so
        // an anchor-bearing box resolves its insets even when it turns out not
        // to be relatively positioned.
        let resolved = if has_anchor_insets {
            self.resolve_anchor_insets(
                node,
                self.callbacks.in_flow_containing_block(node),
                None,
                NodeSlotId::INVALID,
                None,
            )
        } else {
            None
        };
        let style = initial_style.with_resolved_insets(resolved.as_ref());
        if style.position() != positioning::RELATIVE {
            return;
        }

        let resolve_opposing = |first: style_values::InsetValue, second: style_values::InsetValue, basis: CssPixels| {
            let resolved_first = first.to_px(basis);
            let resolved_second = second.to_px(basis);
            if first.is_auto() && second.is_auto() {
                (CssPixels::default(), CssPixels::default())
            } else if first.is_auto() {
                (-resolved_second, resolved_second)
            } else {
                (resolved_first, -resolved_first)
            }
        };
        let (left, right) = resolve_opposing(
            style.inset_left(),
            style.inset_right(),
            containing_block_size.inline_size,
        );

        let treat_block_axis_percentage_insets_as_auto = (style.inset_top().contains_percentage()
            || style.inset_bottom().contains_percentage())
            && !formatting_context::resolve_block_axis_percentage_inset_basis_is_definite(
                self.records,
                &self.callbacks,
                self.callbacks.in_flow_containing_block(node),
                formatting_context_root,
                treat_block_axis_percentage_insets_as_auto_beyond_root,
            );
        fn block_axis_inset_value(
            value: style_values::InsetValue<'_>,
            treat_percentage_as_auto: bool,
        ) -> style_values::InsetValue<'_> {
            if treat_percentage_as_auto && value.contains_percentage() {
                style_values::InsetValue::auto_value()
            } else {
                value
            }
        }
        let (top, bottom) = resolve_opposing(
            block_axis_inset_value(style.inset_top(), treat_block_axis_percentage_insets_as_auto),
            block_axis_inset_value(style.inset_bottom(), treat_block_axis_percentage_insets_as_auto),
            containing_block_size.block_size,
        );
        let used = self.used(node);
        used.inset_left.set(left);
        used.inset_right.set(right);
        used.inset_top.set(top);
        used.inset_bottom.set(bottom);
    }
}

pub(crate) fn drain_abspos_with_placed_containing_blocks(
    records: &RunRecords<'_>,
    callbacks: LayoutPass<'_>,
    should_collect_devtools_layout_data: bool,
    entry_fragments: &std::rc::Rc<fragment_tree::RunFragmentBuilder>,
) {
    let accumulator_root = entry_fragments.root_node();
    let run = FormattingContextRun {
        purpose: formatting_context::LayoutPurpose::Commit,
        records,
        box_: accumulator_root,
        layout_mode: LayoutMode::Normal,
        callbacks,
        should_collect_devtools_layout_data,
        treat_block_axis_percentage_insets_as_auto_beyond_root: false,
        fragments: Some(entry_fragments.clone()),
        previous_line_data: None,
        fragmentainer: None,
    };
    loop {
        let batch = entry_fragments.take_drainable_abspos(accumulator_root, records, &callbacks);
        if batch.is_empty() {
            break;
        }
        let engine = AbsposEngine::for_run(&run);
        for entry in batch {
            engine.layout_pending_child(&run, entry);
        }
    }
}

pub(crate) fn compute_inset_native(
    run: &FormattingContextRun<'_>,
    node: Node,
    inline_size: CssPixels,
    block_size: CssPixels,
) {
    AbsposEngine::for_run(run).compute_inset(
        node,
        geometry::LogicalSize {
            inline_size,
            block_size,
        },
        run.box_,
        run.treat_block_axis_percentage_insets_as_auto_beyond_root,
    );
}
