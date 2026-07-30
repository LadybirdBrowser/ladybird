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

pub(crate) struct AbsposEngine<'pass> {
    state: &'pass LayoutState,
    callbacks: FfiLayoutFcCallbacks,
}

impl<'pass> AbsposEngine<'pass> {
    fn new(state: &'pass LayoutState, callbacks: FfiLayoutFcCallbacks) -> Self {
        Self { state, callbacks }
    }

    fn sizing(&self) -> SizingContext<'_> {
        SizingContext::new(self.state, self.callbacks)
    }

    fn style(&self, node: Node) -> StyleValues<'pass> {
        self.state.style_facts(&self.callbacks, node)
    }

    fn facts(&self, node: Node) -> NodeFacts<'_> {
        self.state.node_facts(&self.callbacks, node)
    }

    fn used_pointer(&self, node: Node) -> &'pass UsedValues {
        self.state.used_values(&self.callbacks, node)
    }

    fn try_used_pointer(&self, node: Node) -> Option<&'pass UsedValues> {
        self.state.try_used_values(&self.callbacks, node)
    }

    fn used(&self, node: Node) -> &'pass UsedValues {
        self.used_pointer(node)
    }

    fn used_mut(&self, node: Node) -> &'pass UsedValues {
        self.used_pointer(node)
    }

    fn static_position_containing_block(&self, node: Node) -> Node {
        self.callbacks.static_position_containing_block(node)
    }

    fn inline_containing_block(&self, node: Node) -> Node {
        self.callbacks.inline_containing_block(node)
    }

    fn non_anonymous_containing_block(&self, node: Node) -> Node {
        self.callbacks.non_anonymous_containing_block(node)
    }

    fn node_is_ancestor(&self, ancestor: Node, node: Node) -> bool {
        self.callbacks.is_ancestor(ancestor, node)
    }

    fn belongs_to_inline_containing_block(&self, inline_node: Node, node: Node) -> bool {
        !self.facts(node).is_anonymous() && self.node_is_ancestor(inline_node, node)
    }

    fn resolve_static_position_relative_to_containing_block(
        &self,
        node: Node,
        static_position_rect: StaticPositionRect,
    ) -> StaticPositionRect {
        let static_position_cb = self.static_position_containing_block(node);
        let actual_containing_block = self.callbacks.containing_block(node);
        if static_position_cb.is_invalid() || static_position_cb == actual_containing_block {
            return static_position_rect;
        }

        let mut merge_point = static_position_cb;
        while merge_point != actual_containing_block && !self.node_is_ancestor(merge_point, actual_containing_block) {
            merge_point = self.callbacks.containing_block(merge_point);
            assert!(!merge_point.is_invalid());
        }

        let offset_relative_to_merge_point = |descendant: Node| {
            let mut offset = FfiCssPixelPoint::default();
            let mut current = descendant;
            while current != merge_point {
                let used = self.used(current);
                offset = point_add(offset, used.content_offset.get());
                current = self.callbacks.containing_block(current);
                assert!(!current.is_invalid());
            }
            offset
        };
        translate_static_position_between_chains(
            static_position_rect,
            offset_relative_to_merge_point(static_position_cb),
            offset_relative_to_merge_point(actual_containing_block),
        )
    }

    fn line_fragments(&self, node: Node) -> Vec<LineFragmentFacts> {
        let mut fragments = Vec::new();
        let Some(lines) = self.state.line_data(self.callbacks.slot_index(node)) else {
            return fragments;
        };
        for line in &lines.line_boxes {
            for fragment in &line.fragments {
                let (x, y) = fragment.offset();
                let (width, height) = fragment.size();
                fragments.push(LineFragmentFacts {
                    layout_node: fragment.layout_node,
                    is_atomic_inline: fragment.is_atomic_inline,
                    writing_mode: fragment.writing_mode,
                    style_block_axis_is_reverse: fragment.style_block_axis_is_reverse,
                    inline_offset: fragment.inline_offset,
                    block_offset: fragment.block_offset,
                    offset: FfiCssPixelPoint { x, y },
                    size: FfiCssPixelPoint { x: width, y: height },
                });
            }
        }
        fragments
    }

    fn add_atomic_inline_fragment_rect(
        &self,
        inline_node: Node,
        fragment: LineFragmentFacts,
        offset: FfiCssPixelPoint,
        bounding_rect: &mut Option<PhysicalRect>,
        empty_bounding_rect: &mut Option<PhysicalRect>,
    ) {
        let Some(child_used) = self.try_used_pointer(fragment.layout_node) else {
            return;
        };
        let collapsed = child_used.uses_collapsing_borders_model.get();
        let is_horizontal = fragment.writing_mode == writing_mode::HORIZONTAL_TB;
        let inline_axis_border_box_start = fragment.inline_offset
            - if is_horizontal {
                child_used.border_box_left(collapsed)
            } else {
                child_used.border_box_top(collapsed)
            };
        let inline_axis_border_box_extent = if is_horizontal {
            child_used.border_box_inline_size(collapsed)
        } else {
            child_used.border_box_block_size(collapsed)
        };
        let block_axis_line_height = self.style(inline_node).line_height();
        let block_axis_start = if fragment.style_block_axis_is_reverse {
            fragment.block_offset + child_used.border_box_right(collapsed) - block_axis_line_height
        } else {
            fragment.block_offset
                - if is_horizontal {
                    child_used.border_box_top(collapsed)
                } else {
                    child_used.border_box_left(collapsed)
                }
        };
        let rect = if is_horizontal {
            PhysicalRect {
                x: inline_axis_border_box_start,
                y: block_axis_start,
                width: inline_axis_border_box_extent,
                height: block_axis_line_height,
            }
        } else {
            PhysicalRect {
                x: block_axis_start,
                y: inline_axis_border_box_start,
                width: block_axis_line_height,
                height: inline_axis_border_box_extent,
            }
        }
        .translated(offset);
        add_fragment_rect(rect, bounding_rect, empty_bounding_rect);
    }

    fn walk_inline_containing_block(
        &self,
        inline_node: Node,
        node: Node,
        offset: FfiCssPixelPoint,
        bounding_rect: &mut Option<PhysicalRect>,
        empty_bounding_rect: &mut Option<PhysicalRect>,
    ) {
        for fragment in self.line_fragments(node) {
            if !self.belongs_to_inline_containing_block(inline_node, fragment.layout_node) {
                continue;
            }
            if fragment.is_atomic_inline {
                self.add_atomic_inline_fragment_rect(inline_node, fragment, offset, bounding_rect, empty_bounding_rect);
                continue;
            }
            add_fragment_rect(
                PhysicalRect {
                    x: fragment.offset.x + offset.x,
                    y: fragment.offset.y + offset.y,
                    width: fragment.size.x,
                    height: fragment.size.y,
                },
                bounding_rect,
                empty_bounding_rect,
            );
        }

        let mut child = self.callbacks.first_child(node);
        while !child.is_invalid() {
            let next = self.callbacks.next_sibling(child);
            let facts = self.facts(child);
            if facts.is_absolutely_positioned() || facts.is_floating() {
                child = next;
                continue;
            }
            let child_used_pointer = self.try_used_pointer(child);
            let child_offset = if let Some(child_used) = child_used_pointer {
                point_add(offset, child_used.content_offset.get())
            } else {
                offset
            };
            if facts.is_box() && !facts.is_anonymous() {
                if !self.belongs_to_inline_containing_block(inline_node, child) {
                    child = next;
                    continue;
                }
                if facts.is_atomic_inline() {
                    child = next;
                    continue;
                }
                if let Some(child_used) = child_used_pointer {
                    let collapsed = child_used.uses_collapsing_borders_model.get();
                    let border_box_origin = FfiCssPixelPoint {
                        x: child_offset.x - child_used.border_left_collapsed(collapsed) - child_used.padding_left.get(),
                        y: child_offset.y - child_used.border_top_collapsed(collapsed) - child_used.padding_top.get(),
                    };
                    add_fragment_rect(
                        PhysicalRect {
                            x: border_box_origin.x,
                            y: border_box_origin.y,
                            width: child_used.border_box_inline_size(collapsed),
                            height: child_used.border_box_block_size(collapsed),
                        },
                        bounding_rect,
                        empty_bounding_rect,
                    );
                }
            }
            self.walk_inline_containing_block(inline_node, child, child_offset, bounding_rect, empty_bounding_rect);
            child = next;
        }
    }

    fn compute_inline_containing_block_rect(
        &self,
        inline_node: Node,
        abspos_containing_block: Node,
    ) -> Option<PhysicalRect> {
        if self.facts(inline_node).is_anonymous() {
            return None;
        }
        let outer_block = self.non_anonymous_containing_block(inline_node);
        if outer_block.is_invalid() {
            return None;
        }

        let mut outer_offset = FfiCssPixelPoint::default();
        let mut ancestor = outer_block;
        while !ancestor.is_invalid() && ancestor != abspos_containing_block {
            let used = self.try_used_pointer(ancestor);
            if let Some(used) = used {
                outer_offset = point_add(outer_offset, used.content_offset.get());
            }
            ancestor = self.callbacks.parent(ancestor);
        }

        let mut bounding_rect = None;
        let mut empty_bounding_rect = None;
        self.walk_inline_containing_block(
            inline_node,
            outer_block,
            outer_offset,
            &mut bounding_rect,
            &mut empty_bounding_rect,
        );
        let mut rect = bounding_rect.or(empty_bounding_rect)?;
        if let Some(inline_used) = self.try_used_pointer(inline_node) {
            rect.x -= inline_used.padding_left.get();
            rect.y -= inline_used.padding_top.get();
            rect.width += inline_used.padding_left.get() + inline_used.padding_right.get();
            rect.height += inline_used.padding_top.get() + inline_used.padding_bottom.get();
        }
        Some(rect)
    }

    fn base_containing_block_info(&self, node: Node) -> AbsposContainingBlockInfo {
        let style = self.style(node);
        let (inline_axis_mode, block_axis_mode) = axis_modes(style);
        let containing_block = self.callbacks.containing_block(node);
        assert!(!containing_block.is_invalid());
        let inline_containing_block = self.inline_containing_block(node);
        if !inline_containing_block.is_invalid()
            && let Some(rect) = self.compute_inline_containing_block_rect(inline_containing_block, containing_block)
        {
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

        let containing_block_used = self.used(containing_block);
        AbsposContainingBlockInfo {
            rect: LogicalRect {
                offset: LogicalOffset {
                    inline_offset: -containing_block_used.padding_left.get(),
                    block_offset: -containing_block_used.padding_top.get(),
                },
                size: LogicalSize {
                    inline_size: containing_block_used.content_inline_size.get()
                        + containing_block_used.padding_left.get()
                        + containing_block_used.padding_right.get(),
                    block_size: containing_block_used.content_block_size.get()
                        + containing_block_used.padding_top.get()
                        + containing_block_used.padding_bottom.get(),
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

fn add_fragment_rect(
    rect: PhysicalRect,
    bounding_rect: &mut Option<PhysicalRect>,
    empty_bounding_rect: &mut Option<PhysicalRect>,
) {
    let destination = if rect.is_empty() {
        empty_bounding_rect
    } else {
        bounding_rect
    };
    *destination = Some(destination.map_or(rect, |existing| existing.union(rect)));
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
    is_from_end: bool,
    is_horizontal_axis: bool,
    containing_block_extent: CssPixels,
    resolution_state: *mut AnchorResolutionState,
}

impl AbsposEngine<'_> {
    fn anchor_lookup(&self, positioned_box: Node, anchor_name: usize) -> Option<Node> {
        let eligible_anchor_boxes = self.state.used_value_nodes();
        let eligible_anchor_shells = eligible_anchor_boxes
            .iter()
            .map(|&node| self.callbacks.shell(node))
            .collect::<Vec<_>>();
        // SAFETY: The name handle is retained by either the style snapshot or
        // the live anchor() shell. The eligible-node slice is borrowed only
        // for this synchronous lookup.
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

    fn anchor_rect(&self, anchor_box: Node, containing_block: Node) -> PhysicalRect {
        let anchor_state = self.used(anchor_box);
        let mut anchor_offset = FfiCssPixelPoint::default();
        let mut node = anchor_box;
        while node != containing_block {
            assert!(!node.is_invalid());
            anchor_offset = point_add(anchor_offset, self.used(node).content_offset.get());
            node = self.callbacks.containing_block(node);
        }
        anchor_rect_from_geometry(anchor_state, self.used(containing_block), anchor_offset)
    }

    fn anchor_side(
        &self,
        facts: FfiAnchorFunctionFacts,
        rect: PhysicalRect,
        positioned_box: Node,
        containing_block: Node,
        is_from_end: bool,
        is_horizontal_axis: bool,
    ) -> Option<CssPixels> {
        let containing_block_direction = self.style(containing_block).direction();
        let box_direction = self.style(positioned_box).direction();
        match facts.side_kind {
            FfiAnchorSideKind::Invalid => None,
            FfiAnchorSideKind::Top => (!is_horizontal_axis).then_some(rect.top()),
            FfiAnchorSideKind::Bottom => (!is_horizontal_axis).then_some(rect.bottom()),
            FfiAnchorSideKind::Left => is_horizontal_axis.then_some(rect.left()),
            FfiAnchorSideKind::Right => is_horizontal_axis.then_some(rect.right()),
            FfiAnchorSideKind::Center => Some(if is_horizontal_axis {
                rect.left() + rect.width / 2
            } else {
                rect.top() + rect.height / 2
            }),
            FfiAnchorSideKind::Start | FfiAnchorSideKind::End => {
                let is_start = facts.side_kind == FfiAnchorSideKind::Start;
                if is_horizontal_axis {
                    let use_left = (containing_block_direction == direction::LTR) == is_start;
                    Some(if use_left { rect.left() } else { rect.right() })
                } else {
                    Some(if is_start { rect.top() } else { rect.bottom() })
                }
            }
            FfiAnchorSideKind::SelfStart | FfiAnchorSideKind::SelfEnd => {
                let is_start = facts.side_kind == FfiAnchorSideKind::SelfStart;
                if is_horizontal_axis {
                    let use_left = (box_direction == direction::LTR) == is_start;
                    Some(if use_left { rect.left() } else { rect.right() })
                } else {
                    Some(if is_start { rect.top() } else { rect.bottom() })
                }
            }
            FfiAnchorSideKind::Inside | FfiAnchorSideKind::Outside => {
                let same_side = facts.side_kind == FfiAnchorSideKind::Inside;
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
            FfiAnchorSideKind::Percentage => {
                if is_horizontal_axis {
                    let (start, end) = if containing_block_direction == direction::LTR {
                        (rect.left(), rect.right())
                    } else {
                        (rect.right(), rect.left())
                    };
                    Some(start + CssPixels::nearest_value_for((end - start).to_double() * facts.side_percentage))
                } else {
                    Some(rect.top() + CssPixels::nearest_value_for(rect.height.to_double() * facts.side_percentage))
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

    fn resolve_anchor_value(
        &self,
        value: FfiSizeValue,
        positioned_box: Node,
        containing_block: Node,
        axis: AnchorValueAxis,
        resolution_state: &mut AnchorResolutionState,
    ) -> Option<CssPixels> {
        assert!(value.contains_anchor_function);
        assert!(!value.calc.is_null());
        let mut callback_context = AnchorCalcCallbackContext {
            engine: self,
            positioned_box,
            containing_block,
            is_from_end: axis.is_from_end,
            is_horizontal_axis: axis.is_horizontal,
            containing_block_extent: axis.containing_block_extent,
            resolution_state,
        };
        // SAFETY: The calculated handle is retained by the style cache and
        // all callback state remains live for this synchronous resolution.
        let result = unsafe {
            resolve_calc_with_external_resolutions(
                value.calc,
                axis.containing_block_extent,
                (&raw mut callback_context).cast(),
                Some(resolve_anchor_non_math_function),
            )
        };
        result.resolved.then(|| CssPixels::nearest_value_for(result.value))
    }

    fn resolve_anchor_insets(&self, node: Node) {
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
        let top_contains_anchor = style.inset_top().contains_anchor_function;
        let right_contains_anchor = style.inset_right().contains_anchor_function;
        let bottom_contains_anchor = style.inset_bottom().contains_anchor_function;
        let left_contains_anchor = style.inset_left().contains_anchor_function;
        if !top_contains_anchor && !right_contains_anchor && !bottom_contains_anchor && !left_contains_anchor {
            return;
        }

        let containing_block = self.callbacks.containing_block(node);
        if containing_block.is_invalid() {
            return;
        }
        let containing_block_state = self.used(containing_block);
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
        let mut resolved = FfiResolvedAnchorInsets::default();

        if top_contains_anchor {
            let value = self.resolve_anchor_value(
                style.inset_top(),
                node,
                containing_block,
                AnchorValueAxis {
                    is_from_end: false,
                    is_horizontal: false,
                    containing_block_extent: containing_block_state.content_block_size.get()
                        + containing_block_state.padding_top.get()
                        + containing_block_state.padding_bottom.get(),
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
                AnchorValueAxis {
                    is_from_end: true,
                    is_horizontal: true,
                    containing_block_extent: containing_block_state.content_inline_size.get()
                        + containing_block_state.padding_left.get()
                        + containing_block_state.padding_right.get(),
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
                AnchorValueAxis {
                    is_from_end: true,
                    is_horizontal: false,
                    containing_block_extent: containing_block_state.content_block_size.get()
                        + containing_block_state.padding_top.get()
                        + containing_block_state.padding_bottom.get(),
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
                AnchorValueAxis {
                    is_from_end: false,
                    is_horizontal: true,
                    containing_block_extent: containing_block_state.content_inline_size.get()
                        + containing_block_state.padding_left.get()
                        + containing_block_state.padding_right.get(),
                },
                &mut resolution_state,
            );
            resolved.resolves_left = true;
            resolved.left_is_auto = value.is_none();
            resolved.left = value.unwrap_or_default();
        }

        // The computed-values writeback waits until the pass has committed;
        // the pass itself reads the resolved insets from the replaced cache
        // entries below.
        self.state.defer_resolved_anchor_insets(&self.callbacks, node, resolved);
        self.state
            .replace_resolved_anchor_insets(&self.callbacks, node, resolved);

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
    }
}

unsafe extern "C" fn resolve_anchor_non_math_function(context: *mut c_void, shell: *const c_void) -> *const c_void {
    // SAFETY: The CSS calc engine calls this only during resolve_anchor_value,
    // whose stack owns this callback context.
    let context = unsafe { &mut *context.cast::<AnchorCalcCallbackContext<'_>>() };
    // SAFETY: The engine pointer is live for the enclosing resolution.
    let engine = unsafe { &*context.engine };
    // SAFETY: `shell` is the live Rust style-value handle supplied by the
    // CSS calc core.
    let facts = unsafe { (engine.callbacks.build_anchor_function_facts)(engine.callbacks.context, shell) };
    let style = engine.style(context.positioned_box);
    let anchor_name = if facts.has_anchor_name {
        Some(facts.anchor_name)
    } else if style.has_position_anchor() {
        Some(style.position_anchor_name())
    } else {
        None
    };
    let mut resolved_node = std::ptr::null();
    if engine.facts(context.positioned_box).is_absolutely_positioned()
        && let Some(anchor_name) = anchor_name
        && let Some(anchor_box) = engine.anchor_lookup(context.positioned_box, anchor_name)
    {
        let rect = engine.anchor_rect(anchor_box, context.containing_block);
        if let Some(side) = engine.anchor_side(
            facts,
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
            resolved_node = calc_node_create_px_dimension(inset.to_double());
        }
    }
    if facts.has_anchor_name {
        // SAFETY: The C++ facts callback transferred one raw fly-string
        // reference for this explicit anchor name.
        unsafe {
            (engine.callbacks.release_anchor_name_handle)(facts.anchor_name);
        }
    }
    if !resolved_node.is_null() {
        return resolved_node;
    }

    // SAFETY: The callback borrows fallback data from the live anchor style
    // value for this synchronous resolution.
    let fallback = unsafe { (engine.callbacks.anchor_function_fallback)(engine.callbacks.context, shell) };
    match fallback.kind {
        FfiAnchorFallbackKind::None => std::ptr::null(),
        FfiAnchorFallbackKind::Px => calc_node_create_px_dimension(fallback.px.to_double()),
        FfiAnchorFallbackKind::Percentage => {
            calc_node_create_px_dimension(context.containing_block_extent.to_double() * fallback.fraction)
        }
        FfiAnchorFallbackKind::Calculated => {
            assert!(!fallback.value.is_null());
            let mut nested_context = *context;
            let resolved = unsafe {
                resolve_calc_with_external_resolutions(
                    fallback.value,
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
        FfiAnchorFallbackKind::Anchor => {
            assert!(!fallback.value.is_null());
            let mut nested_context = *context;
            unsafe { resolve_anchor_non_math_function((&raw mut nested_context).cast(), fallback.value) }
        }
    }
}

type AutoPx = Option<CssPixels>;

fn resolve_or_auto(value: FfiSizeValue, basis: CssPixels) -> AutoPx {
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
    fn static_offset(&self, node: Node, rect: StaticPositionRect) -> LogicalOffset {
        let used = self.used(node);
        let collapsed = used.uses_collapsing_borders_model.get();
        aligned_static_offset(
            rect,
            used.margin_box_inline_size(collapsed),
            used.margin_box_block_size(collapsed),
        )
    }

    fn solve_non_replaced_inline_once(
        &self,
        node: Node,
        containing_block_inline_size: CssPixels,
        _available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: StaticPositionRect,
        input_inline_size: AutoPx,
    ) -> (AutoPx, CssPixels, CssPixels, AutoPx, AutoPx) {
        let style = self.style(node);
        let used = self.used(node);
        let border_left = style.border_left_width();
        let border_right = style.border_right_width();
        let padding_left = used.padding_left.get();
        let padding_right = used.padding_right.get();
        let computed_left = style.inset_left();
        let computed_right = style.inset_right();
        let mut left = style.inset_left().to_px(containing_block_inline_size);
        let mut right = style.inset_right().to_px(containing_block_inline_size);
        let mut margin_left = resolve_or_auto(style.margin_left(), containing_block_inline_size);
        let mut margin_right = resolve_or_auto(style.margin_right(), containing_block_inline_size);
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
            self.used_mut(node).set_content_inline_size(content_inline_size);
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
                );
            }
        }

        let used = self.used_mut(node);
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
    ) {
        let sizing = self.sizing();
        let inline_size = sizing.compute_inline_size_for_replaced_element(node, available_space, constraints);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let style = self.style(node);
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
            resolve_or_auto(style.margin_left(), containing_block_inline_size),
            resolve_or_auto(style.margin_right(), containing_block_inline_size),
            self.static_offset(node, static_position_rect).inline_offset,
            ReplacedAxisBehavior {
                clear_auto_margins_if_start_is_auto: true,
                clear_negative_auto_margins: true,
            },
        );

        let used = self.used_mut(node);
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
    ) {
        if self
            .sizing()
            .box_is_sized_as_replaced_element(node, available_space, constraints)
        {
            self.compute_inline_size_for_replaced(node, available_space, constraints, static_position_rect);
        } else {
            self.compute_inline_size_for_non_replaced(node, available_space, constraints, static_position_rect);
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum BlockSizePass {
    BeforeInsideLayout,
    AfterInsideLayout,
}

impl AbsposEngine<'_> {
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
            if pass == BlockSizePass::BeforeInsideLayout {
                return None;
            }
            return Some(automatic_block_size_for_bfc_root(self.state, self.callbacks, node));
        }
        let inner = self
            .used(node)
            .available_inner_space_or_constraints_from(available_space);
        Some(
            self.sizing()
                .calculate_fit_content_size(node, SizingAxis::Block, inner, constraints),
        )
    }

    fn solve_non_replaced_block_once(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: StaticPositionRect,
        pass: BlockSizePass,
        mut block_size: AutoPx,
    ) -> (AutoPx, AutoPx, AutoPx, AutoPx, AutoPx) {
        let style = self.style(node);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let containing_block_block_size = available_space.block_size.to_px_or_zero();
        let mut margin_top = resolve_or_auto(style.margin_top(), containing_block_inline_size);
        let mut margin_bottom = resolve_or_auto(style.margin_bottom(), containing_block_inline_size);
        let mut top = resolve_or_auto(style.inset_top(), containing_block_block_size);
        let mut bottom = resolve_or_auto(style.inset_bottom(), containing_block_block_size);
        let used = self.used(node);
        let padding_top = used.padding_top.get();
        let padding_bottom = used.padding_bottom.get();

        let solve_for = |length: AutoPx,
                         clamp_to_zero: bool,
                         top: AutoPx,
                         margin_top: AutoPx,
                         block_size: AutoPx,
                         margin_bottom: AutoPx,
                         bottom: AutoPx| {
            solve_abspos_axis_for(
                containing_block_block_size,
                length,
                clamp_to_zero,
                top,
                margin_top,
                style.border_top_width(),
                padding_top,
                block_size,
                padding_bottom,
                style.border_bottom_width(),
                margin_bottom,
                bottom,
            )
        };

        if top.is_none() && block_size.is_none() && bottom.is_none() {
            if margin_top.is_none() {
                margin_top = Some(CssPixels::default());
            }
            if margin_bottom.is_none() {
                margin_bottom = Some(CssPixels::default());
            }
            let Some(automatic) = self.automatic_block_size(node, available_space, constraints, pass) else {
                return (block_size, top, bottom, margin_top, margin_bottom);
            };
            block_size = Some(automatic);
            let constrained = self.apply_min_max_block_size_constraints(node, available_space, constraints, block_size);
            self.used_mut(node).set_content_block_size(auto_px_value(constrained));
            top = Some(self.static_offset(node, static_position_rect).block_offset);
            bottom = Some(solve_for(
                bottom,
                false,
                top,
                margin_top,
                block_size,
                margin_bottom,
                bottom,
            ));
        } else if top.is_some() && block_size.is_some() && bottom.is_some() {
            if margin_top.is_none() && margin_bottom.is_none() {
                let remainder = solve_for(
                    Some(auto_px_value(margin_top) + auto_px_value(margin_bottom)),
                    false,
                    top,
                    margin_top,
                    block_size,
                    margin_bottom,
                    bottom,
                );
                margin_top = Some(remainder / 2);
                margin_bottom = Some(remainder / 2);
            } else if margin_top.is_none() || margin_bottom.is_none() {
                if margin_top.is_none() {
                    margin_top = Some(solve_for(
                        margin_top,
                        false,
                        top,
                        margin_top,
                        block_size,
                        margin_bottom,
                        bottom,
                    ));
                } else {
                    margin_bottom = Some(solve_for(
                        margin_bottom,
                        false,
                        top,
                        margin_top,
                        block_size,
                        margin_bottom,
                        bottom,
                    ));
                }
            } else {
                bottom = Some(solve_for(
                    bottom,
                    false,
                    top,
                    margin_top,
                    block_size,
                    margin_bottom,
                    bottom,
                ));
            }
        } else {
            if margin_top.is_none() {
                margin_top = Some(CssPixels::default());
            }
            if margin_bottom.is_none() {
                margin_bottom = Some(CssPixels::default());
            }

            if top.is_none() && block_size.is_none() && bottom.is_some() {
                let Some(automatic) = self.automatic_block_size(node, available_space, constraints, pass) else {
                    return (block_size, top, bottom, margin_top, margin_bottom);
                };
                block_size = Some(automatic);
                top = Some(solve_for(
                    top,
                    false,
                    top,
                    margin_top,
                    block_size,
                    margin_bottom,
                    bottom,
                ));
            } else if top.is_none() && bottom.is_none() && block_size.is_some() {
                top = Some(self.static_offset(node, static_position_rect).block_offset);
                bottom = Some(solve_for(
                    bottom,
                    false,
                    top,
                    margin_top,
                    block_size,
                    margin_bottom,
                    bottom,
                ));
            } else if block_size.is_none() && bottom.is_none() && top.is_some() {
                let Some(automatic) = self.automatic_block_size(node, available_space, constraints, pass) else {
                    return (block_size, top, bottom, margin_top, margin_bottom);
                };
                block_size = Some(automatic);
                bottom = Some(solve_for(
                    bottom,
                    false,
                    top,
                    margin_top,
                    block_size,
                    margin_bottom,
                    bottom,
                ));
            } else if top.is_none() && block_size.is_some() && bottom.is_some() {
                top = Some(solve_for(
                    top,
                    false,
                    top,
                    margin_top,
                    block_size,
                    margin_bottom,
                    bottom,
                ));
            } else if block_size.is_none() && top.is_some() && bottom.is_some() {
                block_size = Some(solve_for(
                    block_size,
                    true,
                    top,
                    margin_top,
                    block_size,
                    margin_bottom,
                    bottom,
                ));
            } else if bottom.is_none() && top.is_some() && block_size.is_some() {
                bottom = Some(solve_for(
                    bottom,
                    false,
                    top,
                    margin_top,
                    block_size,
                    margin_bottom,
                    bottom,
                ));
            }
        }
        (block_size, top, bottom, margin_top, margin_bottom)
    }

    fn compute_block_size_for_non_replaced(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: StaticPositionRect,
        pass: BlockSizePass,
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
        let (mut used_block_size, mut top, mut bottom, mut margin_top, mut margin_bottom) =
            self.solve_non_replaced_block_once(node, available_space, constraints, static_position_rect, pass, initial);

        if used_block_size.is_some() && !style.max_height().is_none() {
            let max_block_size = self.sizing().calculate_inner_block_size(
                node,
                intrinsic_available_space,
                style.max_height(),
                constraints,
            );
            if auto_px_value(used_block_size) > max_block_size {
                (used_block_size, top, bottom, margin_top, margin_bottom) = self.solve_non_replaced_block_once(
                    node,
                    available_space,
                    constraints,
                    static_position_rect,
                    pass,
                    Some(max_block_size),
                );
            }
        }
        if used_block_size.is_some() && !style.min_height().is_auto() {
            let min_block_size = self.sizing().calculate_inner_block_size(
                node,
                intrinsic_available_space,
                style.min_height(),
                constraints,
            );
            if auto_px_value(used_block_size) < min_block_size {
                (used_block_size, top, bottom, margin_top, margin_bottom) = self.solve_non_replaced_block_once(
                    node,
                    available_space,
                    constraints,
                    static_position_rect,
                    pass,
                    Some(min_block_size),
                );
            }
        }
        if used_block_size.is_none() {
            used_block_size =
                self.apply_min_max_block_size_constraints(node, available_space, constraints, used_block_size);
        }

        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let containing_block_block_size = available_space.block_size.to_px_or_zero();
        let used = self.used_mut(node);
        used.set_content_block_size(auto_px_value(used_block_size));
        if style.height().is_auto() && pass == BlockSizePass::BeforeInsideLayout {
            return;
        }
        if !style.height().is_intrinsic_sizing_constraint() {
            used.has_definite_block_size.set(true);
        }
        used.inset_top.set(auto_px_value(top));
        used.inset_bottom.set(auto_px_value(bottom));
        // The local values are already resolved against these bases. Keep the
        // variables to document and pin the C++ basis distinction.
        let _ = (containing_block_inline_size, containing_block_block_size);
        used.margin_top.set(auto_px_value(margin_top));
        used.margin_bottom.set(auto_px_value(margin_bottom));
    }

    fn compute_block_size_for_replaced(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        static_position_rect: StaticPositionRect,
        pass: BlockSizePass,
    ) {
        let block_size = self
            .sizing()
            .compute_block_size_for_replaced_element(node, available_space, constraints);
        let containing_block_block_size = available_space.block_size.to_px_or_zero();
        let style = self.style(node);
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
            resolve_or_auto(style.margin_top(), containing_block_block_size),
            resolve_or_auto(style.margin_bottom(), containing_block_block_size),
            self.static_offset(node, static_position_rect).block_offset,
            ReplacedAxisBehavior {
                clear_auto_margins_if_start_is_auto: false,
                clear_negative_auto_margins: false,
            },
        );

        let used = self.used_mut(node);
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
    ) {
        if self
            .sizing()
            .box_is_sized_as_replaced_element(node, available_space, constraints)
        {
            self.compute_block_size_for_replaced(node, available_space, constraints, static_position_rect, pass);
        } else {
            self.compute_block_size_for_non_replaced(node, available_space, constraints, static_position_rect, pass);
        }
    }
}

impl<'pass> AbsposEngine<'pass> {
    fn layout_element(&self, frame: &mut crate::layout::FcFrame<'pass>, node: Node, inputs: AbsposLayoutInputs) {
        assert!(!self.facts(node).is_svg_box());
        let containing_block_size = LogicalSize {
            inline_size: clamp_to_max_dimension_value(inputs.containing_block_info.rect.size.inline_size),
            block_size: clamp_to_max_dimension_value(inputs.containing_block_info.rect.size.block_size),
        };
        let available_space = AvailableSpace {
            inline_size: AvailableSize::definite(containing_block_size.inline_size),
            block_size: AvailableSize::definite(containing_block_size.block_size),
        };
        let constraints = ContainingBlockConstraints {
            percentage_basis_inline_size: Some(containing_block_size.inline_size),
            percentage_basis_block_size: Some(containing_block_size.block_size),
            quirks_mode_percentage_basis_block_size: None,
        };
        let style = self.style(node);
        {
            let used = self.used_mut(node);
            used.border_left.set(style.border_left_width());
            used.border_right.set(style.border_right_width());
            used.border_top.set(style.border_top_width());
            used.border_bottom.set(style.border_bottom_width());
            used.padding_left
                .set(style.padding_left().to_px(containing_block_size.inline_size));
            used.padding_right
                .set(style.padding_right().to_px(containing_block_size.inline_size));
            used.padding_top
                .set(style.padding_top().to_px(containing_block_size.inline_size));
            used.padding_bottom
                .set(style.padding_bottom().to_px(containing_block_size.inline_size));
        }

        self.compute_inline_size(node, available_space, constraints, inputs.static_position_rect);
        self.compute_block_size(
            node,
            available_space,
            constraints,
            inputs.static_position_rect,
            BlockSizePass::BeforeInsideLayout,
        );

        {
            let used = self.used_mut(node);
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
            let used = self.used_mut(node);
            used.has_definite_inline_size.set(true);
            if (!style.height().is_auto() && !style.height().is_intrinsic_sizing_constraint())
                || block_size_resolved_from_aspect_ratio
            {
                used.has_definite_block_size.set(true);
            }
        }

        self.sizing()
            .make_button_content_box_definite(node, LayoutMode::Normal, available_space, constraints, None);

        let inner_available_space = self
            .used(node)
            .available_inner_space_or_constraints_from(available_space);
        let child_layout = match crate::layout::layout_inside_child(
            frame,
            None,
            None,
            node,
            LayoutMode::Normal,
            LayoutInput {
                available_space: inner_available_space,
                containing_block_constraints: constraints,
                content_box_position_in_bfc_root: None,
                table_grid_min_border_box_block_size: None,
            },
            false,
        ) {
            crate::layout::ChildLayoutOutcome::Created(child_layout) => Some(child_layout),
            crate::layout::ChildLayoutOutcome::Skipped => None,
            // Absolutely positioned boxes with children establish an
            // independent formatting context, so they cannot remain in
            // the currently running context.
            crate::layout::ChildLayoutOutcome::ReenterCurrent => {
                unreachable!("abspos child with contents did not establish a formatting context")
            }
        };

        if style.height().is_auto() {
            self.compute_block_size(
                node,
                available_space,
                constraints,
                inputs.static_position_rect,
                BlockSizePass::AfterInsideLayout,
            );
        }

        {
            let used = self.used_mut(node);
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
        crate::layout::place_child(
            self.state,
            &self.callbacks,
            node,
            FfiCssPixelPoint {
                x: used_offset.inline_offset,
                y: used_offset.block_offset,
            },
        );

        let is_measurement = self.state.is_measurement();
        if !is_measurement {
            self.state
                .used_values_rare_data_for_node_mut(&self.callbacks, node)
                .abspos_layout_inputs = Some(inputs);
        }

        if let Some(child_layout) = child_layout {
            child_layout.finish();
        }
    }

    pub(crate) fn layout_children(&self, frame: &mut crate::layout::FcFrame<'pass>) {
        debug_assert!(!self.state.is_measurement());
        while let Some(child) = self.state.take_next_contained_abspos_child(frame.box_) {
            let child_box = child.child_box;
            if self.try_used_pointer(child_box).is_none() {
                self.state
                    .create_used_values(&self.callbacks, child_box, ContainingBlockConstraints::default());
            }
            self.resolve_anchor_insets(child_box);
            let inputs = AbsposLayoutInputs {
                static_position_rect: self
                    .resolve_static_position_relative_to_containing_block(child_box, child.static_position_rect),
                containing_block_info: child
                    .containing_block_info_override
                    .unwrap_or_else(|| self.base_containing_block_info(child_box)),
            };
            self.layout_element(frame, child_box, inputs);
        }
    }

    fn replay(&self, frame: &mut crate::layout::FcFrame<'pass>, node: Node) {
        let saved_inputs = self.callbacks.saved_abspos_layout_inputs(node);
        let found = saved_inputs.is_some();
        assert!(found);
        let mut inputs = saved_inputs.unwrap();
        if !inputs.containing_block_info.derives_from_own_computed_values {
            let (inline, block) = axis_modes(self.style(node));
            inputs.containing_block_info.inline_axis_mode = inline;
            inputs.containing_block_info.block_axis_mode = block;
        }
        // Partial relayout uses a fresh state and creates the replay root
        // exactly once.
        self.state
            .create_used_values(&self.callbacks, node, ContainingBlockConstraints::default());
        self.layout_element(frame, node, inputs);
    }

    fn compute_inset(&self, node: Node, containing_block_size: LogicalSize) {
        // Most boxes are neither relatively positioned nor carry anchor()
        // insets. Preserve the old C++ fast path without populating the
        // comprehensive Rust facts caches for those boxes.
        // SAFETY: The callback only reads the live node's computed values.
        if !unsafe {
            (self.callbacks.needs_inset_resolution)(self.callbacks.context, self.callbacks.shell(node))
        } {
            return;
        }
        let initial_style = self.style(node);
        if initial_style.inset_top().contains_anchor_function
            || initial_style.inset_right().contains_anchor_function
            || initial_style.inset_bottom().contains_anchor_function
            || initial_style.inset_left().contains_anchor_function
        {
            self.resolve_anchor_insets(node);
        }
        let style = self.style(node);
        if style.position() != positioning::RELATIVE {
            return;
        }

        let resolve_opposing = |first: FfiSizeValue, second: FfiSizeValue, basis: CssPixels| {
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

        let treat_percentage_as_auto = |value: FfiSizeValue| {
            if !value.contains_percentage {
                return value;
            }
            let mut containing_block = self.callbacks.containing_block(node);
            while !containing_block.is_invalid() {
                let facts = self.facts(containing_block);
                if !facts.is_anonymous() || facts.is_table_cell() {
                    break;
                }
                containing_block = self.callbacks.containing_block(containing_block);
            }
            if !containing_block.is_invalid() && !self.used(containing_block).has_definite_block_size() {
                FfiSizeValue::auto_value()
            } else {
                value
            }
        };
        let (top, bottom) = resolve_opposing(
            treat_percentage_as_auto(style.inset_top()),
            treat_percentage_as_auto(style.inset_bottom()),
            containing_block_size.block_size,
        );
        let used = self.used_mut(node);
        used.inset_left.set(left);
        used.inset_right.set(right);
        used.inset_top.set(top);
        used.inset_bottom.set(bottom);
    }
}

pub(crate) fn layout_contained_abspos_children(frame: &mut crate::layout::FcFrame<'_>) {
    AbsposEngine::new(frame.state, frame.callbacks).layout_children(frame);
}

/// Lays out every registered abspos child once the in-flow run has finished.
/// Queues are processed in the order their formatting contexts completed, so
/// deeper containing blocks come first and the root comes last: the layout
/// order anchor() acceptability assumes for absolutely positioned anchors.
/// A formatting context completing during the pass belongs to an absolutely
/// positioned subtree; its children are laid out at that completion point,
/// before later boxes of the queue that produced it.
pub(crate) fn run_abspos_layout_pass(
    state: &LayoutState,
    callbacks: FfiLayoutFcCallbacks,
    should_collect_devtools_layout_data: bool,
) {
    state.set_abspos_layout_pass_is_active(true);
    while let Some(root) = state.pop_from_abspos_layout_pass_queue() {
        if !state.has_contained_abspos_children(root) {
            continue;
        }
        let mut frame =
            crate::layout::FcFrame::new(state, root, LayoutMode::Normal, callbacks, should_collect_devtools_layout_data);
        layout_contained_abspos_children(&mut frame);
    }
    state.set_abspos_layout_pass_is_active(false);
    debug_assert!(
        state.all_registered_contained_abspos_children_are_laid_out(),
        "registered abspos children were left without layout after the pass"
    );
}

pub(crate) fn compute_inset_native(
    state: &LayoutState,
    callbacks: FfiLayoutFcCallbacks,
    node: Node,
    inline_size: CssPixels,
    block_size: CssPixels,
) {
    AbsposEngine::new(state, callbacks).compute_inset(
        node,
        LogicalSize {
            inline_size,
            block_size,
        },
    );
}
