/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::{line_style, overflow, text_decoration_line, text_decoration_style};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::painting::border_radii::BorderRadii;
use crate::painting::corner_shape::CornerShapes;
use crate::painting::fragment_ownership;
use crate::painting::paint_order_plan::PaintScope;
use crate::painting::paint_read::{GeometryRead, PaintRead};
use crate::painting::published_frame::PublishedPaintState;
use crate::painting::record::paint::{
    border_image, column_rule, outline, shadow, table_backgrounds, table_borders, text, text_decoration,
};
use crate::painting::visual_context::node_values::{OverflowClipRanges, overflow_clip_ranges};
use crate::painting::{border_shape, chrome_geometry, node_painting, paintable_geometry, style_queries, text_fragment};

#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub(crate) struct InkOverflow {
    pub rect: CssPixelRect,
    pub unbounded: bool,
    pub contribution_unbounded: bool,
    /// Whether something the rect bounds is there by the transform of a box below this one. A transform changes
    /// without anything being painted again, so the rect bounds where that is now and not where it will be.
    pub holds_what_a_transform_moves: bool,
}

fn inflated(rect: CssPixelRect, reach: CssPixels) -> CssPixelRect {
    rect.inflated(reach, reach, reach, reach)
}

fn text_stroke_reach(style: ComputedValuesView<'_>) -> CssPixels {
    CssPixels::nearest_value_for(style.inherited_text().webkit_text_stroke_width * 2.0)
}

fn css_rect_is_saturated(rect: CssPixelRect) -> bool {
    let [x, y, width, height] = [rect.x, rect.y, rect.width, rect.height].map(|value| i64::from(value.raw_value()));
    let minimum = i64::from(i32::MIN);
    let maximum = i64::from(i32::MAX);
    [x, y, width, height, x + width, y + height]
        .into_iter()
        .any(|value| value <= minimum || value >= maximum)
}

fn box_decoration_ink(source: &impl PaintRead, node: NodeSlotId, border_box: CssPixelRect) -> InkOverflow {
    let mut ink = InkOverflow {
        rect: border_box,
        unbounded: css_rect_is_saturated(border_box),
        ..Default::default()
    };
    let Some(style) = source.node_style_if_live(node) else {
        return ink;
    };
    ink.unbounded |= border_shape::has_border_shape(style);
    for layer in style
        .effects()
        .box_shadows
        .as_slice()
        .iter()
        .filter(|layer| !layer.is_inner())
    {
        let geometry = shadow::box_shadow_layer_geometry(layer, border_box, border_box);
        let spread = geometry.spread_distance.max(CssPixels::default());
        let reach = if CornerShapes::of(style).0 != [1.0; 4] {
            // Bound the full spread stroke at its miter limit and the full blur kernel. Their
            // independently quantized device widths can exceed the nominal CSS half-width bound.
            spread * 8 + geometry.blur_radius * 4
        } else {
            spread * 2 + geometry.blur_radius * 4
        };
        ink.rect
            .unite(inflated(geometry.content_rect, reach).translated(geometry.offset_x, geometry.offset_y));
    }
    let border_reach = [
        (style.border_top_style(), style.border_top_width()),
        (style.border_right_style(), style.border_right_width()),
        (style.border_bottom_style(), style.border_bottom_width()),
        (style.border_left_style(), style.border_left_width()),
    ]
    .into_iter()
    .filter(|(style, _)| matches!(*style, line_style::DASHED | line_style::DOTTED))
    .map(|(_, width)| width * 2)
    .max()
    .unwrap_or_default();
    ink.rect.unite(inflated(border_box, border_reach));
    if border_image::paints_border_image(style) {
        let [top, right, bottom, left] = border_image::border_image_outsets(
            style,
            [
                style.border_top_width(),
                style.border_right_width(),
                style.border_bottom_width(),
                style.border_left_width(),
            ],
            crate::painting::record::paint::border::ALL_PIECE_EDGES,
        );
        ink.rect.unite(border_box.inflated(top, right, bottom, left));
    }
    let outline_width = style_queries::outline_geometry(style)
        .map_or(CssPixels::default(), |geometry| geometry.width)
        .max(style_queries::auto_outline_width());
    let (outline_rect, _) = outline::outline_border_geometry(
        outline_width,
        style_queries::outline_offset(source, node),
        border_box,
        BorderRadii::default(),
    );
    ink.rect.unite(inflated(outline_rect, outline_width * 2));
    ink.unbounded |= css_rect_is_saturated(ink.rect);
    ink
}

/// Everything the producers owned by this row can paint, before effects, in absolute layout CSS pixels.
pub(crate) fn own_ink_overflow(
    source: &impl PaintRead,
    paint_state: &PublishedPaintState,
    owner: NodeSlotId,
) -> InkOverflow {
    let mut ink = InkOverflow::default();
    for border_box in paintable_geometry::absolute_border_box_rects_of_fragments(source, owner) {
        let fragment_ink = box_decoration_ink(source, owner, border_box);
        ink.rect.unite(fragment_ink.rect);
        ink.unbounded |= fragment_ink.unbounded;
    }
    ink.unbounded |= style_queries::node_is_root_element(source, owner);
    if source.node_kind_if_live(owner) == Some(NodeKind::SVGSVGBox) {
        ink.unbounded |= source
            .node_style_if_live(owner)
            .is_some_and(|style| style.overflow_x() == overflow::VISIBLE || style.overflow_y() == overflow::VISIBLE);
        return ink;
    }
    if table_backgrounds::paints_background_in_cells(style_queries::display(source, owner)) {
        for cell in table_backgrounds::originating_cells(source, owner) {
            ink.rect
                .unite(paintable_geometry::absolute_border_box_rect(source, cell));
        }
    }
    if let Some((bounds, _, _)) = table_borders::collapsed_table_borders_bounds_and_grid_lines(source, owner) {
        ink.rect.unite(bounds);
    }
    if let Some(style) = source.node_style_if_live(owner) {
        if column_rule::has_visible_column_rule(style) {
            for rule in column_rule::column_rule_rects(source, owner, style) {
                ink.rect.unite(inflated(rule, style.column_rule_width() * 2));
            }
        }
        if style_queries::is_scroll_container(source, owner) || chrome_geometry::has_resizer(source, owner) {
            ink.rect.unite(inflated(
                paintable_geometry::absolute_padding_box_rect(source, owner),
                CssPixels::from_integer(32),
            ));
        }
    }
    if let Some(math) = paintable_geometry::committed_math_paint_data(source, owner) {
        let origin = paintable_geometry::absolute_rect(source, owner).location();
        for rect in &math.rules {
            ink.rect.unite(rect.translated_by(origin));
        }
        let glyph_stroke_reach = match source.node_style_if_live(owner) {
            Some(style) if math.replaces_text => text_stroke_reach(style),
            _ => CssPixels::default(),
        };
        for run in &math.glyph_runs {
            ink.rect
                .unite(inflated(run.ink_bounds.translated_by(origin), glyph_stroke_reach));
        }
    }
    let root = if node_painting::has_lines(source, owner) {
        Some(owner)
    } else {
        source.inline_pieces_root(owner)
    };
    if let Some(root) = root {
        let side = source.committed_side_data(root);
        let positions = paintable_geometry::LineRootPositions::new(source, root);
        let mut include_piece = |piece: &crate::painting::paintable_data::InlineBoxPieceRecord| {
            // NB: A box leaving the line root resets its row before the root lays out its lines again, and resetting
            //     the row asks for the root's ink overflow, so the pieces can name a box that paints nothing now.
            if !source.paintable_row_is_populated(piece.node) {
                return;
            }
            let piece_owner = if fragment_ownership::is_self_painting_inline(source, piece.node) {
                piece.node
            } else {
                fragment_ownership::nearest_self_painting_inline_box(source, piece.node).unwrap_or(root)
            };
            if piece.node != owner && piece_owner != owner {
                return;
            }
            let border_box = CssPixelRect::from(piece.border_box_rect)
                .translated_by(positions.of_line(side.lines(), piece.line_index as usize));
            let piece_ink = box_decoration_ink(source, piece.node, border_box);
            ink.rect.unite(piece_ink.rect);
            if paints_backdrop_filter(source, piece.node) {
                ink.rect
                    .unite(paintable_geometry::absolute_border_box_rect(source, piece.node));
            }
            ink.unbounded |= piece_ink.unbounded;
        };
        if owner == root || fragment_ownership::is_self_painting_inline(source, owner) {
            for piece in side.inline_box_pieces() {
                include_piece(piece);
            }
        } else {
            for &index in side.inline_box_piece_indices(owner) {
                include_piece(&side.inline_box_pieces()[index as usize]);
            }
        }
        let filter = fragment_ownership::effective_filter(source, owner);
        let owns_every_fragment = owner == root && filter.include_everything && filter.excluded.is_empty();
        let mut text_ink = CssPixelRect::default();
        let mut block_chain_has_text_decoration_lines = None;
        if owns_every_fragment {
            for (line_index, line_ink) in side.glyph_ink_of_lines().iter().enumerate() {
                if !line_ink.is_empty() {
                    text_ink.unite(line_ink.translated_by(positions.of_line(side.lines(), line_index)));
                }
            }
            for (index, fragment) in side.fragments().iter().enumerate() {
                if fragment.glyph_run.is_none() {
                    continue;
                }
                let rect = text_fragment::rect_in_containing_block(fragment)
                    .translated_by(positions.of_line(side.lines(), fragment.line_index as usize));
                include_text_paint_effects(
                    source,
                    paint_state,
                    root,
                    index as u32,
                    rect,
                    &mut block_chain_has_text_decoration_lines,
                    &mut text_ink,
                );
            }
            if crate::painting::record::ink_bounds::verification().enabled {
                let mut text_ink_of_each_fragment = CssPixelRect::default();
                for index in 0..side.fragments().len() {
                    include_text_fragment(
                        source,
                        paint_state,
                        root,
                        index as u32,
                        &mut None,
                        &mut text_ink_of_each_fragment,
                    );
                }
                assert!(
                    text_ink == text_ink_of_each_fragment
                        || (text_ink.is_empty() && text_ink_of_each_fragment.is_empty())
                        || (css_rect_is_saturated(text_ink) && css_rect_is_saturated(text_ink_of_each_fragment)),
                    "The glyph ink kept with the lines of {root:?} is stale: {text_ink:?}, each fragment gives {text_ink_of_each_fragment:?}"
                );
            }
        } else {
            filter.for_each_owned_fragment_index(side.fragments().len(), |index| {
                include_text_fragment(
                    source,
                    paint_state,
                    root,
                    index as u32,
                    &mut block_chain_has_text_decoration_lines,
                    &mut text_ink,
                );
            });
        }
        ink.rect.unite(text_ink);
    }
    ink.unbounded |= css_rect_is_saturated(ink.rect);
    ink
}

fn highlight_style<'a>(
    source: &impl PaintRead,
    styles: &'a crate::painting::paint_state::SelectionPseudoStyles,
    node: NodeSlotId,
) -> Option<&'a text::SelectionStyleAnswer> {
    if let Some(answer) = styles.get(&node) {
        return Some(answer);
    }
    let mut element = source.node_parent_if_live(node)?;
    while source.node_flags_if_live(element) & NodeFlag::Anonymous as u32 != 0 {
        element = source.node_parent_if_live(element)?;
    }
    if let Some(answer) = styles.get(&element) {
        return Some(answer);
    }
    if source.node_flags_if_live(element) & NodeFlag::IsInUserAgentShadowTree as u32 == 0 {
        return None;
    }
    while source.node_flags_if_live(element) & NodeFlag::IsInUserAgentShadowTree as u32 != 0 {
        element = source.node_parent_if_live(element)?;
    }
    styles.get(&element).map(|answer| answer.as_ref())
}

/// What text fragment `index` of the line root `root` draws, in absolute layout CSS pixels, in the space the root
/// paints its lines in.
pub(crate) fn text_fragment_ink_rect(
    source: &impl PaintRead,
    state: &PublishedPaintState,
    root: NodeSlotId,
    index: u32,
) -> CssPixelRect {
    let mut rect = CssPixelRect::default();
    include_text_fragment(source, state, root, index, &mut None, &mut rect);
    rect
}

/// What inline box piece `index` of the line root `root` draws, where `positions` places the root's lines, in absolute
/// layout CSS pixels. A piece can record the backdrop filter region of its whole inline.
pub(crate) fn inline_piece_ink(
    source: &impl PaintRead,
    root: NodeSlotId,
    positions: &paintable_geometry::LineRootPositions,
    index: u32,
) -> InkOverflow {
    let side = source.committed_side_data(root);
    let piece = &side.inline_box_pieces()[index as usize];
    let border_box = CssPixelRect::from(piece.border_box_rect)
        .translated_by(positions.of_line(side.lines(), piece.line_index as usize));
    let mut ink = box_decoration_ink(source, piece.node, border_box);
    if paints_backdrop_filter(source, piece.node) {
        ink.rect
            .unite(paintable_geometry::absolute_border_box_rect(source, piece.node));
    }
    ink
}

/// Whether each piece of the inline box `node` records a backdrop filter region covering the whole inline.
fn paints_backdrop_filter(source: &impl PaintRead, node: NodeSlotId) -> bool {
    source
        .node_style_if_live(node)
        .is_some_and(|style| style.effects().backdrop_filter.operations.length != 0)
}

fn include_text_fragment(
    source: &impl PaintRead,
    state: &PublishedPaintState,
    root: NodeSlotId,
    index: u32,
    block_chain_has_text_decoration_lines: &mut Option<bool>,
    bounds: &mut CssPixelRect,
) {
    let side = source.committed_side_data(root);
    let fragment = &side.fragments()[index as usize];
    if fragment.glyph_run.is_none() {
        return;
    }
    let rect = text_fragment::absolute_rect(source, fragment);
    let rect_in_containing_block = text_fragment::rect_in_containing_block(fragment);
    bounds.unite(
        text_fragment::glyph_ink_rect_in_containing_block(fragment)
            .translated(rect.x - rect_in_containing_block.x, rect.y - rect_in_containing_block.y),
    );
    include_text_paint_effects(
        source,
        state,
        root,
        index,
        rect,
        block_chain_has_text_decoration_lines,
        bounds,
    );
}

fn include_text_paint_effects(
    source: &impl PaintRead,
    state: &PublishedPaintState,
    root: NodeSlotId,
    index: u32,
    rect: CssPixelRect,
    block_chain_has_text_decoration_lines: &mut Option<bool>,
    bounds: &mut CssPixelRect,
) {
    let side = source.committed_side_data(root);
    let fragment = &side.fragments()[index as usize];
    let Some(run) = &fragment.glyph_run else {
        return;
    };
    let Some(style) = source.node_style_if_live(fragment.style_source) else {
        return;
    };
    let stroke_reach = text_stroke_reach(style);
    if stroke_reach > CssPixels::default() {
        bounds.unite(inflated(
            text_fragment::glyph_rect_of_fragment_at(fragment, run, rect),
            stroke_reach,
        ));
    }
    for shadow in style.inherited_text().text_shadow.as_slice() {
        let reach = CssPixels::from_raw(shadow.blur_radius) * 2;
        bounds.unite(inflated(rect, reach).translated(
            CssPixels::from_raw(shadow.offset_x),
            CssPixels::from_raw(shadow.offset_y),
        ));
    }
    let mut highlights = Vec::new();
    for styles in [
        &state.selection_pseudo_styles,
        &state.search_text_pseudo_styles,
        &state.search_text_current_pseudo_styles,
        &state.spelling_error_pseudo_styles,
    ]
    .into_iter()
    .chain(state.custom_highlight_pseudo_styles.values())
    {
        if styles.is_empty() {
            continue;
        }
        let Some(answer) = highlight_style(source, styles, fragment.layout_node) else {
            continue;
        };
        for shadow in &answer.shadows {
            bounds.unite(
                inflated(rect, shadow.layer.blur_radius * 2).translated(shadow.layer.offset_x, shadow.layer.offset_y),
            );
        }
        if answer.facts.has_text_decoration {
            highlights.push(text::HighlightOverlay {
                highlight: text::HighlightLayer::TextFragmentIndication,
                background_color: 0,
                shadow_layers: Vec::new(),
                text_decoration: Some(text::SpanTextDecoration {
                    lines: answer.facts.text_decoration_lines,
                    line_count: answer.facts.text_decoration_line_count,
                    style: answer.facts.text_decoration_style,
                    color: 0,
                    metrics: answer.decoration_metrics,
                }),
            });
        }
    }
    if state.spelling_errors.by_node.contains_key(&fragment.layout_node) {
        highlights.push(text::HighlightOverlay {
            highlight: text::HighlightLayer::TextFragmentIndication,
            background_color: 0,
            shadow_layers: Vec::new(),
            text_decoration: Some(text::SpanTextDecoration {
                lines: [text_decoration_line::SPELLING_ERROR, 0, 0, 0, 0, 0, 0, 0],
                line_count: 1,
                style: text_decoration_style::WAVY,
                color: 0,
                metrics: None,
            }),
        });
    }
    if highlights.is_empty()
        && !text_decoration::fragment_may_paint_text_decorations(
            source,
            root,
            fragment,
            block_chain_has_text_decoration_lines,
        )
    {
        return;
    }
    let span = text::RenderSpan {
        fragment_index: index,
        start_code_unit: 0,
        end_code_unit: fragment.length_in_code_units,
        text_color: style.current_color(),
        shadow_layers: Vec::new(),
        highlights,
        decoration_color: None,
    };
    for set in text_decoration::decoration_sets_for_span(source, root, &span) {
        bounds.unite(text_decoration::decoration_ink_bounds(fragment, rect, &set));
    }
}

fn has_resampled_ink(style: crate::layout::ComputedValuesView<'_>) -> bool {
    use crate::css::property_metadata::property_id;
    style
        .misc_interaction()
        .implicit_will_change
        .as_slice()
        .iter()
        .any(|property| {
            matches!(
                *property,
                property_id::TRANSFORM
                    | property_id::TRANSLATE
                    | property_id::ROTATE
                    | property_id::SCALE
                    | property_id::OFFSET_PATH
                    | property_id::OFFSET_DISTANCE
                    | property_id::OFFSET_POSITION
                    | property_id::OFFSET_ANCHOR
                    | property_id::OFFSET_ROTATE
                    | property_id::TRANSFORM_ORIGIN
                    | property_id::PERSPECTIVE
                    | property_id::PERSPECTIVE_ORIGIN
                    | property_id::FILTER
            )
        })
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum HitTestBoundOfLine {
    RectInContentBoxOfLineRoot(CssPixelRect),
    Unbounded,
}

pub(crate) fn hit_test_bounds_of_lines(
    source: &impl PaintRead,
    line_root: NodeSlotId,
) -> Option<std::sync::Arc<[HitTestBoundOfLine]>> {
    use crate::painting::paintable_data::PaintableFlag;
    if !node_painting::has_lines(source, line_root)
        || source
            .paintable_data(line_root)
            .has_flag(PaintableFlag::HasLaterBoxFragments)
    {
        return None;
    }
    let side = source.committed_side_data(line_root);
    if side.lines().len() < 2 {
        return None;
    }
    let content_origin = paintable_geometry::absolute_rect(source, line_root).location();
    let mut bounds = vec![HitTestBoundOfLine::RectInContentBoxOfLineRoot(CssPixelRect::default()); side.lines().len()];
    for fragment in side.fragments() {
        let bound = bounds.get_mut(fragment.line_index as usize)?;
        let HitTestBoundOfLine::RectInContentBoxOfLineRoot(rect) = bound else {
            continue;
        };
        if !fragment.is_atomic_inline {
            rect.unite(text_fragment::rect_in_containing_block(fragment));
            continue;
        }
        let atomic_inline = fragment.layout_node;
        if !source.paintable_row_is_populated(atomic_inline) {
            continue;
        }
        let ink = source.stored_ink_overflow(atomic_inline);
        if ink.unbounded || ink.contribution_unbounded {
            *bound = HitTestBoundOfLine::Unbounded;
            continue;
        }
        let padding_box_origin = paintable_geometry::first_fragment_padding_box_rect(source, atomic_inline).location();
        rect.unite(
            ink.rect
                .translated_by(padding_box_origin)
                .translated(-content_origin.x, -content_origin.y),
        );
    }
    Some(bounds.into())
}

/// What bounds everything a paint scope paints.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) enum ScopeInkBound {
    /// In absolute layout CSS pixels, in the space the owner's own box is painted in.
    Rect(CssPixelRect),
    /// The owner covers the scope, and where the scope paints cannot be told from a rect in the owner's space.
    Unbounded,
    /// The scope paints boxes the owner's ink overflow does not gather: whoever walks it has to descend.
    NotCovering,
}

pub(crate) fn scope_ink_bound(source: &impl PaintRead, scope: PaintScope) -> ScopeInkBound {
    if !source.paintable_row_is_populated(scope.owner) || !scope.paints_only_what_its_owner_contains(source) {
        return ScopeInkBound::NotCovering;
    }
    let ink = source.stored_ink_overflow(scope.owner);
    // What makes a box's contribution to its containing block unbounded also moves or spreads what the box's own
    // scope paints away from the rect: a sticky or animated offset, a filter, later fragments. The viewport's
    // content is cut to it by the window rather than by a clip of the tree.
    if ink.unbounded || ink.contribution_unbounded || source.node_kind_if_live(scope.owner) == Some(NodeKind::Viewport)
    {
        return ScopeInkBound::Unbounded;
    }
    source
        .absolute_ink_overflow_rect(scope.owner)
        .map_or(ScopeInkBound::Unbounded, ScopeInkBound::Rect)
}

fn include_child(ink: &mut InkOverflow, child: InkOverflow) {
    ink.rect.unite(child.rect);
    ink.unbounded |= child.contribution_unbounded;
    ink.holds_what_a_transform_moves |= child.holds_what_a_transform_moves;
}

/// Cuts the content a box gathered down to what its overflow clip keeps. The box's own ink is not content.
fn clip_content_to_overflow_clip(ink: &mut InkOverflow, own: InkOverflow, clip: &OverflowClipRanges) {
    let (left, right) = clip.horizontal.unwrap_or((ink.rect.x, ink.rect.right()));
    let (top, bottom) = clip.vertical.unwrap_or((ink.rect.y, ink.rect.bottom()));
    ink.rect = own.rect;
    ink.rect.unite(CssPixelRect::new(left, top, right - left, bottom - top));
    if clip.horizontal.is_some() && clip.vertical.is_some() {
        // Whatever made a child's contribution unbounded is cut off with the rest of it, and so is whatever a
        // transform moves.
        ink.unbounded = own.unbounded;
        ink.holds_what_a_transform_moves = own.holds_what_a_transform_moves;
    }
}

fn mapped_ink(ink: InkOverflow, transform: &crate::painting::visual_context::TransformData) -> InkOverflow {
    if !transform.matrix.is_2d_affine() {
        return InkOverflow {
            contribution_unbounded: true,
            ..ink
        };
    }
    let rect = libgfx_rust::FloatRect::new(
        ink.rect.x.to_float(),
        ink.rect.y.to_float(),
        ink.rect.width.to_float(),
        ink.rect.height.to_float(),
    );
    let mapped = transform
        .matrix
        .extract_2d_affine()
        .map_rect(rect.translated(-transform.origin.x, -transform.origin.y))
        .translated(transform.origin.x, transform.origin.y);
    let coordinates = [mapped.x, mapped.y, mapped.x + mapped.width, mapped.y + mapped.height];
    if coordinates.iter().any(|coordinate| !coordinate.is_finite()) {
        return InkOverflow {
            contribution_unbounded: true,
            ..ink
        };
    }
    let [left, top, right, bottom] = coordinates.map(f64::from);
    let pixel = CssPixels::nearest_value_for;
    let rect = CssPixelRect::new(
        pixel(left.floor()),
        pixel(top.floor()),
        pixel(right.ceil() - left.floor()),
        pixel(bottom.ceil() - top.floor()),
    );
    InkOverflow {
        rect,
        contribution_unbounded: ink.contribution_unbounded || css_rect_is_saturated(rect),
        ..ink
    }
}

fn recalculated_ink_overflow(
    source: &impl crate::painting::paintable_rows::PaintableRowsRead,
    state: &PublishedPaintState,
    node: NodeSlotId,
) -> InkOverflow {
    use crate::css::css_enums::positioning;
    use crate::painting::paintable_data::PaintableFlag;
    let own = own_ink_overflow(source, state, node);
    let mut ink = own;
    let padding = paintable_geometry::first_fragment_padding_box_rect(source, node);
    if let Some(link) = source.with_committed_fragment_link(node, |link| link.cloned()) {
        let later_fragments = source.committed_side_data(node);
        let fragments =
            std::iter::once(&link).chain(later_fragments.box_fragments().iter().map(|fragment| &fragment.link));
        for (fragment_index, link) in fragments.enumerate() {
            for child_link in source.links_in_containing_block_fragment(node, link, fragment_index as u32) {
                let child = child_link.fragment.node;
                let child_ink = source.stored_ink_overflow(child);
                let child_padding = paintable_geometry::first_fragment_padding_box_rect(source, child);
                let mut contribution_relative_to_padding_box = InkOverflow {
                    rect: child_ink
                        .rect
                        .translated(child_padding.x - padding.x, child_padding.y - padding.y),
                    ..child_ink
                };
                if let Some((mut transform, _)) =
                    crate::painting::visual_context::node_values::compute_transform(source, child, 1.0)
                {
                    // NB: Round out relative to the padding box, as moving the box with its descendants keeps the ink.
                    transform.origin.x -= padding.x.to_float();
                    transform.origin.y -= padding.y.to_float();
                    contribution_relative_to_padding_box = mapped_ink(contribution_relative_to_padding_box, &transform);
                    contribution_relative_to_padding_box.holds_what_a_transform_moves = true;
                }
                include_child(
                    &mut ink,
                    InkOverflow {
                        rect: contribution_relative_to_padding_box
                            .rect
                            .translated(padding.x, padding.y),
                        ..contribution_relative_to_padding_box
                    },
                );
            }
        }
    }
    if let Some(clip) = overflow_clip_ranges(source, node) {
        clip_content_to_overflow_clip(&mut ink, own, &clip);
    }
    if let Some(style) = source.node_style_if_live(node) {
        ink.contribution_unbounded |= style.position() == positioning::STICKY
            || style.transform().has_perspective
            || crate::painting::css_filter::can_spread(&style.effects().filter)
            || has_resampled_ink(style);
    }
    ink.contribution_unbounded |= source
        .paintable_data(node)
        .has_flag(PaintableFlag::HasLaterBoxFragments)
        || !source.default_scroll_shift_anchor(node).is_invalid();
    ink.contribution_unbounded |= crate::painting::visual_context::node_values::compute_transform(source, node, 1.0)
        .is_some_and(|(transform, _)| !transform.matrix.is_2d_affine());
    ink.unbounded |= css_rect_is_saturated(ink.rect);
    ink.contribution_unbounded |= ink.unbounded;
    ink.rect = ink.rect.translated(-padding.x, -padding.y);
    ink.unbounded |= css_rect_is_saturated(ink.rect);
    ink.contribution_unbounded |= ink.unbounded;
    ink
}

impl crate::painting::paintable_rows::PaintableRowStore {
    /// Whether the row had no recalculation pending until now.
    fn mark_ink_overflow_dirty(&mut self, node: NodeSlotId) -> bool {
        let index = node.slot_index() as usize;
        if self.ink_overflow_dirty.len() <= index {
            self.ink_overflow_dirty.resize_with_default(index + 1);
        }
        !std::mem::replace(&mut self.ink_overflow_dirty[index], true)
    }
}

struct StoredInkOverflowAuditCounts {
    rows: usize,
    checked: usize,
    unbounded: usize,
    contribution_unbounded: usize,
}

impl crate::layout::LayoutNodeArena {
    pub(crate) fn request_ink_overflow_recalculation(&mut self, node: NodeSlotId) {
        if !self.paintable_row_is_populated(node) {
            return;
        }
        if self.paintable_rows.mark_ink_overflow_dirty(node) {
            self.paintable_rows.ink_overflow_requests.push(node);
        }
    }

    pub(crate) fn request_containing_block_ink_overflow_recalculation(&mut self, node: NodeSlotId) {
        if self.paintable_row_is_populated(node) {
            let block = self.live_paintable_data(node).containing_block;
            self.request_ink_overflow_recalculation(block);
        }
    }

    pub(crate) fn request_inline_ink_overflow_recalculation(&mut self, node: NodeSlotId) {
        let rows = self.paintable_rows();
        let root = if node_painting::is_fragmented_inline(&rows, node) {
            rows.inline_pieces_root(node)
        } else {
            None
        };
        if let Some(root) = root {
            self.request_ink_overflow_recalculation(root);
        }
    }

    /// Takes the row's contribution away from whatever gathered it. The row must still name its containing block.
    pub(crate) fn forget_ink_overflow_of_row_being_reset(&mut self, node: NodeSlotId) {
        if !self.paintable_row_is_populated(node) {
            return;
        }
        self.request_containing_block_ink_overflow_recalculation(node);
        self.request_inline_ink_overflow_recalculation(node);
        let index = node.slot_index() as usize;
        let store = &mut self.paintable_rows;
        if let Some(dirty) = store.ink_overflow_dirty.get_mut(index) {
            *dirty = false;
        }
        if store
            .ink_overflow
            .get(index)
            .is_some_and(|ink| *ink != InkOverflow::default())
        {
            store.ink_overflow.set(index, InkOverflow::default());
            store.ink_overflow_stamp = crate::cow_column::new_edit_token();
        }
    }

    pub(crate) fn ink_overflow_is_settled(&self) -> bool {
        self.paintable_rows.ink_overflow_requests.is_empty()
    }

    pub(crate) fn settle_ink_overflow(&mut self) {
        // A relayout leaves the boxes of a line without their share of its fragments until it is assigned again,
        // and what a box paints follows its share.
        crate::painting::fragment_ownership::assign_fragment_ownership_for_pending_line_roots(self);
        let verification = crate::painting::record::ink_bounds::verification();
        if self.paintable_rows.ink_overflow_requests.is_empty() && !verification.enabled {
            return;
        }
        let start = verification.report.then(std::time::Instant::now);
        let state = PublishedPaintState::new(self.paint_state());
        let recalculated = self.recalculate_requested_ink_overflow(&state);
        if !verification.enabled {
            return;
        }
        let settle_elapsed = start.map(|start| start.elapsed());
        let counts = self.verify_stored_ink_overflow(&state);
        if verification.counts
            && let Some(start) = start
            && let Some(settle_elapsed) = settle_elapsed
        {
            let total_elapsed = start.elapsed();
            eprintln!(
                "Paint ink overflow audit: rows={} checked={} unbounded={} contribution-unbounded={} recalculated={recalculated} settle-us={} audit-us={} size={}",
                counts.rows,
                counts.checked,
                counts.unbounded,
                counts.contribution_unbounded,
                settle_elapsed.as_micros(),
                (total_elapsed - settle_elapsed).as_micros(),
                std::mem::size_of::<InkOverflow>()
            );
        }
    }

    /// Recalculates the requested rows, descendants before the containing blocks that gather them, and follows
    /// every changed value up to its containing block. Returns how many rows it recalculated.
    fn recalculate_requested_ink_overflow(&mut self, state: &PublishedPaintState) -> usize {
        let mut requests = std::mem::take(&mut self.paintable_rows.ink_overflow_requests);
        requests.retain(|node| self.slot_is_live(*node) && self.paintable_row_is_populated(*node));
        requests.sort_unstable_by_key(|node| self.node_pre_order_label(*node));
        let mut source = self.paintable_rows_mut();
        // Warm ancestors before descendants: a first layout requests every row, and resolving
        // absolute geometry in the reverse order would repeatedly walk their containing blocks.
        for &node in &requests {
            paintable_geometry::absolute_rect(&source, node);
        }
        let mut queue: std::collections::BinaryHeap<_> = requests
            .into_iter()
            .map(|node| (source.node_pre_order_label(node), node.index))
            .collect();
        let mut recalculated = 0;
        while let Some((_, index)) = queue.pop() {
            let node = NodeSlotId { index };
            let index = node.slot_index() as usize;
            source.paintable_rows.ink_overflow_dirty[index] = false;
            let ink = recalculated_ink_overflow(&source, state, node);
            recalculated += 1;
            let hit_test_bounds_of_lines = hit_test_bounds_of_lines(&source, node);
            if hit_test_bounds_of_lines.is_some() || source.committed_side_data(node).hit_test_bounds_of_lines.is_some()
            {
                source.committed_side_data_mut(node).hit_test_bounds_of_lines = hit_test_bounds_of_lines;
            }
            let store = &mut source.paintable_rows;
            if store.ink_overflow.get(index).is_some_and(|previous| *previous == ink) {
                continue;
            }
            store.ink_overflow.grow_to(index + 1);
            store.ink_overflow.set(index, ink);
            store.ink_overflow_stamp = crate::cow_column::new_edit_token();
            let block = source.paintable_data(node).containing_block;
            if source.slot_is_live(block)
                && source.paintable_row_is_populated(block)
                && source.paintable_rows.mark_ink_overflow_dirty(block)
            {
                queue.push((source.node_pre_order_label(block), block.index));
            }
        }
        recalculated
    }

    /// Recalculates rows from nothing and requires what is stored to equal the result, which is what catches a
    /// write that should have requested a row and did not. A large document is sampled, elsewhere at every call.
    fn verify_stored_ink_overflow(&mut self, state: &PublishedPaintState) -> StoredInkOverflowAuditCounts {
        let audit_sequence = self.paintable_rows.ink_overflow_audit_sequence.wrapping_add(1);
        self.paintable_rows.ink_overflow_audit_sequence = audit_sequence;
        let source = self.paintable_rows();
        let nodes: Vec<_> = (0..source.slot_capacity())
            .filter_map(|index| source.live_slot_at(index as u32))
            .filter(|node| source.paintable_row_is_populated(*node))
            .collect();
        let mut counts = StoredInkOverflowAuditCounts {
            rows: nodes.len(),
            checked: 0,
            unbounded: 0,
            contribution_unbounded: 0,
        };
        for (index, node) in nodes.iter().copied().enumerate() {
            let stored = source.stored_ink_overflow(node);
            counts.unbounded += usize::from(stored.unbounded);
            counts.contribution_unbounded += usize::from(stored.contribution_unbounded);
            if nodes.len() > 4096 && !(index as u64 + audit_sequence).is_multiple_of(64) {
                continue;
            }
            let recomputed = recalculated_ink_overflow(&source, state, node);
            assert!(
                stored == recomputed,
                "Stored ink overflow is stale: {:?} {node:?}, stored {stored:?}, recomputed {recomputed:?}",
                source.node_kind_if_live(node)
            );
            let stored_hit_test_bounds_of_lines = source.committed_side_data(node).hit_test_bounds_of_lines.clone();
            let recomputed_hit_test_bounds_of_lines = hit_test_bounds_of_lines(&source, node);
            assert!(
                stored_hit_test_bounds_of_lines == recomputed_hit_test_bounds_of_lines,
                "Stored hit test bounds of the lines of {node:?} are stale: stored {stored_hit_test_bounds_of_lines:?}, recomputed {recomputed_hit_test_bounds_of_lines:?}"
            );
            counts.checked += 1;
        }
        counts
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::LayoutNodeArena;
    use crate::layout::fragment_tree::FragmentLink;
    use std::sync::Arc;

    fn rect(x: i64, y: i64, width: i64, height: i64) -> CssPixelRect {
        CssPixelRect::new(
            CssPixels::from_integer(x),
            CssPixels::from_integer(y),
            CssPixels::from_integer(width),
            CssPixels::from_integer(height),
        )
    }

    #[test]
    fn negative_children_and_unbounded_contributions_reach_the_container() {
        let mut ink = InkOverflow {
            rect: rect(0, 0, 100, 100),
            ..Default::default()
        };
        include_child(
            &mut ink,
            InkOverflow {
                rect: rect(-200, -300, 20, 30),
                ..Default::default()
            },
        );
        assert_eq!(ink.rect, rect(-200, -300, 300, 400));
        include_child(
            &mut ink,
            InkOverflow {
                contribution_unbounded: true,
                ..Default::default()
            },
        );
        assert!(ink.unbounded);
    }

    #[test]
    fn content_clip_keeps_own_ink_and_the_clipped_range_on_each_clipped_axis() {
        let own = InkOverflow {
            rect: rect(-10, -20, 120, 140),
            ..Default::default()
        };
        let content = InkOverflow {
            rect: rect(-300, -400, 900, 1000),
            unbounded: true,
            ..Default::default()
        };
        let pixels = CssPixels::from_integer;
        let mut ink = content;
        clip_content_to_overflow_clip(
            &mut ink,
            own,
            &OverflowClipRanges {
                horizontal: Some((pixels(0), pixels(100))),
                vertical: None,
            },
        );
        assert_eq!(ink.rect, rect(-10, -400, 120, 1000));
        assert!(ink.unbounded);
        let mut ink = content;
        clip_content_to_overflow_clip(
            &mut ink,
            own,
            &OverflowClipRanges {
                horizontal: Some((pixels(0), pixels(100))),
                vertical: Some((pixels(-50), pixels(150))),
            },
        );
        assert_eq!(ink.rect, rect(-10, -50, 120, 200));
        assert!(!ink.unbounded);
    }

    #[test]
    fn what_a_transform_moves_is_remembered_up_to_a_box_that_cuts_it_off() {
        let mut ink = InkOverflow {
            rect: rect(0, 0, 100, 100),
            ..Default::default()
        };
        include_child(
            &mut ink,
            InkOverflow {
                rect: rect(10, 10, 20, 20),
                holds_what_a_transform_moves: true,
                ..Default::default()
            },
        );
        assert!(ink.holds_what_a_transform_moves);
        let pixels = CssPixels::from_integer;
        let own = InkOverflow {
            rect: rect(0, 0, 100, 100),
            ..Default::default()
        };
        let mut cut_on_one_axis = ink;
        clip_content_to_overflow_clip(
            &mut cut_on_one_axis,
            own,
            &OverflowClipRanges {
                horizontal: Some((pixels(0), pixels(100))),
                vertical: None,
            },
        );
        assert!(cut_on_one_axis.holds_what_a_transform_moves);
        clip_content_to_overflow_clip(
            &mut ink,
            own,
            &OverflowClipRanges {
                horizontal: Some((pixels(0), pixels(100))),
                vertical: Some((pixels(0), pixels(100))),
            },
        );
        assert!(!ink.holds_what_a_transform_moves);
    }

    #[test]
    fn affine_transforms_enclose_ink_and_non_affine_transforms_are_unbounded() {
        use crate::painting::visual_context::{TransformData, TransformDataRole};
        let mut transform = TransformData {
            matrix: libgfx_rust::affine_to_matrix(libgfx_rust::AffineTransform::new(0.0, 1.0, -1.0, 0.0, 0.0, 0.0)),
            origin: libgfx_rust::FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: true,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: libgfx_rust::FloatRect::default(),
        };
        let ink = InkOverflow {
            rect: rect(10, 20, 30, 40),
            ..Default::default()
        };
        let mapped = mapped_ink(ink, &transform);
        assert_eq!(mapped.rect, rect(-60, 10, 40, 30));
        assert!(!mapped.contribution_unbounded);
        transform.matrix = libgfx_rust::perspective_matrix(100.0);
        assert!(mapped_ink(ink, &transform).contribution_unbounded);
    }

    fn childless_link(node: NodeSlotId, block: NodeSlotId, bounds: CssPixelRect) -> FragmentLink {
        let mut link = FragmentLink::for_test(node);
        link.placement.containing_block = block;
        link.offset = bounds.location().into();
        let fragment = Arc::get_mut(&mut link.fragment).unwrap();
        fragment.sides.content_size.width = bounds.width;
        fragment.sides.content_size.height = bounds.height;
        link
    }

    fn box_link(
        arena: &mut LayoutNodeArena,
        node: NodeSlotId,
        block: NodeSlotId,
        bounds: CssPixelRect,
    ) -> FragmentLink {
        arena.write_shape(node).set_kind(NodeKind::BlockContainer);
        arena.populate_paintable_row(node);
        arena.paintable_rows_mut().paintable_data_mut(node).containing_block = block;
        let link = childless_link(node, block, bounds);
        arena.set_committed_fragment_link(node, link.clone(), None);
        link
    }

    #[test]
    fn containing_block_aggregation_survives_moves_and_published_rows_are_frozen() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        arena.insert_child(root, child, NodeSlotId::INVALID);
        let mut root_link = box_link(&mut arena, root, NodeSlotId::INVALID, rect(0, 0, 100, 100));
        let child_link = box_link(&mut arena, child, root, rect(-200, 300, 20, 20));
        arena
            .paintable_rows
            .take_committed_fragment_link(root.slot_index(), root.generation());
        Arc::get_mut(&mut root_link.fragment)
            .unwrap()
            .children
            .push(child_link.clone());
        arena.set_committed_fragment_link(root, root_link, None);
        arena.settle_ink_overflow();
        assert_eq!(arena.stored_ink_overflow(root).rect, rect(-200, 0, 300, 320));
        let stamp = arena.ink_overflow_stamp();
        let published = arena.publish_rows();
        let mut moved_child = child_link;
        moved_child.offset.x = CssPixels::from_integer(-400);
        arena.set_committed_fragment_link(child, moved_child, None);
        arena.clear_absolute_rect_memo();
        arena.note_paint_change(child, crate::painting::record::change_ticks::RowPaintChange::PLACE);
        arena.settle_ink_overflow();
        assert_eq!(arena.stored_ink_overflow(root).rect, rect(-400, 0, 500, 320));
        assert_ne!(arena.ink_overflow_stamp(), stamp);
        assert_eq!(published.ink_overflow_stamp, stamp);
        assert_eq!(
            published.ink_overflow.get(root.slot_index() as usize).unwrap().rect,
            rect(-200, 0, 300, 320)
        );
        arena
            .free_subtree(root)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn held_apart_child_contributes_to_its_containing_block() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let middle = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        arena.insert_child(root, middle, NodeSlotId::INVALID);
        arena.insert_child(middle, child, NodeSlotId::INVALID);
        box_link(&mut arena, root, NodeSlotId::INVALID, rect(0, 0, 100, 100));
        box_link(&mut arena, middle, root, rect(0, 0, 10, 10));
        box_link(&mut arena, child, root, rect(-500, -600, 20, 20));
        arena.note_whether_link_is_held_apart_from_its_containing_block(child, root, true);
        arena.settle_ink_overflow();
        assert!(
            arena
                .stored_ink_overflow(root)
                .rect
                .contains_rect(rect(-500, -600, 20, 20))
        );
        assert!(
            !arena
                .stored_ink_overflow(middle)
                .rect
                .contains_rect(rect(-500, -600, 20, 20))
        );
        arena
            .free_subtree(root)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }

    #[test]
    fn resetting_a_row_takes_its_contribution_from_its_containing_block() {
        let mut arena = LayoutNodeArena::new();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        arena.insert_child(root, child, NodeSlotId::INVALID);
        let mut root_link = box_link(&mut arena, root, NodeSlotId::INVALID, rect(0, 0, 100, 100));
        let child_link = box_link(&mut arena, child, root, rect(-200, 300, 20, 20));
        arena
            .paintable_rows
            .take_committed_fragment_link(root.slot_index(), root.generation());
        Arc::get_mut(&mut root_link.fragment).unwrap().children.push(child_link);
        arena.set_committed_fragment_link(root, root_link, None);
        arena.settle_ink_overflow();
        assert_eq!(arena.stored_ink_overflow(root).rect, rect(-200, 0, 300, 320));
        let stamp = arena.ink_overflow_stamp();

        arena.set_committed_fragment_link(
            root,
            childless_link(root, NodeSlotId::INVALID, rect(0, 0, 100, 100)),
            None,
        );
        arena.clear_absolute_rect_memo();
        arena.remove_child(root, child);
        arena
            .free_subtree(child)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        arena.settle_ink_overflow();
        assert_eq!(arena.stored_ink_overflow(root).rect, rect(0, 0, 100, 100));
        assert_ne!(arena.ink_overflow_stamp(), stamp);
        arena
            .free_subtree(root)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
    }
}
