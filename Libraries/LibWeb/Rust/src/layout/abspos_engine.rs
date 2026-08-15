/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

fn axis_modes(style: StyleValues) -> (AbsposAxisMode, AbsposAxisMode) {
    (
        if style.inset_left().is_auto() && style.inset_right().is_auto() {
            AbsposAxisMode::StaticPosition
        } else {
            AbsposAxisMode::InsetFromRect
        },
        if style.inset_top().is_auto() && style.inset_bottom().is_auto() {
            AbsposAxisMode::StaticPosition
        } else {
            AbsposAxisMode::InsetFromRect
        },
    )
}

pub(crate) fn aligned_static_offset(
    static_position_rect: StaticPositionRect,
    margin_box_inline_size: CssPixels,
    margin_box_block_size: CssPixels,
) -> LogicalOffset {
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

fn out_of_flow_root_space(inputs: AbsposLayoutInputs) -> (AvailableSpace, ContainingBlockConstraints) {
    let containing_block_size = LogicalSize {
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

pub(crate) struct AbsposEngine {
    purpose: LayoutPurpose,
    records: std::rc::Rc<RunRecords>,
    callbacks: FfiLayoutFcCallbacks,
    fragments: Option<std::rc::Rc<crate::layout::RunFragmentBuilder>>,
}

impl AbsposEngine {
    pub(crate) fn for_run(run: &crate::layout::FormattingContextRun) -> Self {
        Self {
            purpose: run.purpose,
            records: run.records.clone(),
            callbacks: run.callbacks,
            fragments: run.fragments.clone(),
        }
    }

    fn containing_block_geometry_for_pending_child(&self, entry: &PendingAbsposChild) -> ContainingBlockGeometry {
        let containing_block = self.callbacks.containing_block(entry.child_box);
        let Some(containing_block_used) = self.records.used_values_if_owned(containing_block) else {
            panic!(
                "no owned record for the containing block of slot {} (cb slot {})",
                entry.child_box.slot_index(),
                containing_block.slot_index()
            );
        };
        let content_origin_in_entry_space =
            self.origin_of_containing_block_in_entry_space(containing_block, entry.coordinate_space_box);
        ContainingBlockGeometry::from_used_values(&containing_block_used, content_origin_in_entry_space)
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
            origin = point_add(origin, used.content_offset.get());
            chain_box = self.callbacks.containing_block(chain_box);
            if chain_box.is_invalid() {
                break;
            }
        }
        let mut space_box = entry_space;
        let mut space_origin = FfiCssPixelPoint::default();
        loop {
            if let Some((_, origin)) = origins_by_chain_box.iter().find(|(chain_box, _)| *chain_box == space_box) {
                return point_sub(*origin, space_origin);
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
            space_origin = point_add(space_origin, used.content_offset.get());
            space_box = self.callbacks.containing_block(space_box);
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
        while merge_point != to_space && !self.callbacks.is_ancestor(merge_point, to_space) {
            merge_point = self.callbacks.containing_block(merge_point);
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
                offset = point_add(offset, used.content_offset.get());
                current = self.callbacks.containing_block(current);
                assert!(!current.is_invalid());
            }
            offset
        };
        point_sub(
            offset_relative_to_merge_point(from_space),
            offset_relative_to_merge_point(to_space),
        )
    }

    fn sizing(&self) -> SizingContext {
        SizingContext::new(self.purpose, self.records.clone(), self.callbacks)
    }

    fn style(&self, node: Node) -> StyleValues<'static> {
        StyleValues::for_node(&self.callbacks, node)
    }

    fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.callbacks, node)
    }

    #[track_caller]
    fn used(&self, node: Node) -> std::rc::Rc<UsedValues> {
        self.records.used_values(node)
    }

    fn base_containing_block_info(
        &self,
        node: Node,
        inline_containing_block_rect: Option<PhysicalRect>,
        entry_containing_block_geometry: &ContainingBlockGeometry,
        resolved_anchor_insets: Option<&ResolvedAnchorInsets>,
    ) -> AbsposContainingBlockInfo {
        let style = self.style(node).with_resolved_insets(resolved_anchor_insets);
        let (inline_axis_mode, block_axis_mode) = axis_modes(style);
        let containing_block = self.callbacks.containing_block(node);
        assert!(!containing_block.is_invalid());
        if let Some(rect) = inline_containing_block_rect {
            return AbsposContainingBlockInfo {
                rect: LogicalRect {
                    offset: LogicalOffset {
                        inline_offset: rect.x,
                        block_offset: rect.y,
                    },
                    size: LogicalSize {
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

        AbsposContainingBlockInfo {
            rect: LogicalRect {
                offset: LogicalOffset {
                    inline_offset: -entry_containing_block_geometry.padding_left,
                    block_offset: -entry_containing_block_geometry.padding_top,
                },
                size: LogicalSize {
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

fn calc_node_create_px_dimension(value: f64) -> *const c_void {
    crate::css::calc::rust_calc_node_create_numeric_dimension(
        CALC_NUMERIC_KIND_LENGTH,
        value,
        crate::css::style_compute::px_length_unit(),
    )
    .cast()
}

struct AnchorResolutionState {
    default_anchor_box: Node,
    compensates_for_horizontal_scroll: bool,
    compensates_for_vertical_scroll: bool,
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
struct AnchorCalcCallbackContext {
    engine: *const AbsposEngine,
    positioned_box: Node,
    containing_block: Node,
    containing_block_geometry: Option<ContainingBlockGeometry>,
    entry_coordinate_space_box: Node,
    is_from_end: bool,
    is_horizontal_axis: bool,
    containing_block_extent: CssPixels,
    resolution_state: *mut AnchorResolutionState,
}

impl AbsposEngine {
    fn anchor_lookup(&self, positioned_box: Node, anchor_name: usize) -> Option<Node> {
        let eligible_anchor_shells = self
            .fragments
            .as_deref()
            .map(|fragments| fragments.anchor_candidate_shells(&self.callbacks))
            .unwrap_or_default();
        // SAFETY: The name handle is retained by either the style snapshot or
        let anchor_box = unsafe {
            (self.callbacks.anchor_lookup)(
                self.callbacks.context,
                self.callbacks.shell(positioned_box),
                anchor_name,
                eligible_anchor_shells.as_ptr(),
                eligible_anchor_shells.len(),
            )
        };
        (!anchor_box.is_invalid()).then_some(anchor_box)
    }

    fn nearest_scroll_container_ancestor(&self, node: Node) -> Node {
        let mut ancestor = self.callbacks.containing_block(node);
        while !ancestor.is_invalid() {
            if self.facts(ancestor).is_scroll_container() {
                return ancestor;
            }
            ancestor = self.callbacks.containing_block(ancestor);
        }
        NodeSlotId::INVALID
    }

    fn anchor_rect(
        &self,
        anchor_box: Node,
        containing_block: Node,
        entry_containing_block_geometry: Option<&ContainingBlockGeometry>,
        entry_coordinate_space_box: Node,
    ) -> PhysicalRect {
        let (rect, coordinate_space_box) = self
            .fragments
            .as_deref()
            .and_then(|fragments| fragments.find_anchor_candidate(anchor_box))
            .expect("an accepted anchor is placed in the draining subtree and propagated here");
        match entry_containing_block_geometry {
            Some(geometry) => {
                let fold_into_entry_space =
                    self.translation_between_payload_resting_spaces(coordinate_space_box, entry_coordinate_space_box);
                PhysicalRect {
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
                PhysicalRect {
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
        rect: PhysicalRect,
        positioned_box: Node,
        containing_block: Node,
        is_from_end: bool,
        is_horizontal_axis: bool,
    ) -> Option<CssPixels> {
        let containing_block_direction = self.style(containing_block).direction();
        let box_direction = self.style(positioned_box).direction();
        match side {
            AnchorSide::Invalid => None,
            AnchorSide::Top => (!is_horizontal_axis).then_some(rect.top()),
            AnchorSide::Bottom => (!is_horizontal_axis).then_some(rect.bottom()),
            AnchorSide::Left => is_horizontal_axis.then_some(rect.left()),
            AnchorSide::Right => is_horizontal_axis.then_some(rect.right()),
            AnchorSide::Center => Some(if is_horizontal_axis {
                rect.left() + rect.width / 2
            } else {
                rect.top() + rect.height / 2
            }),
            AnchorSide::Start | AnchorSide::End => {
                let is_start = side == AnchorSide::Start;
                if is_horizontal_axis {
                    let use_left = (containing_block_direction == direction::LTR) == is_start;
                    Some(if use_left { rect.left() } else { rect.right() })
                } else {
                    Some(if is_start { rect.top() } else { rect.bottom() })
                }
            }
            AnchorSide::SelfStart | AnchorSide::SelfEnd => {
                let is_start = side == AnchorSide::SelfStart;
                if is_horizontal_axis {
                    let use_left = (box_direction == direction::LTR) == is_start;
                    Some(if use_left { rect.left() } else { rect.right() })
                } else {
                    Some(if is_start { rect.top() } else { rect.bottom() })
                }
            }
            AnchorSide::Inside | AnchorSide::Outside => {
                let same_side = side == AnchorSide::Inside;
                if is_horizontal_axis {
                    Some(if is_from_end == same_side {
                        rect.right()
                    } else {
                        rect.left()
                    })
                } else {
                    Some(if is_from_end == same_side {
                        rect.bottom()
                    } else {
                        rect.top()
                    })
                }
            }
            AnchorSide::Percentage(fraction) => {
                if is_horizontal_axis {
                    let (start, end) = if containing_block_direction == direction::LTR {
                        (rect.left(), rect.right())
                    } else {
                        (rect.right(), rect.left())
                    };
                    Some(start + CssPixels::nearest_value_for((end - start).to_double() * fraction))
                } else {
                    Some(rect.top() + CssPixels::nearest_value_for(rect.height.to_double() * fraction))
                }
            }
        }
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
        value: InsetValue,
        positioned_box: Node,
        containing_block: Node,
        containing_block_geometry: Option<ContainingBlockGeometry>,
        entry_coordinate_space_box: Node,
        axis: AnchorValueAxis,
        resolution_state: &mut AnchorResolutionState,
    ) -> Option<CssPixels> {
        assert!(value.contains_anchor_function());
        let calculated = value.anchor_bearing_calculated();
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
            resolve_calc_with_external_resolutions(
                calculated,
                axis.containing_block_extent,
                (&raw mut callback_context).cast(),
                Some(resolve_anchor_non_math_function),
            )
        };
        result.resolved.then(|| CssPixels::nearest_value_for(result.value))
    }

    fn resolve_anchor_insets(
        &self,
        node: Node,
        entry_containing_block_geometry: Option<&ContainingBlockGeometry>,
        entry_coordinate_space_box: Node,
    ) -> Option<ResolvedAnchorInsets> {
        // Clear a stale default scroll shift before any early return.
        // SAFETY: The node is live and a null anchor clears the weak target.
        unsafe {
            (self.callbacks.set_default_scroll_shift)(
                self.callbacks.context,
                self.callbacks.shell(node),
                std::ptr::null_mut(),
                false,
                false,
            );
        }

        let style = self.style(node);
        let top_contains_anchor = style.inset_top().contains_anchor_function();
        let right_contains_anchor = style.inset_right().contains_anchor_function();
        let bottom_contains_anchor = style.inset_bottom().contains_anchor_function();
        let left_contains_anchor = style.inset_left().contains_anchor_function();
        if !top_contains_anchor && !right_contains_anchor && !bottom_contains_anchor && !left_contains_anchor {
            return None;
        }

        let containing_block = self.callbacks.containing_block(node);
        if containing_block.is_invalid() {
            return None;
        }
        let containing_block_geometry = match entry_containing_block_geometry {
            Some(geometry) => *geometry,
            None => {
                ContainingBlockGeometry::from_used_values(&self.used(containing_block), FfiCssPixelPoint::default())
            }
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
        let mut resolved = ResolvedAnchorInsets::default();

        if top_contains_anchor {
            let value = self.resolve_anchor_value(
                style.inset_top(),
                node,
                containing_block,
                entry_containing_block_geometry.copied(),
                entry_coordinate_space_box,
                AnchorValueAxis {
                    is_from_end: false,
                    is_horizontal: false,
                    containing_block_extent: containing_block_geometry.padding_box_block_size(),
                },
                &mut resolution_state,
            );
            resolved.resolves_top = true;
            resolved.top_is_auto = value.is_none();
            resolved.top = value.unwrap_or_default();
        }
        if right_contains_anchor {
            let value = self.resolve_anchor_value(
                style.inset_right(),
                node,
                containing_block,
                entry_containing_block_geometry.copied(),
                entry_coordinate_space_box,
                AnchorValueAxis {
                    is_from_end: true,
                    is_horizontal: true,
                    containing_block_extent: containing_block_geometry.padding_box_inline_size(),
                },
                &mut resolution_state,
            );
            resolved.resolves_right = true;
            resolved.right_is_auto = value.is_none();
            resolved.right = value.unwrap_or_default();
        }
        if bottom_contains_anchor {
            let value = self.resolve_anchor_value(
                style.inset_bottom(),
                node,
                containing_block,
                entry_containing_block_geometry.copied(),
                entry_coordinate_space_box,
                AnchorValueAxis {
                    is_from_end: true,
                    is_horizontal: false,
                    containing_block_extent: containing_block_geometry.padding_box_block_size(),
                },
                &mut resolution_state,
            );
            resolved.resolves_bottom = true;
            resolved.bottom_is_auto = value.is_none();
            resolved.bottom = value.unwrap_or_default();
        }
        if left_contains_anchor {
            let value = self.resolve_anchor_value(
                style.inset_left(),
                node,
                containing_block,
                entry_containing_block_geometry.copied(),
                entry_coordinate_space_box,
                AnchorValueAxis {
                    is_from_end: false,
                    is_horizontal: true,
                    containing_block_extent: containing_block_geometry.padding_box_inline_size(),
                },
                &mut resolution_state,
            );
            resolved.resolves_left = true;
            resolved.left_is_auto = value.is_none();
            resolved.left = value.unwrap_or_default();
        }

        if resolution_state.compensates_for_horizontal_scroll || resolution_state.compensates_for_vertical_scroll {
            // SAFETY: The anchor and positioned box remain live through the
            // pass; C++ stores the anchor as a weak pointer.
            unsafe {
                (self.callbacks.set_default_scroll_shift)(
                    self.callbacks.context,
                    self.callbacks.shell(node),
                    self.callbacks.shell(resolution_state.default_anchor_box),
                    resolution_state.compensates_for_horizontal_scroll,
                    resolution_state.compensates_for_vertical_scroll,
                );
            }
        }

        Some(resolved)
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
    let StyleValueData::Anchor {
        has_anchor_name,
        anchor_name: explicit_anchor_name,
        anchor_side,
        fallback_value,
    } = (unsafe { &*shell.cast::<StyleValueData>() })
    else {
        return std::ptr::null();
    };
    let style = engine.style(context.positioned_box);
    let anchor_name = if *has_anchor_name {
        Some(explicit_anchor_name.raw())
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
        if let Some(side) = engine.anchor_side(
            anchor_side_from_style_value(anchor_side.data()),
            rect,
            context.positioned_box,
            context.containing_block,
            context.is_from_end,
            context.is_horizontal_axis,
        ) {
            // SAFETY: The state pointer is live and uniquely used by this
            // synchronous resolver.
            let resolution_state = unsafe { &mut *context.resolution_state };
            engine.note_resolved_anchor_function(anchor_box, context.is_horizontal_axis, resolution_state);
            let inset = if context.is_from_end {
                context.containing_block_extent - side
            } else {
                side
            };
            // SAFETY: This CSS crate export transfers one Arc reference to
            // the external-resolution snapshot, which releases it after
            // calc resolution.
            return calc_node_create_px_dimension(inset.to_double());
        }
    }

    match fallback_value.optional_data() {
        None => std::ptr::null(),
        Some(StyleValueData::Length { value, unit }) => {
            // Computed anchor() fallbacks hold absolute lengths only; relative
            // units carry NaN canonical ratios.
            let ratio = crate::css::style_compute::LENGTH_UNIT_CANONICAL_PX_RATIOS[*unit as usize];
            assert!(ratio.is_finite());
            calc_node_create_px_dimension(CssPixels::nearest_value_for(*value * ratio).to_double())
        }
        Some(StyleValueData::Percentage { value }) => {
            calc_node_create_px_dimension(context.containing_block_extent.to_double() * (value / 100.0))
        }
        Some(StyleValueData::Calculated { .. }) => {
            let mut nested_context = *context;
            // SAFETY: The anchor() shell retains the fallback calculation for
            // this synchronous resolution.
            let resolved = unsafe {
                resolve_calc_with_external_resolutions(
                    fallback_value.pointer().cast(),
                    context.containing_block_extent,
                    (&raw mut nested_context).cast(),
                    Some(resolve_anchor_non_math_function),
                )
            };
            if !resolved.resolved {
                return std::ptr::null();
            }
            calc_node_create_px_dimension(resolved.value)
        }
        Some(StyleValueData::Anchor { .. }) => {
            let mut nested_context = *context;
            // SAFETY: The outer anchor() shell retains the nested anchor()
            // for this synchronous resolution.
            unsafe { resolve_anchor_non_math_function((&raw mut nested_context).cast(), fallback_value.pointer().cast()) }
        }
        Some(_) => unreachable!("anchor() fallback must be a length, percentage, calculation or nested anchor()"),
    }
}

type AutoPx = Option<CssPixels>;

fn resolve_or_auto(value: InsetValue, basis: CssPixels) -> AutoPx {
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

impl AbsposEngine {
    fn static_offset(&self, node: Node, rect: StaticPositionRect) -> LogicalOffset {
        let used = self.used(node);
        let collapsed = used.uses_collapsing_borders_model.get();
        aligned_static_offset(
            rect,
            used.margin_box_inline_size(collapsed),
            used.margin_box_block_size(collapsed),
        )
    }

    #[expect(clippy::too_many_arguments)]
    fn solve_non_replaced_inline_once(
        &self,
        node: Node,
        containing_block_inline_size: CssPixels,
        _available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: StaticPositionRect,
        input_inline_size: AutoPx,
        resolved_anchor_insets: Option<&ResolvedAnchorInsets>,
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
        static_position_rect: StaticPositionRect,
        resolved_anchor_insets: Option<&ResolvedAnchorInsets>,
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
                crate::layout::TableWrapperInlineSizeMode::ClampToAvailableInlineSize,
            ))
        } else if style.width().is_auto() {
            None
        } else {
            Some(sizing.calculate_inner_inline_size(node, available_space.inline_size, style.width(), constraints))
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

        if !sizing.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            let max_inline_size =
                sizing.calculate_inner_inline_size(node, available_space.inline_size, style.max_width(), constraints);
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
        if !style.min_width().is_auto() {
            let min_inline_size =
                sizing.calculate_inner_inline_size(node, available_space.inline_size, style.min_width(), constraints);
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
        static_position_rect: StaticPositionRect,
        resolved_anchor_insets: Option<&ResolvedAnchorInsets>,
    ) {
        let sizing = self.sizing();
        let inline_size = sizing.compute_inline_size_for_replaced_element(node, available_space, constraints);
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
        static_position_rect: StaticPositionRect,
        resolved_anchor_insets: Option<&ResolvedAnchorInsets>,
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

impl AbsposEngine {
    fn apply_min_max_block_size_constraints(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        unconstrained: AutoPx,
    ) -> AutoPx {
        let style = self.style(node);
        let sizing = self.sizing();
        let mut constrained = unconstrained;
        if !style.max_height().is_none() {
            let maximum = sizing.calculate_inner_block_size(node, available_space, style.max_height(), constraints);
            if maximum < auto_px_value(constrained) {
                constrained = Some(maximum);
            }
        }
        if !style.min_height().is_auto() {
            let minimum = sizing.calculate_inner_block_size(node, available_space, style.min_height(), constraints);
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
        static_position_rect: StaticPositionRect,
        pass: BlockSizePass,
        block_size: AutoPx,
        resolved_anchor_insets: Option<&ResolvedAnchorInsets>,
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
            let constrained =
                self.apply_min_max_block_size_constraints(node, available_space, constraints, solution.block_size);
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
        static_position_rect: StaticPositionRect,
        pass: BlockSizePass,
        resolved_anchor_insets: Option<&ResolvedAnchorInsets>,
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
            Some(
                self.sizing()
                    .calculate_inner_block_size(node, intrinsic_available_space, style.height(), constraints),
            )
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

        if solution.block_size.is_some() && !style.max_height().is_none() {
            let max_block_size = self.sizing().calculate_inner_block_size(
                node,
                intrinsic_available_space,
                style.max_height(),
                constraints,
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
        if solution.block_size.is_some() && !style.min_height().is_auto() {
            let min_block_size = self.sizing().calculate_inner_block_size(
                node,
                intrinsic_available_space,
                style.min_height(),
                constraints,
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
            solution.block_size =
                self.apply_min_max_block_size_constraints(node, available_space, constraints, solution.block_size);
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
        static_position_rect: StaticPositionRect,
        pass: BlockSizePass,
        resolved_anchor_insets: Option<&ResolvedAnchorInsets>,
    ) {
        let block_size = self
            .sizing()
            .compute_block_size_for_replaced_element(node, available_space, constraints);
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
        static_position_rect: StaticPositionRect,
        pass: BlockSizePass,
        resolved_anchor_insets: Option<&ResolvedAnchorInsets>,
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

impl AbsposEngine {
    // Run-prelude sizing for an absolutely positioned root: box-model
    // metrics, the inset-aware inline solve, the pre-inside-layout block
    // pass, and the definiteness overrides insets and aspect ratios provide.
    pub(crate) fn dimension_out_of_flow_root(&self, node: Node, inputs: AbsposLayoutInputs) {
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

        self.compute_inline_size(node, available_space, constraints, inputs.static_position_rect, resolved);
        self.compute_block_size(
            node,
            available_space,
            constraints,
            inputs.static_position_rect,
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

        self.sizing()
            .make_button_content_box_definite(node, LayoutMode::Normal, available_space, constraints, None);
    }

    pub(crate) fn finalize_out_of_flow_root_after_inside_layout(
        &self,
        node: Node,
        inputs: AbsposLayoutInputs,
        automatic_content_block_size_of_inside_layout: Option<CssPixels>,
    ) {
        let (available_space, constraints) = out_of_flow_root_space(inputs);
        let containing_block_size = LogicalSize {
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
                inputs.static_position_rect,
                BlockSizePass::AfterInsideLayout {
                    automatic_content_block_size_of_inside_layout,
                },
                resolved,
            );
        }

        {
            let used = self.used(node);
            let collapsed = used.uses_collapsing_borders_model.get();
            if let Some(inline_alignment) = inputs.containing_block_info.inline_alignment
                && style.inset_left().is_auto()
                && style.inset_right().is_auto()
            {
                let available = containing_block_size.inline_size - used.margin_box_inline_size(collapsed);
                match inline_alignment {
                    AbsposAlignment::Center => {
                        used.inset_left.set(available / 2);
                        used.inset_right.set(available / 2);
                    }
                    AbsposAlignment::Start => {
                        used.inset_right.set(available);
                    }
                    AbsposAlignment::End => {
                        used.inset_left.set(available);
                    }
                    _ => {}
                }
            }
            if let Some(block_alignment) = inputs.containing_block_info.block_alignment
                && style.inset_top().is_auto()
                && style.inset_bottom().is_auto()
            {
                let available = containing_block_size.block_size - used.margin_box_block_size(collapsed);
                match block_alignment {
                    AbsposAlignment::Center => {
                        used.inset_top.set(available / 2);
                        used.inset_bottom.set(available / 2);
                    }
                    AbsposAlignment::Start | AbsposAlignment::SelfStart => {
                        used.inset_bottom.set(available);
                    }
                    AbsposAlignment::End | AbsposAlignment::SelfEnd => {
                        used.inset_top.set(available);
                    }
                    _ => {}
                }
            }
        }
    }

    fn layout_element(&self, run: &crate::layout::FormattingContextRun, node: Node, inputs: AbsposLayoutInputs) {
        assert!(!self.facts(node).is_svg_box());
        let (available_space, constraints) = out_of_flow_root_space(inputs);

        match crate::layout::layout_inside_child(
            run,
            None,
            None,
            node,
            LayoutMode::Normal,
            LayoutInput::new(available_space, constraints, ParticipationInParentFormattingContext::AbsolutelyPositioned(inputs)),
            false,
        ) {
            crate::layout::ChildLayoutOutcome::Created(_) | crate::layout::ChildLayoutOutcome::Skipped => {}
            crate::layout::ChildLayoutOutcome::ReenterCurrent => {
                unreachable!("abspos child with contents did not establish a formatting context")
            }
        }

        let static_offset = self.static_offset(node, inputs.static_position_rect);
        let used = self.used(node);
        let collapsed = used.uses_collapsing_borders_model.get();
        let mut used_offset = LogicalOffset {
            inline_offset: if inputs.containing_block_info.inline_axis_mode == AbsposAxisMode::StaticPosition {
                static_offset.inline_offset
            } else {
                inputs.containing_block_info.rect.offset.inline_offset + used.inset_left.get()
            },
            block_offset: if inputs.containing_block_info.block_axis_mode == AbsposAxisMode::StaticPosition {
                static_offset.block_offset
            } else {
                inputs.containing_block_info.rect.offset.block_offset + used.inset_top.get()
            },
        };
        used_offset.inline_offset += used.margin_left.get() + used.border_box_left(collapsed);
        used_offset.block_offset += used.margin_top.get() + used.border_box_top(collapsed);
        let is_measurement = self.purpose.is_measurement();
        if !is_measurement {
            self.used(node)
                .rare_data_mut()
                .abspos_layout_inputs = Some(inputs);
        }
        crate::layout::place_child(
            run,
            node,
            FfiCssPixelPoint {
                x: used_offset.inline_offset,
                y: used_offset.block_offset,
            },
            None,
        );
    }

    pub(crate) fn layout_pending_child(&self, run: &crate::layout::FormattingContextRun, mut child: PendingAbsposChild) {
        debug_assert!(!self.purpose.is_measurement());
        let child_box = child.child_box;
        self.records
            .create_used_values(&self.callbacks, child_box, ContainingBlockConstraints::default());
        let containing_block_geometry = self.containing_block_geometry_for_pending_child(&child);
        let resolved =
            self.resolve_anchor_insets(child_box, Some(&containing_block_geometry), child.coordinate_space_box);
        let inline_containing_block_rect =
            self.inline_containing_block_rect_for_pending_child(&child, &containing_block_geometry);
        let translation_into_containing_block_space =
            point_sub(FfiCssPixelPoint::default(), containing_block_geometry.content_origin_in_entry_space);
        child.static_position_rect =
            crate::layout::translate_static_position_rect(child.static_position_rect, translation_into_containing_block_space);
        let inputs = AbsposLayoutInputs {
            static_position_rect: child.static_position_rect,
            containing_block_info: child
                .containing_block_info_override
                .or_else(|| {
                    self.fragments
                        .as_deref()
                        .and_then(|fragments| fragments.find_abspos_containing_block_info(child_box))
                })
                .unwrap_or_else(|| {
                    self.base_containing_block_info(
                        child_box,
                        inline_containing_block_rect,
                        &containing_block_geometry,
                        resolved.as_ref(),
                    )
                }),
            resolved_anchor_insets: resolved,
        };
        self.layout_element(run, child_box, inputs);
    }

    /// Joins the entry against the inline containing block rect its IFC
    /// contributed, folding the payload's resting space into the containing
    /// block's content-box space (no padding term, unlike anchor rects).
    fn inline_containing_block_rect_for_pending_child(
        &self,
        child: &PendingAbsposChild,
        containing_block_geometry: &ContainingBlockGeometry,
    ) -> Option<PhysicalRect> {
        if child.inline_containing_block.is_invalid() {
            return None;
        }
        let fragments = self.fragments.as_deref()?;
        let (rect, payload_space) = fragments.find_inline_containing_block_rect(child.inline_containing_block)?;
        let fold_into_entry_space =
            self.translation_between_payload_resting_spaces(payload_space, child.coordinate_space_box);
        Some(PhysicalRect {
            x: rect.x + fold_into_entry_space.x - containing_block_geometry.content_origin_in_entry_space.x,
            y: rect.y + fold_into_entry_space.y - containing_block_geometry.content_origin_in_entry_space.y,
            width: rect.width,
            height: rect.height,
        })
    }

    fn replay(&self, run: &crate::layout::FormattingContextRun, node: Node) {
        let saved_inputs = self.callbacks.saved_abspos_layout_inputs(node);
        let found = saved_inputs.is_some();
        assert!(found);
        let mut inputs = saved_inputs.unwrap();
        if !inputs.containing_block_info.derives_from_own_computed_values {
            let (inline, block) =
                axis_modes(self.style(node).with_resolved_insets(inputs.resolved_anchor_insets.as_ref()));
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
        containing_block_size: LogicalSize,
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
            self.resolve_anchor_insets(node, None, NodeSlotId::INVALID)
        } else {
            None
        };
        let style = initial_style.with_resolved_insets(resolved.as_ref());
        if style.position() != positioning::RELATIVE {
            return;
        }

        let resolve_opposing = |first: InsetValue, second: InsetValue, basis: CssPixels| {
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
            && !crate::layout::resolve_block_axis_percentage_inset_basis_is_definite(
                &self.records,
                &self.callbacks,
                self.callbacks.containing_block(node),
                formatting_context_root,
                treat_block_axis_percentage_insets_as_auto_beyond_root,
            );
        fn block_axis_inset_value(value: InsetValue<'_>, treat_percentage_as_auto: bool) -> InsetValue<'_> {
            if treat_percentage_as_auto && value.contains_percentage() {
                InsetValue::auto_value()
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
    records: &std::rc::Rc<RunRecords>,
    callbacks: FfiLayoutFcCallbacks,
    should_collect_devtools_layout_data: bool,
    entry_fragments: &std::rc::Rc<crate::layout::RunFragmentBuilder>,
) {
    let accumulator_root = entry_fragments.root_node();
    let run = crate::layout::FormattingContextRun {
        purpose: LayoutPurpose::Commit,
        records: records.clone(),
        box_: accumulator_root,
        layout_mode: LayoutMode::Normal,
        callbacks,
        should_collect_devtools_layout_data,
        treat_block_axis_percentage_insets_as_auto_beyond_root: false,
        fragments: Some(entry_fragments.clone()),
        previous_line_data: None,
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
    run: &crate::layout::FormattingContextRun,
    node: Node,
    inline_size: CssPixels,
    block_size: CssPixels,
) {
    AbsposEngine::for_run(run).compute_inset(
        node,
        LogicalSize {
            inline_size,
            block_size,
        },
        run.box_,
        run.treat_block_axis_percentage_insets_as_auto_beyond_root,
    );
}
