/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::{line_style, overflow, text_decoration_line, text_decoration_style, writing_mode};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::painting::border_radii::BorderRadii;
use crate::painting::corner_shape::CornerShapes;
use crate::painting::fragment_ownership;
use crate::painting::paint_read::PaintRead;
use crate::painting::published_frame::PublishedPaintState;
use crate::painting::record::paint::{
    border_image, column_rule, outline, shadow, table_backgrounds, table_borders, text, text_decoration,
};
use crate::painting::{border_shape, chrome_geometry, node_painting, paintable_geometry, style_queries, text_fragment};

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct InkOverflow {
    pub rect: CssPixelRect,
    pub unbounded: bool,
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
        let is_scroll_container = style_queries::is_scroll_container(source, owner);
        if is_scroll_container || chrome_geometry::has_resizer(source, owner) {
            let chrome_reach = CssPixels::from_integer(32);
            let mut chrome_bounds = inflated(
                paintable_geometry::absolute_padding_box_rect(source, owner),
                chrome_reach,
            );
            if is_scroll_container {
                // Scrollbar tracks clamp their absolute left or top edge to zero.
                chrome_bounds.set_right(chrome_bounds.right().max(chrome_reach));
                chrome_bounds.set_bottom(chrome_bounds.bottom().max(chrome_reach));
            }
            ink.rect.unite(chrome_bounds);
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
        for piece in side.inline_box_pieces() {
            let piece_owner = if fragment_ownership::is_self_painting_inline(source, piece.node) {
                piece.node
            } else {
                fragment_ownership::nearest_self_painting_inline_box(source, piece.node).unwrap_or(root)
            };
            if piece.node != owner && piece_owner != owner {
                continue;
            }
            let border_box = CssPixelRect::from(piece.border_box_rect)
                .translated_by(positions.of_line(side.lines(), piece.line_index as usize));
            let piece_ink = box_decoration_ink(source, piece.node, border_box);
            ink.rect.unite(piece_ink.rect);
            // Each piece can record a backdrop-filter region covering the whole inline.
            ink.rect
                .unite(paintable_geometry::absolute_border_box_rect(source, piece.node));
            ink.unbounded |= piece_ink.unbounded;
        }
        let filter = fragment_ownership::effective_filter(source, owner);
        filter.for_each_owned_fragment_index(side.fragments().len(), |index| {
            include_text_fragment(source, paint_state, root, index as u32, &mut ink.rect);
        });
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

fn include_text_fragment(
    source: &impl PaintRead,
    state: &PublishedPaintState,
    root: NodeSlotId,
    index: u32,
    bounds: &mut CssPixelRect,
) {
    let side = source.committed_side_data(root);
    let fragment = &side.fragments()[index as usize];
    let Some(run) = &fragment.glyph_run else {
        return;
    };
    let rect = text_fragment::absolute_rect(source, fragment);
    bounds.unite(rect);
    let glyph = run.bounding_box;
    let pixel = CssPixels::nearest_value_for_f32;
    let glyph_rect = if fragment.writing_mode == writing_mode::HORIZONTAL_TB {
        CssPixelRect::new(
            rect.x + pixel(glyph.x),
            rect.y + fragment.baseline + pixel(glyph.y),
            pixel(glyph.width),
            pixel(glyph.height),
        )
    } else {
        CssPixelRect::new(
            rect.right() - fragment.baseline - pixel(glyph.y + glyph.height),
            rect.y + pixel(glyph.x),
            pixel(glyph.height),
            pixel(glyph.width),
        )
    };
    bounds.unite(glyph_rect);
    let Some(style) = source.node_style_if_live(fragment.style_source) else {
        return;
    };
    bounds.unite(inflated(glyph_rect, text_stroke_reach(style)));
    let mut span = text::RenderSpan {
        fragment_index: index,
        start_code_unit: 0,
        end_code_unit: fragment.length_in_code_units,
        text_color: style.current_color(),
        shadow_layers: Vec::new(),
        highlights: Vec::new(),
        decoration_color: None,
    };
    for shadow in style.inherited_text().text_shadow.as_slice() {
        let reach = CssPixels::from_raw(shadow.blur_radius) * 2;
        bounds.unite(inflated(rect, reach).translated(
            CssPixels::from_raw(shadow.offset_x),
            CssPixels::from_raw(shadow.offset_y),
        ));
    }
    for styles in [
        &state.selection_pseudo_styles,
        &state.search_text_pseudo_styles,
        &state.search_text_current_pseudo_styles,
        &state.spelling_error_pseudo_styles,
    ]
    .into_iter()
    .chain(state.custom_highlight_pseudo_styles.values())
    {
        let Some(answer) = highlight_style(source, styles, fragment.layout_node) else {
            continue;
        };
        for shadow in &answer.shadows {
            bounds.unite(
                inflated(rect, shadow.layer.blur_radius * 2).translated(shadow.layer.offset_x, shadow.layer.offset_y),
            );
        }
        if answer.facts.has_text_decoration {
            span.highlights.push(text::HighlightOverlay {
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
    // The default spelling-error decoration exists without a committed pseudo style.
    span.highlights.push(text::HighlightOverlay {
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
    for set in text_decoration::decoration_sets_for_span(source, root, &span) {
        bounds.unite(text_decoration::decoration_ink_bounds(rect, &set));
    }
    bounds.unite(text_fragment::rect_for_selection_offsets(
        source,
        fragment,
        text_fragment::SelectionOffsets {
            start: 0,
            end: fragment.length_in_code_units,
        },
    ));
}
