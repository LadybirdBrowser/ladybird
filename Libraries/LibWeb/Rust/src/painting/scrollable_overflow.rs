/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::{flex_direction, flex_wrap, overflow, positioning, writing_mode};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::css::display::FfiDisplay;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::painting::host::{FfiScrollableOverflowHostCallbacks, FfiVisualContextHostCallbacks};
use crate::painting::paintable_data::FfiOverflowData;
use crate::painting::paintable_rows::{PaintableRowsRead, PaintableRowsWrite};
use crate::painting::visual_context::node_values;
use crate::painting::{paintable_geometry, style_queries, text_fragment};
use libgfx_rust::matrix::{AffineTransform, FloatMatrix4x4};
use std::collections::HashMap;

pub(crate) fn refill_contained_boxes_index(
    layout_arena: &impl PaintableRowsRead,
    root: NodeSlotId,
    contained_boxes_by_containing_block: &mut HashMap<NodeSlotId, Vec<NodeSlotId>>,
) {
    for contained_boxes in contained_boxes_by_containing_block.values_mut() {
        contained_boxes.clear();
    }
    if !layout_arena.shell_if_live(root).is_null() {
        let mut stack = vec![root];
        while let Some(node) = stack.pop() {
            if node != root
                && let Some(sibling) = layout_arena.node_next_sibling_if_live(node)
            {
                stack.push(sibling);
            }
            if let Some(first_child) = layout_arena.node_first_child_if_live(node) {
                stack.push(first_child);
            }
            let node_is_box_kind = layout_arena
                .node_kind_if_live(node)
                .is_some_and(crate::layout::kind_is_box);
            if !node_is_box_kind || !layout_arena.paintable_row_is_populated(node) {
                continue;
            }
            if let Some(containing_block) = layout_arena.node_containing_block_if_live(node) {
                contained_boxes_by_containing_block
                    .entry(containing_block)
                    .or_default()
                    .push(node);
            }
        }
    }
    contained_boxes_by_containing_block.retain(|_, contained_boxes| !contained_boxes.is_empty());
}

pub(crate) struct PhysicalOverflowDirections {
    pub(crate) horizontal_axis_is_positive: bool,
    pub(crate) vertical_axis_is_positive: bool,
}

impl Default for PhysicalOverflowDirections {
    fn default() -> Self {
        Self {
            horizontal_axis_is_positive: true,
            vertical_axis_is_positive: true,
        }
    }
}

struct AxisDirection {
    is_horizontal: bool,
    is_reverse: bool,
}

// https://drafts.csswg.org/cssom-view/#overflow-directions
pub(crate) fn physical_overflow_directions(
    layout_arena: &LayoutNodeArena,
    node: NodeSlotId,
) -> PhysicalOverflowDirections {
    // A scrolling box of a viewport or element has two overflow directions, which are the block-end and inline-end
    // directions for that viewport or element.
    let Some(style) = layout_arena.node_style_if_live(node) else {
        return PhysicalOverflowDirections::default();
    };

    let mut inline_axis = AxisDirection {
        is_horizontal: style.writing_mode() == writing_mode::HORIZONTAL_TB,
        is_reverse: style.inline_axis_is_reverse(),
    };
    let mut block_axis = AxisDirection {
        is_horizontal: !inline_axis.is_horizontal,
        is_reverse: style.block_axis_is_reverse(),
    };

    let display = style.display();
    if display.is_flex_inside() {
        let container_flex_direction = style.flex_direction();
        let is_row_layout = matches!(
            container_flex_direction,
            flex_direction::ROW | flex_direction::ROW_REVERSE
        );
        let (main_axis, cross_axis) = if is_row_layout {
            (&mut inline_axis, &mut block_axis)
        } else {
            (&mut block_axis, &mut inline_axis)
        };

        if matches!(
            container_flex_direction,
            flex_direction::ROW_REVERSE | flex_direction::COLUMN_REVERSE
        ) {
            main_axis.is_reverse = !main_axis.is_reverse;
        }

        // AD-HOC: A legacy webkit box ignores `flex-wrap`, matching other engines.
        if !display.is_webkit_box_inside() && style.flex_wrap() == flex_wrap::WRAP_REVERSE {
            cross_axis.is_reverse = !cross_axis.is_reverse;
        }
    }

    let (horizontal_axis, vertical_axis) = if inline_axis.is_horizontal {
        (inline_axis, block_axis)
    } else {
        (block_axis, inline_axis)
    };
    PhysicalOverflowDirections {
        horizontal_axis_is_positive: !horizontal_axis.is_reverse,
        vertical_axis_is_positive: !vertical_axis.is_reverse,
    }
}

fn extract_two_dimensional_affine_transform(matrix: &FloatMatrix4x4) -> AffineTransform {
    let elements = &matrix.elements;
    AffineTransform {
        values: [
            elements[0][0],
            elements[1][0],
            elements[0][1],
            elements[1][1],
            elements[0][3],
            elements[1][3],
        ],
    }
}

fn affine_is_identity_or_translation(affine: &AffineTransform) -> bool {
    let [a, b, c, d, _, _] = affine.values;
    a == 1.0 && b == 0.0 && c == 0.0 && d == 1.0
}

fn affine_map_point(affine: &AffineTransform, x: f32, y: f32) -> (f32, f32) {
    let [a, b, c, d, e, f] = affine.values;
    (a * x + c * y + e, b * x + d * y + f)
}

struct FloatRectEdges {
    x: f32,
    y: f32,
    width: f32,
    height: f32,
}

fn affine_map_float_rect(affine: &AffineTransform, rect: FloatRectEdges) -> FloatRectEdges {
    if affine_is_identity_or_translation(affine) {
        let [_, _, _, _, e, f] = affine.values;
        return FloatRectEdges {
            x: rect.x + e,
            y: rect.y + f,
            width: rect.width,
            height: rect.height,
        };
    }
    let (x1, y1) = affine_map_point(affine, rect.x, rect.y);
    let (x2, y2) = affine_map_point(affine, rect.x + rect.width, rect.y);
    let (x3, y3) = affine_map_point(affine, rect.x + rect.width, rect.y + rect.height);
    let (x4, y4) = affine_map_point(affine, rect.x, rect.y + rect.height);
    let left = x1.min(x2).min(x3).min(x4);
    let top = y1.min(y2).min(y3).min(y4);
    let right = x1.max(x2).max(x3).max(x4);
    let bottom = y1.max(y2).max(y3).max(y4);
    FloatRectEdges {
        x: left,
        y: top,
        width: right - left,
        height: bottom - top,
    }
}

fn apply_css_transform_to_scrollable_overflow_rect(
    layout_arena: &impl PaintableRowsRead,
    visual_context_callbacks: &FfiVisualContextHostCallbacks,
    box_paintable: NodeSlotId,
    rect: CssPixelRect,
    box_has_css_transform: bool,
) -> CssPixelRect {
    if !box_has_css_transform {
        return rect;
    }
    let Some((transform, _is_invertible)) =
        node_values::compute_transform(layout_arena, visual_context_callbacks, box_paintable, 1.0)
    else {
        return rect;
    };

    let affine = extract_two_dimensional_affine_transform(&transform.matrix);
    let transformed = affine_map_float_rect(
        &affine,
        FloatRectEdges {
            x: rect.x.to_float() - transform.origin.x,
            y: rect.y.to_float() - transform.origin.y,
            width: rect.width.to_float(),
            height: rect.height.to_float(),
        },
    );
    CssPixelRect::new(
        CssPixels::nearest_value_for_f32(transformed.x + transform.origin.x),
        CssPixels::nearest_value_for_f32(transformed.y + transform.origin.y),
        CssPixels::nearest_value_for_f32(transformed.width),
        CssPixels::nearest_value_for_f32(transformed.height),
    )
}

fn padding_inflated_scrollable_overflow(
    layout_arena: &impl PaintableRowsRead,
    box_paintable: NodeSlotId,
    box_node: NodeSlotId,
    in_flow_and_floated_content_bounds: CssPixelRect,
) -> CssPixelRect {
    let content_box = paintable_geometry::absolute_rect(layout_arena, box_paintable);
    let padding_box = paintable_geometry::absolute_padding_box_rect(layout_arena, box_paintable);
    let padding = paintable_geometry::committed_padding(layout_arena, box_paintable);
    let overflow_directions = physical_overflow_directions(layout_arena, box_node);

    let mut left = in_flow_and_floated_content_bounds.left();
    let mut top = in_flow_and_floated_content_bounds.top();
    let mut right = in_flow_and_floated_content_bounds.right();
    let mut bottom = in_flow_and_floated_content_bounds.bottom();

    let in_flow_bounds_overflow_content_box_in_horizontal_axis =
        left < content_box.left() || right > content_box.right();
    if in_flow_bounds_overflow_content_box_in_horizontal_axis {
        if overflow_directions.horizontal_axis_is_positive && right > content_box.right() {
            right += padding.right;
        } else if !overflow_directions.horizontal_axis_is_positive {
            left = left.min(padding_box.left()) - padding.left;
        }
    }

    let in_flow_bounds_overflow_content_box_in_vertical_axis = top < content_box.top() || bottom > content_box.bottom();
    if in_flow_bounds_overflow_content_box_in_vertical_axis {
        if overflow_directions.vertical_axis_is_positive && bottom > content_box.bottom() {
            bottom += padding.bottom;
        } else if !overflow_directions.vertical_axis_is_positive {
            top = top.min(padding_box.top()) - padding.top;
        }
    }

    CssPixelRect::new(left, top, right - left, bottom - top)
}

fn fragment_node_is_in_focused_text_control(
    layout_arena: &LayoutNodeArena,
    overflow_callbacks: &FfiScrollableOverflowHostCallbacks,
    node: NodeSlotId,
) -> bool {
    let flags = layout_arena.node_flags_if_live(node);
    let node_has_dom_node = flags & NodeFlag::Anonymous as u32 == 0;
    if !node_has_dom_node || flags & NodeFlag::IsInUserAgentShadowTree as u32 == 0 {
        return false;
    }
    let shell = layout_arena.shell_if_live(node);
    if shell.is_null() {
        return false;
    }
    overflow_callbacks.layout_node_is_in_focused_text_control(shell)
}

#[derive(Clone, Copy)]
pub(crate) struct OverflowAssignment {
    box_paintable: NodeSlotId,
    /// None re-validates the row's still-current stored rect without rewriting it.
    overflow_relative_to_padding_box: Option<FfiOverflowData>,
}

impl OverflowAssignment {
    pub(crate) fn apply(self, layout_arena: &mut impl PaintableRowsWrite) {
        let data = layout_arena.paintable_data_mut(self.box_paintable);
        if let Some(overflow) = self.overflow_relative_to_padding_box {
            data.overflow_relative_to_padding_box = overflow;
            data.overflow_valid_across_recommits = true;
        }
        data.overflow_measured_this_commit = true;
    }
}

fn store_overflow_data(
    assignments: &mut Vec<OverflowAssignment>,
    box_paintable: NodeSlotId,
    paintable_absolute_padding_box: CssPixelRect,
    scrollable_overflow_rect: CssPixelRect,
    has_scrollable_overflow: bool,
) {
    let rect_relative_to_padding_box =
        scrollable_overflow_rect.translated(-paintable_absolute_padding_box.x, -paintable_absolute_padding_box.y);
    assignments.push(OverflowAssignment {
        box_paintable,
        overflow_relative_to_padding_box: Some(FfiOverflowData {
            rect: rect_relative_to_padding_box.into(),
            has_scrollable_overflow,
        }),
    });
}

// https://drafts.csswg.org/css-overflow-3/#scrollable-overflow-calculation
pub(crate) fn measure_scrollable_overflow(
    layout_arena: &impl PaintableRowsRead,
    contained_boxes_by_containing_block: &HashMap<NodeSlotId, Vec<NodeSlotId>>,
    visual_context_callbacks: &FfiVisualContextHostCallbacks,
    overflow_callbacks: &FfiScrollableOverflowHostCallbacks,
    box_paintable: NodeSlotId,
) -> Vec<OverflowAssignment> {
    // Each box occurs under only one containing block, so this traversal visits each box at most once and can stage
    // assignments append-only.
    let mut assignments = Vec::new();
    measure_scrollable_overflow_impl(
        layout_arena,
        contained_boxes_by_containing_block,
        visual_context_callbacks,
        overflow_callbacks,
        box_paintable,
        &mut assignments,
    );
    assignments
}

fn measure_scrollable_overflow_impl(
    layout_arena: &impl PaintableRowsRead,
    contained_boxes_by_containing_block: &HashMap<NodeSlotId, Vec<NodeSlotId>>,
    visual_context_callbacks: &FfiVisualContextHostCallbacks,
    overflow_callbacks: &FfiScrollableOverflowHostCallbacks,
    box_paintable: NodeSlotId,
    assignments: &mut Vec<OverflowAssignment>,
) -> CssPixelRect {
    let (kind, still_valid_overflow) = {
        let data = layout_arena.paintable_data(box_paintable);
        if data.overflow_measured_this_commit {
            return CssPixelRect::from(data.overflow_relative_to_padding_box.rect)
                .translated_by(paintable_geometry::absolute_padding_box_rect(layout_arena, box_paintable).location());
        }
        (
            data.kind,
            data.overflow_valid_across_recommits
                .then_some(data.overflow_relative_to_padding_box),
        )
    };

    let box_node = box_paintable;
    let paintable_absolute_padding_box = paintable_geometry::absolute_padding_box_rect(layout_arena, box_paintable);
    let paintable_absolute_content_box = paintable_geometry::absolute_rect(layout_arena, box_paintable);

    if let Some(still_valid_overflow) = still_valid_overflow {
        let scrollable_overflow_rect =
            CssPixelRect::from(still_valid_overflow.rect).translated_by(paintable_absolute_padding_box.location());
        assignments.push(OverflowAssignment {
            box_paintable,
            overflow_relative_to_padding_box: None,
        });
        return scrollable_overflow_rect;
    }

    // The scrollable overflow area of a box is the union of:

    // - Its own padding box.
    let mut scrollable_overflow_rect = paintable_absolute_padding_box;
    let mut in_flow_and_floated_content_bounds = paintable_absolute_content_box;

    // Replaced SVG viewports clip their content
    if layout_arena.node_kind_if_live(box_node) == Some(NodeKind::SVGSVGBox) {
        store_overflow_data(
            assignments,
            box_paintable,
            paintable_absolute_padding_box,
            scrollable_overflow_rect,
            false,
        );
        return scrollable_overflow_rect;
    }

    let overflow_directions = physical_overflow_directions(layout_arena, box_node);

    // - All line boxes it directly contains.
    if kind.has_lines() {
        for fragment in &layout_arena.paintable_side_data(box_paintable).fragments {
            let mut fragment_rect = text_fragment::absolute_rect(layout_arena, fragment);
            if fragment_node_is_in_focused_text_control(layout_arena, overflow_callbacks, fragment.layout_node)
                && let Some(style_source_style) =
                    layout_arena.node_style_if_live(text_fragment::style_source(layout_arena, fragment))
            {
                // NB: Reserve one pixel of reachable inline-axis overflow for an end-of-line caret. This keeps the
                //     caret at its insertion position while allowing a text control to scroll the painted bar fully
                //     into view, matching the caret overflow accounted for by other engines.
                let one = CssPixels::from_integer(1);
                if style_source_style.writing_mode() == writing_mode::HORIZONTAL_TB {
                    if style_source_style.inline_axis_is_reverse() {
                        fragment_rect.set_left(fragment_rect.left() - one);
                    } else {
                        fragment_rect.set_right(fragment_rect.right() + one);
                    }
                } else if style_source_style.inline_axis_is_reverse() {
                    fragment_rect.set_top(fragment_rect.top() - one);
                } else {
                    fragment_rect.set_bottom(fragment_rect.bottom() + one);
                }
            }
            scrollable_overflow_rect.unite(fragment_rect);
            in_flow_and_floated_content_bounds.unite(fragment_rect);
        }
    }

    // - The border boxes of all boxes for which it is the containing block and whose border boxes are positioned not
    //   wholly in the negative scrollable overflow region,
    //   FIXME: accounting for 3D transforms by projecting each box onto the plane of the element that establishes
    //          its 3D rendering context. [CSS3-TRANSFORMS]
    if let Some(contained_boxes) = contained_boxes_by_containing_block.get(&box_node) {
        for &child_node in contained_boxes {
            if !layout_arena.paintable_row_is_populated(child_node)
                || layout_arena.node_containing_block_if_live(child_node) != Some(box_node)
            {
                continue;
            }

            let child_style = layout_arena.node_style_if_live(child_node);
            let child_position = child_style.map_or(positioning::STATIC, |style| style.position());

            // https://drafts.csswg.org/css-position/#fixed-positioning-containing-block
            // [..] As a result, parts of fixed-positioned boxes that extend outside the layout viewport/page area
            //      cannot be scrolled to and will not print.
            // FIXME: Properly establish the fixed positioning containing block for `position: fixed`
            if child_position == positioning::FIXED {
                continue;
            }

            let child_has_css_transform =
                child_style.is_some_and(|style| style_queries::has_css_transform(layout_arena, child_node, style));
            let child_is_flex_item = layout_arena.node_flags_if_live(child_node) & NodeFlag::IsFlexItem as u32 != 0;
            let child_is_floating = !child_is_flex_item && child_style.is_some_and(|style| style.is_floating());
            let child_display = child_style.map_or_else(FfiDisplay::block, |style| style.display());

            {
                let child_data = layout_arena.paintable_data(child_node);
                if child_position == positioning::STATIC
                    && child_display.is_inline_outside()
                    && !child_is_floating
                    && !child_has_css_transform
                    && child_data.overflow_valid_across_recommits
                {
                    let border = crate::painting::paintable_geometry::committed_border(layout_arena, child_node);
                    let zero = CssPixels::from_raw(0);
                    let has_border =
                        border.top != zero || border.right != zero || border.bottom != zero || border.left != zero;
                    if !has_border {
                        let padding = crate::painting::paintable_geometry::committed_padding(layout_arena, child_node);
                        let content_size =
                            crate::painting::paintable_geometry::committed_content_size(layout_arena, child_node);
                        let content_box_relative_to_padding_box =
                            CssPixelRect::new(padding.left, padding.top, content_size.width, content_size.height);
                        // The committed line fragment already contributes this content box. A box with no border whose
                        // cached overflow fits inside the content box cannot expand its containing block's overflow.
                        if content_box_relative_to_padding_box
                            .contains_rect(child_data.overflow_relative_to_padding_box.rect.into())
                        {
                            continue;
                        }
                    }
                }
            }

            let untransformed_child_border_box = paintable_geometry::absolute_border_box_rect(layout_arena, child_node);
            let child_border_box = apply_css_transform_to_scrollable_overflow_rect(
                layout_arena,
                visual_context_callbacks,
                child_node,
                untransformed_child_border_box,
                child_has_css_transform,
            );

            // NOTE: Only boxes that are not wholly in the unreachable scrollable overflow region contribute.
            let wholly_in_unreachable_horizontal_axis = if overflow_directions.horizontal_axis_is_positive {
                child_border_box.right() < paintable_absolute_padding_box.x
            } else {
                child_border_box.x > paintable_absolute_padding_box.right()
            };
            let wholly_in_unreachable_vertical_axis = if overflow_directions.vertical_axis_is_positive {
                child_border_box.bottom() < paintable_absolute_padding_box.y
            } else {
                child_border_box.y > paintable_absolute_padding_box.bottom()
            };
            if wholly_in_unreachable_horizontal_axis || wholly_in_unreachable_vertical_axis {
                continue;
            }

            // Border boxes with zero area do not affect the scrollable overflow area.
            if !child_border_box.is_empty() {
                scrollable_overflow_rect.unite(child_border_box);
                let child_is_absolutely_positioned =
                    matches!(child_position, positioning::ABSOLUTE | positioning::FIXED);
                let child_is_in_flow = !(child_is_floating || child_is_absolutely_positioned);
                if child_is_in_flow || child_is_floating {
                    in_flow_and_floated_content_bounds.unite(untransformed_child_border_box);
                }
            }

            // - The scrollable overflow areas of all of the above boxes (including zero-area boxes and accounting for
            //   transforms as described above), provided they themselves have overflow: visible (i.e. do not themselves
            //   trap the overflow) and that scrollable overflow is not already clipped (e.g. by the clip property or the
            //   contain property).
            // Scrollable overflow is already clipped by the contain property.
            if child_style.is_some_and(|style| {
                style_queries::has_layout_containment(layout_arena, child_node, style)
                    || style_queries::has_paint_containment(layout_arena, child_node, style)
            }) {
                continue;
            }

            let child_overflow_x = child_style.map_or(overflow::VISIBLE, |style| style.overflow_x());
            let child_overflow_y = child_style.map_or(overflow::VISIBLE, |style| style.overflow_y());
            if child_overflow_x == overflow::VISIBLE || child_overflow_y == overflow::VISIBLE {
                let untransformed_child_scrollable_overflow = measure_scrollable_overflow_impl(
                    layout_arena,
                    contained_boxes_by_containing_block,
                    visual_context_callbacks,
                    overflow_callbacks,
                    child_node,
                    assignments,
                );
                let child_scrollable_overflow = apply_css_transform_to_scrollable_overflow_rect(
                    layout_arena,
                    visual_context_callbacks,
                    child_node,
                    untransformed_child_scrollable_overflow,
                    child_has_css_transform,
                );
                if !child_scrollable_overflow.is_empty() {
                    if child_overflow_x == overflow::VISIBLE {
                        scrollable_overflow_rect.unite_horizontally(child_scrollable_overflow);
                    }
                    if child_overflow_y == overflow::VISIBLE {
                        scrollable_overflow_rect.unite_vertically(child_scrollable_overflow);
                    }
                }
            }
        }
    }

    // FIXME: - The margin areas of grid item and flex item boxes for which the box establishes a containing block.

    // - Additional padding added to the scrollable overflow rectangle as necessary to enable scroll positions that
    //   satisfy the requirements of both place-content: start and place-content: end alignment.
    //
    // NOTE: This padding represents, within the scrollable overflow rectangle, the box’s own padding so that when its
    //       content is scrolled to its end, there is padding between the edge of its in-flow (or floated) content and
    //       the border edge of the box. It typically ends up being exactly the same size as the box's own padding,
    //       except in a few cases--such as when an out-of-flow positioned element, or the visible overflow of a
    //       descendent, has already increased the size of the scrollable overflow rectangle outside the conceptual
    //       “content edge” of the scroll container’s content.
    let box_is_scroll_container = style_queries::is_scroll_container(layout_arena, box_node);
    if box_is_scroll_container {
        scrollable_overflow_rect.unite(padding_inflated_scrollable_overflow(
            layout_arena,
            box_paintable,
            box_node,
            in_flow_and_floated_content_bounds,
        ));
    }

    let mut has_scrollable_overflow =
        box_is_scroll_container && !paintable_absolute_padding_box.contains_rect(scrollable_overflow_rect);

    // Additionally, due to Web-compatibility constraints (caused by authors exploiting legacy bugs to surreptitiously
    // hide content from visual readers but not search engines and/or speech output), UAs must clip any content in the
    // unreachable scrollable overflow region.
    //
    // https://drafts.csswg.org/css-overflow-3/#unreachable-scrollable-overflow-region
    // Unless otherwise adjusted (e.g. by content alignment [css-align-3]), the area beyond the scroll origin in either
    // axis is considered the unreachable scrollable overflow region: content rendered here is not accessible to the
    // reader, see § 2.2 Scrollable Overflow.
    let left = if overflow_directions.horizontal_axis_is_positive {
        scrollable_overflow_rect.x.max(paintable_absolute_padding_box.x)
    } else {
        scrollable_overflow_rect.x
    };
    let top = if overflow_directions.vertical_axis_is_positive {
        scrollable_overflow_rect.y.max(paintable_absolute_padding_box.y)
    } else {
        scrollable_overflow_rect.y
    };
    let right = if overflow_directions.horizontal_axis_is_positive {
        scrollable_overflow_rect.right()
    } else {
        scrollable_overflow_rect
            .right()
            .min(paintable_absolute_padding_box.right())
    };
    let bottom = if overflow_directions.vertical_axis_is_positive {
        scrollable_overflow_rect.bottom()
    } else {
        scrollable_overflow_rect
            .bottom()
            .min(paintable_absolute_padding_box.bottom())
    };
    if left != scrollable_overflow_rect.x
        || top != scrollable_overflow_rect.y
        || right != scrollable_overflow_rect.right()
        || bottom != scrollable_overflow_rect.bottom()
    {
        let zero = CssPixels::from_raw(0);
        scrollable_overflow_rect = CssPixelRect::new(left, top, (right - left).max(zero), (bottom - top).max(zero));
        has_scrollable_overflow =
            !paintable_absolute_padding_box.contains_rect(scrollable_overflow_rect) && box_is_scroll_container;
    }

    store_overflow_data(
        assignments,
        box_paintable,
        paintable_absolute_padding_box,
        scrollable_overflow_rect,
        has_scrollable_overflow,
    );

    scrollable_overflow_rect
}
