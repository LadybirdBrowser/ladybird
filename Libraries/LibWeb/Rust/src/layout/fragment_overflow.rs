/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::{overflow, positioning, writing_mode};
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixels};
use crate::css::display::FfiDisplay;
use crate::layout::LayoutNodeArena;
use crate::layout::fragment_tree::{Fragment, FragmentLink};
use crate::layout::geometry::Frame;
use crate::layout::inline_content::{InlineContent, PhysicalPlane};
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts::{self, NodeShape};
use crate::layout::used_values::{self, FfiCssPixelSize};
use crate::painting::paint_read::{GeometryRead, PaintRead, PaintRow};
use crate::painting::paintable_data::FfiPixelBox;
use crate::painting::paintable_geometry::sides_to_physical;
use crate::painting::scrollable_overflow::physical_overflow_directions_of_style;
use crate::painting::visual_context::node_values::{self, TransformGeometry};
use crate::painting::{node_painting, scroll_snap, style_queries};
use libgfx_rust::AffineTransform;

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct FragmentScrollableOverflow {
    pub(crate) rect_relative_to_padding_box: CssPixelRect,
    pub(crate) has_scrollable_overflow: bool,
}

pub(crate) fn physical_content_size(fragment: &Fragment, frame: Frame) -> FfiCssPixelSize {
    frame.size_to_physical(fragment.content_inline_size, fragment.content_block_size)
}

pub(crate) fn physical_padding(fragment: &Fragment, frame: Frame) -> FfiPixelBox {
    sides_to_physical(
        frame,
        FfiPixelBox {
            top: fragment.padding_top,
            right: fragment.padding_right,
            bottom: fragment.padding_bottom,
            left: fragment.padding_left,
        },
    )
}

fn physical_border(fragment: &Fragment, frame: Frame) -> FfiPixelBox {
    sides_to_physical(
        frame,
        FfiPixelBox {
            top: fragment.border_top,
            right: fragment.border_right,
            bottom: fragment.border_bottom,
            left: fragment.border_left,
        },
    )
}

fn physical_border_box_edges(fragment: &Fragment, frame: Frame) -> FfiPixelBox {
    let border = physical_border(fragment, frame);
    if !fragment.uses_collapsing_borders_model {
        return border;
    }
    let is_table_box = fragment.is_collapsed_borders_table_box;
    FfiPixelBox {
        top: used_values::collapsed_border_share(border.top, true, is_table_box),
        right: used_values::collapsed_border_share(border.right, false, is_table_box),
        bottom: used_values::collapsed_border_share(border.bottom, false, is_table_box),
        left: used_values::collapsed_border_share(border.left, true, is_table_box),
    }
}

fn inflated_by(rect: CssPixelRect, sides: FfiPixelBox) -> CssPixelRect {
    rect.inflated(sides.top, sides.right, sides.bottom, sides.left)
}

struct BoxesRelativeToOwnPaddingBox {
    content_size: FfiCssPixelSize,
    padding_box: CssPixelRect,
    content_box: CssPixelRect,
    border_box: CssPixelRect,
}

impl BoxesRelativeToOwnPaddingBox {
    fn of(fragment: &Fragment, frame: Frame) -> Self {
        let content_size = physical_content_size(fragment, frame);
        let padding = physical_padding(fragment, frame);
        let content_box = CssPixelRect::new(padding.left, padding.top, content_size.width, content_size.height);
        let padding_box = inflated_by(content_box, padding);
        Self {
            content_size,
            padding_box,
            content_box,
            border_box: inflated_by(padding_box, physical_border_box_edges(fragment, frame)),
        }
    }
}

struct ContainedBoxesRelativeToContainerPaddingBox {
    content_box: CssPixelRect,
    padding_box: CssPixelRect,
    border_box: CssPixelRect,
}

impl ContainedBoxesRelativeToContainerPaddingBox {
    fn of(link: &FragmentLink, container: &BoxesRelativeToOwnPaddingBox) -> Self {
        let fragment = &link.fragment;
        let content_box = link
            .frame
            .rect_to_physical(
                CssPixelRect::new(
                    link.committed_offset.x,
                    link.committed_offset.y,
                    fragment.content_inline_size,
                    fragment.content_block_size,
                ),
                container.content_size,
            )
            .translated_by(container.content_box.location());
        let padding_box = inflated_by(content_box, physical_padding(fragment, link.frame));
        Self {
            content_box,
            padding_box,
            border_box: inflated_by(padding_box, physical_border_box_edges(fragment, link.frame)),
        }
    }
}

struct ContainedBoxTransformGeometry<'boxes> {
    contained: &'boxes ContainedBoxesRelativeToContainerPaddingBox,
    container: &'boxes BoxesRelativeToOwnPaddingBox,
    svg_additional_element_transform: Option<AffineTransform>,
}

impl TransformGeometry for ContainedBoxTransformGeometry<'_> {
    fn content_box(&self) -> CssPixelRect {
        self.contained.content_box
    }

    fn border_box(&self) -> CssPixelRect {
        self.contained.border_box
    }

    fn nearest_svg_viewport_user_rect(&self) -> Option<CssPixelRect> {
        None
    }

    fn containing_block_box(&self, coord_box: u16) -> CssPixelRect {
        use crate::css::css_enums::keyword;
        match coord_box {
            keyword::CONTENT_BOX | keyword::FILL_BOX => self.container.content_box,
            keyword::PADDING_BOX => self.container.padding_box,
            _ => self.container.border_box,
        }
    }

    fn svg_additional_element_transform(&self) -> Option<AffineTransform> {
        self.svg_additional_element_transform
    }
}

pub(crate) fn contained_links(fragment: &Fragment) -> impl Iterator<Item = &FragmentLink> {
    let node = fragment.node;
    fragment.children.iter().flat_map(move |link| {
        let outer_links = link
            .fragment
            .holds_links_of_outer_containing_blocks
            .then_some(&link.fragment.children)
            .into_iter()
            .flatten()
            .filter(move |outer_link| outer_link.containing_block == node);
        (link.containing_block == node)
            .then_some(link)
            .into_iter()
            .chain(outer_links)
    })
}

pub(crate) fn include_links_held_beside_their_containing_blocks(
    arena: &LayoutNodeArena,
    run_root: NodeSlotId,
    links: &mut [FragmentLink],
) {
    let mut held_beside: crate::fast_hash::FastMap<NodeSlotId, smallvec::SmallVec<[usize; 2]>> = Default::default();
    for (index, link) in links.iter().enumerate() {
        if !link.containing_block.is_invalid() && link.containing_block != run_root {
            held_beside.entry(link.containing_block).or_default().push(index);
        }
    }
    for index in 0..links.len() {
        let Some(held_beside_indices) = held_beside.get(&links[index].fragment.node) else {
            continue;
        };
        let fragment = &links[index].fragment;
        let overflow = scrollable_overflow_of(
            arena,
            fragment,
            links[index].frame,
            contained_links(fragment).chain(held_beside_indices.iter().map(|&held| &links[held])),
        );
        if overflow != fragment.scrollable_overflow {
            links[index].fragment = std::sync::Arc::new(fragment.with_scrollable_overflow(overflow));
        }
    }
}

pub(crate) fn holds_links_of_outer_containing_blocks(node: NodeSlotId, children: &[FragmentLink]) -> bool {
    children
        .iter()
        .any(|link| !link.containing_block.is_invalid() && link.containing_block != node)
}

fn line_clamp_clip_rect(style: Option<ComputedValuesView<'_>>, content_box: CssPixelRect) -> CssPixelRect {
    let extent = CssPixels::from_integer((crate::css::css_pixels::MAX_INTEGER_VALUE / 4) as i64);
    let writing_mode = style.map_or(writing_mode::HORIZONTAL_TB, |style| style.writing_mode());
    if writing_mode == writing_mode::HORIZONTAL_TB {
        CssPixelRect::new(-extent, -extent, extent * 2, content_box.bottom() + extent)
    } else if style.is_some_and(|style| style.block_axis_is_reverse()) {
        CssPixelRect::new(content_box.x, -extent, extent - content_box.x, extent * 2)
    } else {
        CssPixelRect::new(-extent, -extent, content_box.right() + extent, extent * 2)
    }
}

// NB: The nearest line-clamp container among a hidden box's ancestors is the one whose formatting context hid it.
fn hidden_content_is_scrollable_overflow(
    arena: &LayoutNodeArena,
    fragment_being_built: &Fragment,
    hidden_box: NodeSlotId,
) -> bool {
    let mut ancestor = arena.node_parent_if_live(hidden_box);
    while let Some(node) = ancestor {
        if node == fragment_being_built.node {
            if fragment_being_built.has_line_clamp_point {
                return fragment_being_built.clamped_content_is_scrollable_overflow;
            }
        } else if let Some(keeps_clamped_content) = line_clamp_container_keeps_clamped_content_scrollable(arena, node) {
            return keeps_clamped_content;
        }
        ancestor = arena.node_parent_if_live(node);
    }
    false
}

fn line_clamp_container_keeps_clamped_content_scrollable(arena: &LayoutNodeArena, node: NodeSlotId) -> Option<bool> {
    use crate::css::css_enums::{continue_value, webkit_box_orient};
    let style = arena.node_style_if_live(node)?;
    let computed_continue = style.continue_();
    // AD-HOC: Content after a -webkit-legacy clamp point stays scrollable overflow, matching other engines.
    let is_webkit_legacy_line_clamp_container = computed_continue == continue_value::_WEBKIT_LEGACY
        && style.display_before_box_type_transformation().is_webkit_box_inside()
        && style.webkit_box_orient() == webkit_box_orient::VERTICAL;
    let is_line_clamp_container = (computed_continue == continue_value::COLLAPSE
        || is_webkit_legacy_line_clamp_container)
        && !node_facts::node_lays_out_in_columns(arena.data(node), style);
    is_line_clamp_container.then_some(is_webkit_legacy_line_clamp_container)
}

struct InlineBoxUnions {
    padding_box: CssPixelRect,
    border_box: CssPixelRect,
}

fn inline_box_unions(
    plane: &PhysicalPlane<'_>,
    content: &InlineContent,
    link: &FragmentLink,
    content_origin: CssPixelPoint,
) -> Option<InlineBoxUnions> {
    let padding_widths = physical_padding(&link.fragment, link.frame);
    let border_widths = physical_border(&link.fragment, link.frame);
    let unite = |union: &mut Option<CssPixelRect>, rect: CssPixelRect| match union {
        None => *union = Some(rect),
        Some(_) if rect.is_empty() => {}
        Some(current) if current.is_empty() => *current = rect,
        Some(current) => current.unite(rect),
    };
    let mut padding_union = None;
    let mut border_union = None;
    for &piece_index in content.inline_boxes.piece_indices_of(link.fragment.node) {
        let piece = &content.inline_box_pieces[piece_index as usize];
        let border_rect = plane.piece_border_box_rect(piece);
        if piece.is_geometry_only_placeholder {
            let padding_rect = inflated_by(border_rect, padding_widths);
            unite(&mut padding_union, padding_rect);
            unite(&mut border_union, inflated_by(padding_rect, border_widths));
            continue;
        }
        let physical_piece = crate::layout::inline_content::InlineBoxPieceRecord {
            present_edges: plane.piece_present_edges(piece),
            ..*piece
        };
        unite(
            &mut padding_union,
            physical_piece.shrunken_by_present_edges(border_rect, border_widths),
        );
        unite(&mut border_union, border_rect);
    }
    Some(InlineBoxUnions {
        padding_box: padding_union?.translated_by(content_origin),
        border_box: border_union?.translated_by(content_origin),
    })
}

// https://drafts.csswg.org/css-overflow-3/#scrollable-overflow-calculation
pub(crate) fn scrollable_overflow_of<'link>(
    arena: &LayoutNodeArena,
    fragment: &Fragment,
    frame: Frame,
    contained_links: impl Iterator<Item = &'link FragmentLink>,
) -> FragmentScrollableOverflow {
    let node = fragment.node;
    let own = BoxesRelativeToOwnPaddingBox::of(fragment, frame);
    let Some(row) = GeometryRead::node(arena, node) else {
        return FragmentScrollableOverflow {
            rect_relative_to_padding_box: own.padding_box,
            has_scrollable_overflow: false,
        };
    };
    let kind = row.kind();
    let style = row.style();

    // Replaced SVG viewports clip their content
    if kind == NodeKind::SVGSVGBox {
        return FragmentScrollableOverflow {
            rect_relative_to_padding_box: own.padding_box,
            has_scrollable_overflow: false,
        };
    }

    let box_is_scroll_container = node_facts::kind_and_style_make_scroll_container(kind, style);
    let line_clamp_clip = (fragment.has_line_clamp_point && !fragment.clamped_content_is_scrollable_overflow)
        .then(|| line_clamp_clip_rect(style, own.content_box));
    let clip_in_flow = |rect: CssPixelRect| line_clamp_clip.map_or(rect, |clip| rect.intersected(clip));
    let overflow_directions = style.map_or_else(Default::default, physical_overflow_directions_of_style);

    // The scrollable overflow area of a box is the union of:

    // - Its own padding box.
    let mut scrollable_overflow_rect = own.padding_box;
    let mut in_flow_and_floated_content_bounds = own.content_box;

    // - All line boxes it directly contains.
    let line_content = fragment
        .line_data
        .as_deref()
        .filter(|_| node_painting::has_lines(arena, node));
    let plane = line_content.map(|content| PhysicalPlane::of(content, own.content_size));
    if let (Some(content), Some(plane)) = (line_content, &plane) {
        let origin = own.content_box.location();
        let line_rects = content.lines.iter().map(|line| plane.rect(line.rect));
        let elided_content_rects = content.elided_content_rects.iter().map(|&(_, rect)| plane.rect(rect));
        let lines_after_clamp_point = content
            .lines_after_clamp_point_rect
            .as_deref()
            .map(|&rect| plane.rect(rect));
        for line_rect in line_rects.chain(elided_content_rects).chain(lines_after_clamp_point) {
            let line_rect = line_rect.translated_by(origin);
            scrollable_overflow_rect.unite(line_rect);
            in_flow_and_floated_content_bounds.unite_horizontally(line_rect);
            in_flow_and_floated_content_bounds.unite_vertically(line_rect);
        }
        for text_fragment in &content.fragments {
            let mut fragment_rect = plane.fragment_rect(text_fragment).translated_by(origin);
            if arena.node_is_in_focused_text_control(text_fragment.layout_node)
                && let Some(style_source_style) = arena.node_style_if_live(text_fragment.style_source)
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
            fragment_rect = clip_in_flow(fragment_rect);
            scrollable_overflow_rect.unite(fragment_rect);
            in_flow_and_floated_content_bounds.unite(fragment_rect);
        }
    }

    // https://drafts.csswg.org/css-grid-2/#overflow
    // The grid is also included in a grid container's scrollable overflow region.
    // INTEROP: Blink only does this when the grid container is a scroll container, so the grid does not reach the
    //          scrollable overflow of its ancestors.
    if box_is_scroll_container && let Some(tracks) = &fragment.used_grid_tracks {
        let grid_area = tracks.grid_area.translated_by(own.content_box.location());
        // NB: A grid with tracks in one axis only, e.g. columns but no rows, still reaches the overflow in that axis.
        if grid_area.width > CssPixels::default() {
            scrollable_overflow_rect.unite_horizontally(grid_area);
            in_flow_and_floated_content_bounds.unite_horizontally(grid_area);
        }
        if grid_area.height > CssPixels::default() {
            scrollable_overflow_rect.unite_vertically(grid_area);
            in_flow_and_floated_content_bounds.unite_vertically(grid_area);
        }
    }

    // - The border boxes of all boxes for which it is the containing block and whose border boxes are positioned not
    //   wholly in the negative scrollable overflow region,
    //   FIXME: accounting for 3D transforms by projecting each box onto the plane of the element that establishes
    //          its 3D rendering context. [CSS3-TRANSFORMS]
    let container_inline_axis_is_horizontal =
        style.is_none_or(|style| style.writing_mode() == writing_mode::HORIZONTAL_TB);
    for link in contained_links {
        let child = &link.fragment;
        let child_node = child.node;
        let Some(child_row) = GeometryRead::node(arena, child_node) else {
            continue;
        };
        if !node_facts::kind_is_box(child_row.kind()) {
            continue;
        }

        let child_style = child_row.style();
        let child_position = child_style.map_or(positioning::STATIC, |style| style.position());
        let child_is_absolutely_positioned = matches!(child_position, positioning::ABSOLUTE | positioning::FIXED);

        // https://drafts.csswg.org/css-position/#fixed-positioning-containing-block
        // [..] As a result, parts of fixed-positioned boxes that extend outside the layout viewport/page area
        //      cannot be scrolled to and will not print.
        // FIXME: Properly establish the fixed positioning containing block for `position: fixed`
        if child_position == positioning::FIXED {
            continue;
        }
        if link.is_invisible_for_line_clamp && !hidden_content_is_scrollable_overflow(arena, fragment, child_node) {
            continue;
        }

        let child_has_css_transform =
            child_style.is_some_and(|style| style_queries::has_css_transform(arena, child_node, style));
        let child_flags = child_row.flags();
        let child_is_flex_or_grid_item = child_flags & (NodeFlag::IsFlexItem as u32 | NodeFlag::IsGridItem as u32) != 0;
        let child_is_floating = !child_is_flex_or_grid_item && child_style.is_some_and(|style| style.is_floating());
        let child_display = child_style.map_or_else(FfiDisplay::block, |style| style.display());
        let child_is_fragmented_inline = child_row.is_fragmented_inline();

        let mut contained = ContainedBoxesRelativeToContainerPaddingBox::of(link, &own);
        if child_is_fragmented_inline {
            let (Some(content), Some(plane)) = (line_content, &plane) else {
                continue;
            };
            let Some(unions) = inline_box_unions(plane, content, link, own.content_box.location()) else {
                continue;
            };
            contained.padding_box = unions.padding_box;
            contained.border_box = unions.border_box;
        }
        let untransformed_child_border_box = contained.border_box;
        // NB: Collect normal-flow bounds before filtering visible overflow, which relative positioning can move
        //     wholly before the scroll origin.
        let child_is_in_flow = !(child_is_floating || child_is_absolutely_positioned);
        if box_is_scroll_container && (child_is_in_flow || child_is_floating) {
            let mut normal_flow_border_box = untransformed_child_border_box;
            if child_position == positioning::RELATIVE {
                let inset = sides_to_physical(
                    link.frame,
                    FfiPixelBox {
                        top: link.inset_top,
                        right: link.inset_right,
                        bottom: link.inset_bottom,
                        left: link.inset_left,
                    },
                );
                normal_flow_border_box = normal_flow_border_box.translated(-inset.left, -inset.top);
            }
            // INTEROP: The edges of in-flow and floated content, after which the container's end padding comes,
            //          are those of the margin boxes of block-level boxes, flex items and grid items, as in Blink:
            //          with their computed margins, auto counting as zero, and where they are laid out before
            //          relative positioning. Toward the end of the scrollable overflow a negative margin counts,
            //          but no further than the size of the box; toward its start, it does not.
            let child_is_block_level = child_is_flex_or_grid_item || !child_display.is_inline_outside();
            let content_box = match child_style {
                Some(style) if child_is_block_level || child_row.is_atomic_inline() => {
                    let border_box = normal_flow_border_box;
                    let margin = &style.surround().margin;
                    let basis = if container_inline_axis_is_horizontal {
                        own.content_size.width
                    } else {
                        own.content_size.height
                    };
                    let zero = CssPixels::from_raw(0);
                    // NB: An atomic inline's inline margins contribute even when its line box has zero area.
                    //     Its block-axis margins are already accounted for by the line box.
                    let (left, right) = if child_is_block_level || container_inline_axis_is_horizontal {
                        (margin.left.to_px(basis), margin.right.to_px(basis))
                    } else {
                        (zero, zero)
                    };
                    let (top, bottom) = if child_is_block_level || !container_inline_axis_is_horizontal {
                        (margin.top.to_px(basis), margin.bottom.to_px(basis))
                    } else {
                        (zero, zero)
                    };
                    let (left, right) = if overflow_directions.horizontal_axis_is_positive {
                        (left.max(zero), right.max(-border_box.width))
                    } else {
                        (left.max(-border_box.width), right.max(zero))
                    };
                    let (top, bottom) = if overflow_directions.vertical_axis_is_positive {
                        (top.max(zero), bottom.max(-border_box.height))
                    } else {
                        (top.max(-border_box.height), bottom.max(zero))
                    };
                    border_box.inflated(top, right, bottom, left)
                }
                _ => normal_flow_border_box,
            };
            // NB: Empty boxes still contribute their extents to the padding alignment bounds.
            in_flow_and_floated_content_bounds.unite_horizontally(content_box);
            in_flow_and_floated_content_bounds.unite_vertically(content_box);
        }
        let transform = child_has_css_transform
            .then(|| {
                let svg_additional_element_transform = style_queries::kind_is_svg_element_box(child_row.kind())
                    .then(|| child.svg.as_ref()?.additional_element_transform)
                    .flatten()
                    .map(Into::into);
                node_values::compute_transform_against(
                    arena,
                    child_node,
                    &ContainedBoxTransformGeometry {
                        contained: &contained,
                        container: &own,
                        svg_additional_element_transform,
                    },
                    1.0,
                )
            })
            .flatten()
            .map(|(transform, _is_invertible)| transform);
        let through_transform = |rect: CssPixelRect| {
            transform.as_ref().map_or(rect, |transform| {
                scroll_snap::map_rect_through_css_transform(transform.matrix, transform.origin, rect)
            })
        };
        let mut child_border_box = through_transform(untransformed_child_border_box);
        if !child_is_absolutely_positioned {
            child_border_box = clip_in_flow(child_border_box);
        }
        // INTEROP: Chrome and Firefox retain reachable overflow in each axis independently, even when
        //          the border box lies wholly in the other axis's unreachable region.
        let wholly_in_unreachable_horizontal_axis = if overflow_directions.horizontal_axis_is_positive {
            child_border_box.right() < own.padding_box.x
        } else {
            child_border_box.x > own.padding_box.right()
        };
        let wholly_in_unreachable_vertical_axis = if overflow_directions.vertical_axis_is_positive {
            child_border_box.bottom() < own.padding_box.y
        } else {
            child_border_box.y > own.padding_box.bottom()
        };
        if wholly_in_unreachable_horizontal_axis && wholly_in_unreachable_vertical_axis {
            continue;
        }

        // Border boxes with zero area do not affect the scrollable overflow area.
        if !child_border_box.is_empty() {
            if !wholly_in_unreachable_horizontal_axis {
                scrollable_overflow_rect.unite_horizontally(child_border_box);
            }
            if !wholly_in_unreachable_vertical_axis {
                scrollable_overflow_rect.unite_vertically(child_border_box);
            }
        }

        // - The scrollable overflow areas of all of the above boxes (including zero-area boxes and accounting for
        //   transforms as described above), provided they themselves have overflow: visible (i.e. do not themselves
        //   trap the overflow) and that scrollable overflow is not already clipped (e.g. by the clip property or the
        //   contain property).
        // Scrollable overflow is already clipped by the contain property.
        if child_style.is_some_and(|style| {
            style_queries::has_layout_containment(arena, child_node, style)
                || style_queries::has_paint_containment(arena, child_node, style)
        }) {
            continue;
        }

        let child_overflow_x = child_style.map_or(overflow::VISIBLE, |style| style.overflow_x());
        let child_overflow_y = child_style.map_or(overflow::VISIBLE, |style| style.overflow_y());
        if child_overflow_x == overflow::VISIBLE || child_overflow_y == overflow::VISIBLE {
            let untransformed_child_scrollable_overflow = child
                .scrollable_overflow
                .rect_relative_to_padding_box
                .translated_by(contained.padding_box.location());
            let mut child_scrollable_overflow = through_transform(untransformed_child_scrollable_overflow);
            if !child_is_absolutely_positioned {
                child_scrollable_overflow = clip_in_flow(child_scrollable_overflow);
            }
            if !child_scrollable_overflow.is_empty() {
                if child_overflow_x == overflow::VISIBLE && !wholly_in_unreachable_horizontal_axis {
                    scrollable_overflow_rect.unite_horizontally(child_scrollable_overflow);
                }
                if child_overflow_y == overflow::VISIBLE && !wholly_in_unreachable_vertical_axis {
                    scrollable_overflow_rect.unite_vertically(child_scrollable_overflow);
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
    if box_is_scroll_container {
        // NB: The box's own padding surrounds its in-flow and floated content. Whatever of it falls inside the padding
        //     box, or in the unreachable scrollable overflow region, adds nothing. This matches Blink.
        let padding = physical_padding(fragment, frame);
        scrollable_overflow_rect.unite(inflated_by(clip_in_flow(in_flow_and_floated_content_bounds), padding));
    }

    let mut has_scrollable_overflow =
        box_is_scroll_container && !own.padding_box.contains_rect(scrollable_overflow_rect);

    // Additionally, due to Web-compatibility constraints (caused by authors exploiting legacy bugs to surreptitiously
    // hide content from visual readers but not search engines and/or speech output), UAs must clip any content in the
    // unreachable scrollable overflow region.
    //
    // https://drafts.csswg.org/css-overflow-3/#unreachable-scrollable-overflow-region
    // Unless otherwise adjusted (e.g. by content alignment [css-align-3]), the area beyond the scroll origin in either
    // axis is considered the unreachable scrollable overflow region: content rendered here is not accessible to the
    // reader, see § 2.2 Scrollable Overflow.
    let left = if overflow_directions.horizontal_axis_is_positive {
        scrollable_overflow_rect.x.max(own.padding_box.x)
    } else {
        scrollable_overflow_rect.x
    };
    let top = if overflow_directions.vertical_axis_is_positive {
        scrollable_overflow_rect.y.max(own.padding_box.y)
    } else {
        scrollable_overflow_rect.y
    };
    let right = if overflow_directions.horizontal_axis_is_positive {
        scrollable_overflow_rect.right()
    } else {
        scrollable_overflow_rect.right().min(own.padding_box.right())
    };
    let bottom = if overflow_directions.vertical_axis_is_positive {
        scrollable_overflow_rect.bottom()
    } else {
        scrollable_overflow_rect.bottom().min(own.padding_box.bottom())
    };
    if left != scrollable_overflow_rect.x
        || top != scrollable_overflow_rect.y
        || right != scrollable_overflow_rect.right()
        || bottom != scrollable_overflow_rect.bottom()
    {
        let zero = CssPixels::from_raw(0);
        scrollable_overflow_rect = CssPixelRect::new(left, top, (right - left).max(zero), (bottom - top).max(zero));
        has_scrollable_overflow = !own.padding_box.contains_rect(scrollable_overflow_rect) && box_is_scroll_container;
    }

    FragmentScrollableOverflow {
        rect_relative_to_padding_box: scrollable_overflow_rect,
        has_scrollable_overflow,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::css::computed_values::{
        BoxValues, ComputedLengthPercentageOrAuto, ComputedStyleValueHandle, GroupPayloadRef, InheritedBoxValues,
        SurroundValues, clone_group_payload, release_group_payload,
    };
    use crate::css::css_enums::overflow;
    use crate::css::table_group_builder::group_index;
    use crate::layout::node_data::{FfiStylePayloads, StylePayloadsRef};

    #[test]
    fn overflow_percentage_margins_use_the_containing_blocks_inline_size() {
        for (mode, expected_bottom) in [
            (writing_mode::HORIZONTAL_TB, 260),
            (writing_mode::VERTICAL_LR, 310),
            (writing_mode::VERTICAL_RL, 310),
        ] {
            let mut arena = LayoutNodeArena::new();
            let mut container_style = FfiStylePayloads {
                groups: std::array::from_fn(|index| GroupPayloadRef::default_payload(index).as_ptr()),
            };
            let mut child_style = container_style;
            // SAFETY: These are private clones of the corresponding default payloads.
            let (box_payload, inherited, surround) = unsafe {
                (
                    clone_group_payload(group_index::BOX, container_style.groups[group_index::BOX]),
                    clone_group_payload(
                        group_index::INHERITED_BOX,
                        container_style.groups[group_index::INHERITED_BOX],
                    ),
                    clone_group_payload(group_index::SURROUND, child_style.groups[group_index::SURROUND]),
                )
            };
            // SAFETY: Each private clone has its group's payload layout and no readers yet.
            unsafe {
                (*box_payload.cast::<BoxValues>()).display = FfiDisplay::block();
                (*box_payload.cast::<BoxValues>()).overflow_x = overflow::AUTO;
                (*box_payload.cast::<BoxValues>()).overflow_y = overflow::AUTO;
                (*inherited.cast::<InheritedBoxValues>()).writing_mode = mode;
                (*surround.cast::<SurroundValues>()).margin.bottom = ComputedLengthPercentageOrAuto {
                    is_auto: false,
                    value: ComputedStyleValueHandle::percentage(50.0),
                };
            }
            container_style.groups[group_index::BOX] = box_payload;
            container_style.groups[group_index::INHERITED_BOX] = inherited;
            child_style.groups[group_index::BOX] = box_payload;
            child_style.groups[group_index::INHERITED_BOX] = inherited;
            child_style.groups[group_index::SURROUND] = surround;
            let container = arena.allocate_for_test().slot;
            let child = arena.allocate_for_test().slot;
            for (node, style) in [(container, &container_style), (child, &child_style)] {
                arena.write_shape(node).set_kind(NodeKind::BlockContainer);
                arena.set_node_style(node, 0, StylePayloadsRef::new(style));
            }
            arena.insert_child(container, child, NodeSlotId::INVALID);
            let mut container_link = FragmentLink::for_test(container);
            let fragment = std::sync::Arc::get_mut(&mut container_link.fragment).unwrap();
            fragment.content_inline_size = CssPixels::from_integer(100);
            fragment.content_block_size = CssPixels::from_integer(200);
            fragment.padding_bottom = CssPixels::from_integer(10);
            let mut child_link = FragmentLink::for_test(child);
            let fragment = std::sync::Arc::get_mut(&mut child_link.fragment).unwrap();
            fragment.content_inline_size = CssPixels::from_integer(20);
            fragment.content_block_size = CssPixels::from_integer(20);
            child_link.committed_offset.y = CssPixels::from_integer(180).into();
            child_link.containing_block = container;
            let overflow = scrollable_overflow_of(
                &arena,
                &container_link.fragment,
                Frame::PHYSICAL,
                std::iter::once(&child_link),
            );
            assert_eq!(
                overflow.rect_relative_to_padding_box.bottom(),
                CssPixels::from_integer(expected_bottom),
                "writing mode {mode}"
            );
            arena
                .free_subtree(container)
                .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
            release_group_payload(group_index::BOX, box_payload);
            release_group_payload(group_index::INHERITED_BOX, inherited);
            release_group_payload(group_index::SURROUND, surround);
        }
    }
}
