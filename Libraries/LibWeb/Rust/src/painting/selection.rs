/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::css::style::tree::StyleNodeID;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeSlotId, SELECTION_PSEUDO_KIND};
use crate::painting::display_list::commands::OptionalColor;
use crate::painting::fragment_ownership;
use crate::painting::host::FfiSelectionStyleFacts;
use crate::painting::paint_read::GeometryRead;
use crate::painting::paintable_data::{FfiSelectionEntry, SELECTION_STATE_NONE};
use crate::painting::paintable_rows::PaintableRowsMut;
use crate::painting::record::damage::PaintDamage;
use crate::painting::record::paint::text::{SelectionStyleAnswer, ShadowLayer};
use crate::painting::text_fragment;
use libgfx_rust::Color;
use std::sync::Arc;

#[derive(Debug)]
pub(crate) struct SelectionRange {
    pub start_offset: usize,
    pub end_offset: usize,
    pub text_states: std::collections::HashMap<NodeSlotId, u8>,
}

fn invalidate_text_node(layout_arena: &PaintableRowsMut<'_>, node: NodeSlotId) {
    if let Some(containing_block) = text_fragment::containing_block_paintable_of_node(layout_arena, node) {
        layout_arena.push_paint_damage(containing_block, PaintDamage::ALL_DRAW);
    }
    if let Some(inline_box) = fragment_ownership::nearest_self_painting_inline_box(layout_arena, node) {
        layout_arena.push_paint_damage(inline_box, PaintDamage::ALL_DRAW);
    }
}

pub(crate) fn clear(layout_arena: &mut PaintableRowsMut<'_>, viewport: NodeSlotId) {
    let previous = layout_arena.paint_state().borrow_mut().selection.take();
    if let Some(previous) = previous {
        for node in previous.text_states.keys() {
            invalidate_text_node(layout_arena, *node);
        }
    }
    let mut slots = Vec::new();
    crate::painting::paint_order::for_each_in_paint_subtree(layout_arena, viewport, |slot| {
        slots.push(slot);
    });
    for current in slots {
        if layout_arena.paintable_data(current).selection_state != SELECTION_STATE_NONE {
            layout_arena.paintable_data_mut(current).selection_state = SELECTION_STATE_NONE;
            layout_arena.push_paint_damage(current, PaintDamage::ALL_DRAW);
        }
    }
}

pub(crate) fn apply(
    layout_arena: &mut PaintableRowsMut<'_>,
    viewport: NodeSlotId,
    entries: &[FfiSelectionEntry],
) -> std::collections::HashMap<NodeSlotId, u8> {
    clear(layout_arena, viewport);
    let mut text_states = std::collections::HashMap::new();
    for entry in entries {
        if entry.is_text_node_entry {
            for &node in layout_arena.text_fragments(entry.layout_node).as_slice() {
                text_states.insert(node, entry.state);
                invalidate_text_node(layout_arena, node);
            }
        } else {
            if !layout_arena.paintable_row_is_populated(entry.layout_node) {
                continue;
            }
            if layout_arena.paintable_data(entry.layout_node).selection_state != entry.state {
                layout_arena.paintable_data_mut(entry.layout_node).selection_state = entry.state;
                layout_arena.push_paint_damage(entry.layout_node, PaintDamage::ALL_DRAW);
            }
        }
    }
    text_states
}

/// https://drafts.csswg.org/css-pseudo-4/#highlight-styling
/// What selected text under `element` paints with, read from `style_record`, the element's
/// `::selection` record (zero for none), or `None` when that record styles nothing a selection
/// paints.
fn selection_pseudo_style_of_record(
    engine: &crate::css::style::StyleEngine,
    element: StyleNodeID,
    style_record: u64,
) -> Option<SelectionStyleAnswer> {
    use crate::css::computed_longhand_table::{HIGHLIGHT_COLOR_IS_CURRENT_COLOR, HIGHLIGHT_COLORS_AUTHORED};
    let style = engine.published_record_view(style_record)?;
    let dependency_flags = engine.published_record_dependency_flags(style_record).unwrap_or(0);
    // https://drafts.csswg.org/css-pseudo-4/#paired-defaults
    // Paired default highlight colors must only be used when neither 'color' nor 'background-color' yield a
    // cascaded value from the author origin (or inherit their value from the author origin).
    let mut facts = FfiSelectionStyleFacts {
        colors_authored: dependency_flags & HIGHLIGHT_COLORS_AUTHORED != 0,
        ..Default::default()
    };
    if facts.colors_authored {
        facts.background_color = Color(style.background().background_color);
        // https://drafts.csswg.org/css-pseudo-4/#highlight-text
        // currentColor on a highlight pseudo-element's 'color' property represents the color of the next active
        // highlight pseudo-element layer below, falling back finally to the colors that would otherwise have been
        // used.
        if dependency_flags & HIGHLIGHT_COLOR_IS_CURRENT_COLOR == 0 {
            facts.text_color = OptionalColor::some(Color(style.inherited_text().color));
        }

        // https://drafts.csswg.org/css-pseudo-4/#highlight-replaced
        // This wash should be of the specified 'background-color' if that is not 'transparent', else of the
        // specified 'color'; however the UA may adjust the alpha channel.
        let mut wash_color = facts.background_color;
        if wash_color.alpha() == 0 {
            wash_color = facts.text_color.get().unwrap_or_else(|| {
                Color(
                    engine
                        .published_style_view(element, None)
                        .map_or(0, |element_style| element_style.inherited_text().color),
                )
            });
        }
        facts.wash_color = transform_selection_background_color(wash_color);
    }

    let shadows: Vec<ShadowLayer> = style
        .inherited_text()
        .text_shadow
        .as_slice()
        .iter()
        .map(|shadow| ShadowLayer {
            color: shadow.color,
            offset_x: CssPixels::from_raw(shadow.offset_x),
            offset_y: CssPixels::from_raw(shadow.offset_y),
            blur_radius: CssPixels::from_raw(shadow.blur_radius),
        })
        .collect();
    facts.has_text_shadow = !shadows.is_empty();

    let text_reset = style.text_reset();
    let lines = text_reset.text_decoration_lines.as_slice();
    if !lines.is_empty() {
        facts.has_text_decoration = true;
        let count = lines.len().min(facts.text_decoration_lines.len());
        facts.text_decoration_lines[..count].copy_from_slice(&lines[..count]);
        facts.text_decoration_line_count = count as u32;
        facts.text_decoration_style = text_reset.text_decoration_style;
        facts.text_decoration_color = Color(text_reset.text_decoration_color);
    }

    (facts.colors_authored || facts.has_text_shadow || facts.has_text_decoration)
        .then_some(SelectionStyleAnswer { facts, shadows })
}

/// The UA's adjustment of a selection wash's alpha channel: an opaque color becomes the most
/// transparent color that, over white, still looks the same.
fn transform_selection_background_color(color: Color) -> Color {
    if color.alpha() < 255 {
        return color;
    }

    const START_ALPHA: i32 = 153; // 60%
    const END_ALPHA: i32 = 204; // 80%
    const ALPHA_INCREMENT: i32 = 17;

    let blend_component = |component: u8, alpha: i32| (i32::from(component) - (255 - alpha)) * 255 / alpha;

    let mut result = Color::default();
    let mut alpha = START_ALPHA;
    while alpha <= END_ALPHA {
        let red = blend_component(color.red(), alpha);
        let green = blend_component(color.green(), alpha);
        let blue = blend_component(color.blue(), alpha);
        result = Color::from_rgba(
            red.clamp(0, 255) as u8,
            green.clamp(0, 255) as u8,
            blue.clamp(0, 255) as u8,
            alpha as u8,
        );
        if red >= 0 && green >= 0 && blue >= 0 {
            break;
        }
        alpha += ALPHA_INCREMENT;
    }
    result
}

/// Gives the rows that paint text under `element` what `style_record`, the `::selection` record
/// the host holds for it (zero for none), says selected text paints with: the element's own rows,
/// or, while it has no box, the rows of its text children, which then have no element row above
/// them to find it on.
pub(crate) fn sync_selection_pseudo_style(arena: &LayoutNodeArena, element: StyleNodeID, style_record: u64) {
    let answer = arena
        .with_style_store(|engine| selection_pseudo_style_of_record(engine, element, style_record))
        .map(Arc::new);
    let element_row = arena.bound_row(element);
    let text_rows_answer = if element_row.is_invalid() {
        answer.as_ref()
    } else {
        None
    };
    arena.with_style_store(|engine| {
        for text in engine
            .tree()
            .dom_children(element)
            .filter(|child| child.text_index().is_some())
        {
            let row = arena.bound_row(text);
            if !row.is_invalid() {
                set_selection_pseudo_style_of_rows(arena, arena.rows_sharing_dom_node_with(row), text_rows_answer);
            }
        }
    });
    if !element_row.is_invalid() {
        set_selection_pseudo_style_of_rows(arena, arena.rows_sharing_dom_node_with(element_row), answer.as_ref());
    }
}

/// Gives a row the tree build made what the published `::selection` record of `element` says
/// selected text paints with: `element`'s own row, or a text row whose parent element has no box.
pub(crate) fn note_built_row_selection_pseudo_style(arena: &LayoutNodeArena, row: NodeSlotId, element: StyleNodeID) {
    let answer = arena
        .with_style_store(|engine| {
            let style_record = engine.pseudo_published_style_record(element, SELECTION_PSEUDO_KIND)?;
            selection_pseudo_style_of_record(engine, element, style_record)
        })
        .map(Arc::new);
    set_selection_pseudo_style_of_rows(arena, arena.rows_sharing_dom_node_with(row), answer.as_ref());
}

fn set_selection_pseudo_style_of_rows(
    arena: &LayoutNodeArena,
    rows: impl Iterator<Item = NodeSlotId>,
    answer: Option<&Arc<SelectionStyleAnswer>>,
) {
    let styles = &mut arena.paint_state().borrow_mut().selection_pseudo_styles;
    for row in rows {
        match answer {
            Some(answer) => {
                Arc::make_mut(styles).insert(row, answer.clone());
            }
            None if styles.contains_key(&row) => {
                Arc::make_mut(styles).remove(&row);
            }
            None => {}
        }
    }
}
