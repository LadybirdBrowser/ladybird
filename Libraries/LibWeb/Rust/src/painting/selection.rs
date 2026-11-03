/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::css::style::index::StyleAtomID;
use crate::css::style::tree::StyleNodeID;
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{
    NodeSlotId, SEARCH_TEXT_CURRENT_PSEUDO_KIND, SEARCH_TEXT_PSEUDO_KIND, SELECTION_PSEUDO_KIND,
    SPELLING_ERROR_PSEUDO_KIND,
};
use crate::painting::display_list::commands::OptionalColor;
use crate::painting::fragment_ownership;
use crate::painting::host::FfiSelectionStyleFacts;
use crate::painting::paint_read::GeometryRead;
use crate::painting::paintable_data::{
    FfiCustomHighlightReplacedEntry, FfiCustomHighlightTextEntry, FfiSearchTextRange, FfiSelectionEntry,
    FfiSpellingError, SELECTION_STATE_END, SELECTION_STATE_NONE, SELECTION_STATE_START, SELECTION_STATE_START_AND_END,
};
use crate::painting::paintable_rows::PaintableRowsMut;
use crate::painting::record::damage::PaintDamage;
use crate::painting::record::paint::text::{
    HighlightDecorationMetrics, HighlightShadowLayer, SelectionStyleAnswer, ShadowLayer,
};
use crate::painting::text_fragment;
use libgfx_rust::Color;
use std::sync::Arc;

/// A highlight pseudo-element whose overlay text paints.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum HighlightPseudoElement {
    Selection,
    SearchText,
    SearchTextCurrent,
    SpellingError,
}

impl HighlightPseudoElement {
    pub(crate) const ALL: [Self; 4] = [
        Self::Selection,
        Self::SearchText,
        Self::SearchTextCurrent,
        Self::SpellingError,
    ];

    /// The pseudo-element's kind, as the style engine numbers an element's pseudo-element records.
    pub(crate) fn pseudo_kind(self) -> u8 {
        match self {
            Self::Selection => SELECTION_PSEUDO_KIND,
            Self::SearchText => SEARCH_TEXT_PSEUDO_KIND,
            Self::SearchTextCurrent => SEARCH_TEXT_CURRENT_PSEUDO_KIND,
            Self::SpellingError => SPELLING_ERROR_PSEUDO_KIND,
        }
    }
}

/// The committed styles of each highlight pseudo-element, for an element's rows.
pub(crate) type HighlightStyleAnswers = [Option<Arc<SelectionStyleAnswer>>; 4];

/// The records an element holds for each highlight pseudo-element, zero for none.
pub(crate) type HighlightStyleRecords = [u64; 4];

#[derive(Debug)]
pub(crate) struct SelectionRange {
    pub start_offset: usize,
    pub end_offset: usize,
    pub text_states: std::collections::HashMap<NodeSlotId, u8>,
    pub boxes_with_selection_state: Vec<NodeSlotId>,
}

fn invalidate_text_node(layout_arena: &mut PaintableRowsMut<'_>, node: NodeSlotId) {
    if let Some(containing_block) = text_fragment::containing_block_paintable_of_node(layout_arena, node) {
        layout_arena.push_paint_damage(containing_block, PaintDamage::ALL_DRAW);
    }
    if let Some(inline_box) = fragment_ownership::nearest_self_painting_inline_box(layout_arena, node) {
        layout_arena.push_paint_damage(inline_box, PaintDamage::ALL_DRAW);
    }
}

pub(crate) fn clear(layout_arena: &mut PaintableRowsMut<'_>) {
    let Some(previous) = layout_arena.paint_state_mut().selection.take() else {
        return;
    };
    for node in previous.text_states.keys() {
        invalidate_text_node(layout_arena, *node);
    }
    for &row in &previous.boxes_with_selection_state {
        if layout_arena.paintable_row_is_populated(row)
            && layout_arena.paintable_data(row).selection_state != SELECTION_STATE_NONE
        {
            layout_arena.paintable_data_mut(row).selection_state = SELECTION_STATE_NONE;
            layout_arena.push_paint_damage(row, PaintDamage::ALL_DRAW);
        }
    }
}

pub(crate) fn apply(
    layout_arena: &mut PaintableRowsMut<'_>,
    entries: &[FfiSelectionEntry],
    start_offset: usize,
    end_offset: usize,
) -> SelectionRange {
    clear(layout_arena);
    let mut text_states = std::collections::HashMap::new();
    let mut boxes_with_selection_state = Vec::new();
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
                boxes_with_selection_state.push(entry.layout_node);
            }
        }
    }
    SelectionRange {
        start_offset,
        end_offset,
        text_states,
        boxes_with_selection_state,
    }
}

#[derive(Debug)]
pub(crate) struct SearchTextMatch {
    pub start_offset: usize,
    pub end_offset: usize,
    pub is_current: bool,
}

impl SearchTextMatch {
    /// Where the match starts and ends in the text of a node it touches, given its selection state there.
    pub(crate) fn offsets_in_node(&self, state: u8) -> (usize, usize) {
        let starts_in_node = state == SELECTION_STATE_START || state == SELECTION_STATE_START_AND_END;
        let ends_in_node = state == SELECTION_STATE_END || state == SELECTION_STATE_START_AND_END;
        (
            if starts_in_node { self.start_offset } else { 0 },
            if ends_in_node { self.end_offset } else { usize::MAX },
        )
    }
}

#[derive(Debug, Default)]
pub(crate) struct SearchTextHighlights {
    pub matches: Vec<SearchTextMatch>,
    /// For each node, the matches touching it with their selection states there, in the order they start in the
    /// node. Find-in-page matches do not overlap, so they also end in that order.
    pub by_node: std::collections::HashMap<NodeSlotId, Vec<(u32, u8)>>,
}

impl SearchTextHighlights {
    fn node_signature(&self, node: NodeSlotId) -> Vec<(u8, usize, usize, bool)> {
        let mut signature: Vec<_> = self
            .by_node
            .get(&node)
            .map(|touching| {
                touching
                    .iter()
                    .map(|&(match_index, state)| {
                        let found = &self.matches[match_index as usize];
                        (state, found.start_offset, found.end_offset, found.is_current)
                    })
                    .collect()
            })
            .unwrap_or_default();
        signature.sort_unstable();
        signature
    }
}

pub(crate) fn clear_search_text(layout_arena: &mut PaintableRowsMut<'_>) {
    let previous = std::mem::take(&mut layout_arena.paint_state_mut().search_text);
    for node in previous.by_node.keys() {
        invalidate_text_node(layout_arena, *node);
    }
}

pub(crate) fn apply_search_text(
    layout_arena: &mut PaintableRowsMut<'_>,
    ranges: &[FfiSearchTextRange],
    entries: &[FfiSelectionEntry],
) {
    let mut highlights = SearchTextHighlights::default();
    for range in ranges {
        let match_index = highlights.matches.len() as u32;
        highlights.matches.push(SearchTextMatch {
            start_offset: range.start_offset,
            end_offset: range.end_offset,
            is_current: range.is_current,
        });
        let range_entries = entries
            .get(range.first_entry..range.first_entry + range.entry_count)
            .unwrap_or_default();
        for entry in range_entries.iter().filter(|entry| entry.is_text_node_entry) {
            for &node in layout_arena.text_fragments(entry.layout_node).as_slice() {
                highlights
                    .by_node
                    .entry(node)
                    .or_default()
                    .push((match_index, entry.state));
            }
        }
    }
    let matches = &highlights.matches;
    for touching in highlights.by_node.values_mut() {
        touching.sort_by_key(|&(match_index, state)| matches[match_index as usize].offsets_in_node(state));
    }
    let previous = std::mem::take(&mut layout_arena.paint_state_mut().search_text);
    for node in previous.by_node.keys().chain(highlights.by_node.keys()) {
        if previous.node_signature(*node) != highlights.node_signature(*node) {
            invalidate_text_node(layout_arena, *node);
        }
    }
    layout_arena.paint_state_mut().search_text = Arc::new(highlights);
}

#[derive(Debug, Default)]
pub(crate) struct SpellingErrorHighlights {
    /// For each text fragment row, the misspelled runs of its DOM text, as start and end offsets in order.
    pub by_node: std::collections::HashMap<NodeSlotId, Vec<(usize, usize)>>,
}

pub(crate) fn clear_spelling_errors(layout_arena: &mut PaintableRowsMut<'_>) {
    let previous = std::mem::take(&mut layout_arena.paint_state_mut().spelling_errors);
    for node in previous.by_node.keys() {
        invalidate_text_node(layout_arena, *node);
    }
}

pub(crate) fn apply_spelling_errors(layout_arena: &mut PaintableRowsMut<'_>, errors: &[FfiSpellingError]) {
    let mut highlights = SpellingErrorHighlights::default();
    for error in errors {
        for &node in layout_arena.text_fragments(error.layout_node).as_slice() {
            highlights
                .by_node
                .entry(node)
                .or_default()
                .push((error.start_offset, error.end_offset));
        }
    }
    for runs in highlights.by_node.values_mut() {
        runs.sort_unstable();
    }
    let previous = std::mem::take(&mut layout_arena.paint_state_mut().spelling_errors);
    for node in previous.by_node.keys().chain(highlights.by_node.keys()) {
        if previous.by_node.get(node) != highlights.by_node.get(node) {
            invalidate_text_node(layout_arena, *node);
        }
    }
    layout_arena.paint_state_mut().spelling_errors = Arc::new(highlights);
}

#[derive(Clone, Debug, Default)]
pub(crate) struct CustomHighlights {
    /// The custom highlight name of each layer, from bottom to top.
    pub layers: Vec<StyleAtomID>,
    /// For each text fragment row, the runs of its DOM text each layer covers, as layer and start and end offsets.
    pub text_runs: std::collections::HashMap<NodeSlotId, Vec<(u32, usize, usize)>>,
    /// For each replaced row, the layers whose ranges contain its element, from bottom to top.
    pub replaced: std::collections::HashMap<NodeSlotId, Vec<u32>>,
}

impl CustomHighlights {
    fn text_signature(&self, node: NodeSlotId) -> Vec<(StyleAtomID, usize, usize)> {
        self.text_runs
            .get(&node)
            .into_iter()
            .flatten()
            .map(|&(layer, start, end)| (self.layers[layer as usize], start, end))
            .collect()
    }

    fn replaced_signature(&self, node: NodeSlotId) -> Vec<StyleAtomID> {
        self.replaced
            .get(&node)
            .into_iter()
            .flatten()
            .map(|&layer| self.layers[layer as usize])
            .collect()
    }

    pub(crate) fn forget_row(&mut self, node: NodeSlotId) {
        self.text_runs.remove(&node);
        self.replaced.remove(&node);
    }

    pub(crate) fn covers_row(&self, node: NodeSlotId) -> bool {
        self.text_runs.contains_key(&node) || self.replaced.contains_key(&node)
    }
}

fn damage_custom_highlight_changes(
    layout_arena: &mut PaintableRowsMut<'_>,
    previous: &CustomHighlights,
    highlights: &CustomHighlights,
) {
    for node in previous.text_runs.keys().chain(highlights.text_runs.keys()) {
        if previous.text_signature(*node) != highlights.text_signature(*node) {
            invalidate_text_node(layout_arena, *node);
        }
    }
    for &node in previous.replaced.keys().chain(highlights.replaced.keys()) {
        if previous.replaced_signature(node) != highlights.replaced_signature(node)
            && layout_arena.paintable_row_is_populated(node)
        {
            layout_arena.push_paint_damage(node, PaintDamage::ALL_DRAW);
        }
    }
}

pub(crate) fn clear_custom_highlights(layout_arena: &mut PaintableRowsMut<'_>) {
    let previous = std::mem::take(&mut layout_arena.paint_state_mut().custom_highlights);
    damage_custom_highlight_changes(layout_arena, &previous, &CustomHighlights::default());
}

pub(crate) fn apply_custom_highlights(
    layout_arena: &mut PaintableRowsMut<'_>,
    layers: &[StyleAtomID],
    text_entries: &[FfiCustomHighlightTextEntry],
    replaced_entries: &[FfiCustomHighlightReplacedEntry],
) {
    let mut highlights = CustomHighlights {
        layers: layers.to_vec(),
        ..CustomHighlights::default()
    };
    for entry in text_entries
        .iter()
        .filter(|entry| (entry.layer as usize) < layers.len())
    {
        for &node in layout_arena.text_fragments(entry.layout_node).as_slice() {
            highlights
                .text_runs
                .entry(node)
                .or_default()
                .push((entry.layer, entry.start_offset, entry.end_offset));
        }
    }
    // https://drafts.csswg.org/css-highlight-api-1/#painting
    // Overlapping ranges within a single custom highlight are rendered as if a single range representing the union
    // of the overlapping ones had been specified.
    for runs in highlights.text_runs.values_mut() {
        runs.sort_unstable();
        let mut merged: Vec<(u32, usize, usize)> = Vec::with_capacity(runs.len());
        for &(layer, start, end) in runs.iter() {
            match merged.last_mut() {
                Some(last) if last.0 == layer && start <= last.2 => last.2 = last.2.max(end),
                _ => merged.push((layer, start, end)),
            }
        }
        *runs = merged;
    }
    for entry in replaced_entries
        .iter()
        .filter(|entry| (entry.layer as usize) < layers.len())
    {
        if layout_arena.paintable_row_is_populated(entry.layout_node) {
            highlights
                .replaced
                .entry(entry.layout_node)
                .or_default()
                .push(entry.layer);
        }
    }
    for layers in highlights.replaced.values_mut() {
        layers.sort_unstable();
        layers.dedup();
    }
    let previous = std::mem::take(&mut layout_arena.paint_state_mut().custom_highlights);
    damage_custom_highlight_changes(layout_arena, &previous, &highlights);
    layout_arena.paint_state_mut().custom_highlights = Arc::new(highlights);
}

/// https://drafts.csswg.org/css-pseudo-4/#highlight-styling
/// What text under `element` paints with inside a highlight pseudo-element, read from
/// `style_record`, the element's record for that pseudo-element (zero for none), or `None` when
/// that record styles nothing a highlight paints.
fn highlight_pseudo_style_of_record(
    engine: &crate::css::style::RetainedState,
    element: StyleNodeID,
    style_record: u64,
    takes_own_decoration_metrics: bool,
) -> Option<SelectionStyleAnswer> {
    use crate::css::computed_longhand_table::{HIGHLIGHT_COLOR_IS_CURRENT_COLOR, HIGHLIGHT_COLORS_AUTHORED};
    use crate::css::property_metadata::property_id;
    let style = engine.published_record_view(style_record)?;
    let dependency_flags = engine.published_record_dependency_flags(style_record).unwrap_or(0);
    let longhands = engine.published_record_longhands(style_record);
    let specified_as_current_color =
        |property_id: u16| longhands.is_some_and(|longhands| longhands.specified_value_is_current_color(property_id));
    // https://drafts.csswg.org/css-pseudo-4/#paired-defaults
    // Paired default highlight colors must only be used when neither 'color' nor 'background-color' yield a
    // cascaded value from the author origin (or inherit their value from the author origin).
    let mut facts = FfiSelectionStyleFacts {
        colors_authored: dependency_flags & HIGHLIGHT_COLORS_AUTHORED != 0,
        ..Default::default()
    };
    if facts.colors_authored {
        facts.background_color = Color(style.background_color());
        facts.background_color_is_current_color = specified_as_current_color(property_id::BACKGROUND_COLOR);
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

    let shadow_colors_are_current_color = longhands
        .map(|longhands| longhands.text_shadow_layer_colors_are_current_color())
        .unwrap_or_default();
    let shadows: Vec<HighlightShadowLayer> = style
        .inherited_text()
        .text_shadow
        .as_slice()
        .iter()
        .enumerate()
        .map(|(index, shadow)| HighlightShadowLayer {
            layer: ShadowLayer {
                color: shadow.color.resolve(style.current_color()),
                offset_x: CssPixels::from_raw(shadow.offset_x),
                offset_y: CssPixels::from_raw(shadow.offset_y),
                blur_radius: CssPixels::from_raw(shadow.blur_radius),
            },
            color_is_current_color: shadow_colors_are_current_color.get(index).copied().unwrap_or(false),
        })
        .collect();
    facts.has_text_shadow = !shadows.is_empty();

    let text_reset = style.text_reset();
    let lines = text_reset.text_decoration_lines.as_slice();
    let mut decoration_metrics = None;
    if !lines.is_empty() && takes_own_decoration_metrics {
        let inherited_text = style.inherited_text();
        let element_font_size = engine
            .published_style_view(element, None)
            .map_or(CssPixels::from_raw(0), |element_style| element_style.font_size());
        decoration_metrics = Some(HighlightDecorationMetrics {
            thickness: (text_reset.text_decoration_thickness_kind == 2).then(|| {
                text_reset
                    .text_decoration_thickness
                    .length_percentage()
                    .map_or(CssPixels::from_integer(1), |value| value.to_px(element_font_size))
                    .max(CssPixels::from_integer(1))
            }),
            underline_offset: inherited_text.text_underline_offset.used_value,
            underline_position_horizontal: inherited_text.text_underline_position.horizontal,
            skip_ink: inherited_text.text_decoration_skip_ink,
        });
    }
    if !lines.is_empty() {
        facts.has_text_decoration = true;
        let count = lines.len().min(facts.text_decoration_lines.len());
        facts.text_decoration_lines[..count].copy_from_slice(&lines[..count]);
        facts.text_decoration_line_count = count as u32;
        facts.text_decoration_style = text_reset.text_decoration_style;
        facts.text_decoration_color = Color(style.text_decoration_color());
        facts.text_decoration_color_is_current_color = specified_as_current_color(property_id::TEXT_DECORATION_COLOR);
    }

    (facts.colors_authored || facts.has_text_shadow || facts.has_text_decoration).then_some(SelectionStyleAnswer {
        facts,
        shadows,
        decoration_metrics,
    })
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

fn highlight_pseudo_styles_of_records(
    arena: &LayoutNodeArena,
    element: StyleNodeID,
    style_records: HighlightStyleRecords,
) -> HighlightStyleAnswers {
    arena.with_style_store(|engine| {
        style_records
            .map(|style_record| highlight_pseudo_style_of_record(engine, element, style_record, false).map(Arc::new))
    })
}

/// Gives the rows that paint text under `element` what `style_records`, the highlight
/// pseudo-element records the host holds for it, say highlighted text paints with: the element's
/// own rows, or, while it has no box, the rows of its text children, which then have no element
/// row above them to find it on.
pub(crate) fn sync_highlight_pseudo_styles(
    arena: &mut LayoutNodeArena,
    element: StyleNodeID,
    style_records: HighlightStyleRecords,
) {
    let answers = highlight_pseudo_styles_of_records(arena, element, style_records);
    let element_row = arena.bound_row(element);
    let text_rows_answers = if element_row.is_invalid() {
        answers.clone()
    } else {
        HighlightStyleAnswers::default()
    };
    for row in text_child_rows(arena, element) {
        let rows: Vec<NodeSlotId> = arena.rows_sharing_dom_node_with(row).collect();
        set_highlight_pseudo_styles_of_rows(arena, &rows, &text_rows_answers);
    }
    if !element_row.is_invalid() {
        let rows: Vec<NodeSlotId> = arena.rows_sharing_dom_node_with(element_row).collect();
        set_highlight_pseudo_styles_of_rows(arena, &rows, &answers);
    }
}

/// Gives the rows that paint text under `element` `selection_record`, the record of its `::selection` a clock lane
/// styled, zero for none. The other highlight pseudo-elements stay as the host published them.
pub(crate) fn sync_selection_style(arena: &mut LayoutNodeArena, element: StyleNodeID, selection_record: u64) {
    let style_records = arena.with_style_store(|engine| {
        HighlightPseudoElement::ALL.map(|highlight| match highlight {
            HighlightPseudoElement::Selection => selection_record,
            _ => engine
                .pseudo_published_style_record(element, highlight.pseudo_kind())
                .unwrap_or(0),
        })
    });
    sync_highlight_pseudo_styles(arena, element, style_records);
}

fn text_child_rows(arena: &LayoutNodeArena, element: StyleNodeID) -> Vec<NodeSlotId> {
    arena.with_style_store(|engine| {
        engine
            .dom_children(element)
            .filter(|child| child.text_index().is_some())
            .map(|text| arena.bound_row(text))
            .filter(|row| !row.is_invalid())
            .collect()
    })
}

type CustomHighlightStyleAnswers = Vec<(StyleAtomID, Arc<SelectionStyleAnswer>)>;

fn custom_highlight_pseudo_styles_of_records(
    arena: &LayoutNodeArena,
    element: StyleNodeID,
    style_records: &[(StyleAtomID, u64)],
) -> CustomHighlightStyleAnswers {
    arena.with_style_store(|engine| {
        style_records
            .iter()
            .filter_map(|&(name, style_record)| {
                // Custom highlights take their decoration metrics from their own style, matching other engines.
                Some((
                    name,
                    Arc::new(highlight_pseudo_style_of_record(engine, element, style_record, true)?),
                ))
            })
            .collect()
    })
}

pub(crate) fn sync_custom_highlight_pseudo_styles(
    arena: &mut LayoutNodeArena,
    element: StyleNodeID,
    style_records: &[(StyleAtomID, u64)],
) {
    let answers = custom_highlight_pseudo_styles_of_records(arena, element, style_records);
    let element_row = arena.bound_row(element);
    for row in text_child_rows(arena, element) {
        let rows: Vec<NodeSlotId> = arena.rows_sharing_dom_node_with(row).collect();
        let text_rows_answers = if element_row.is_invalid() { &answers[..] } else { &[] };
        set_custom_highlight_pseudo_styles_of_rows(arena, &rows, text_rows_answers);
    }
    if !element_row.is_invalid() {
        let rows: Vec<NodeSlotId> = arena.rows_sharing_dom_node_with(element_row).collect();
        set_custom_highlight_pseudo_styles_of_rows(arena, &rows, &answers);
    }
}

fn set_custom_highlight_pseudo_styles_of_rows(
    arena: &mut LayoutNodeArena,
    rows: &[NodeSlotId],
    answers: &[(StyleAtomID, Arc<SelectionStyleAnswer>)],
) {
    let paint_state = arena.paint_state_mut();
    let unstyled: Vec<StyleAtomID> = paint_state
        .custom_highlight_pseudo_styles
        .iter()
        .filter(|(name, styles)| {
            !answers.iter().any(|(answered, _)| answered == *name) && rows.iter().any(|row| styles.contains_key(row))
        })
        .map(|(name, _)| *name)
        .collect();
    if unstyled.is_empty() && answers.is_empty() {
        return;
    }
    let table = Arc::make_mut(&mut paint_state.custom_highlight_pseudo_styles);
    for name in unstyled {
        if let Some(styles) = table.get_mut(&name) {
            let styles = Arc::make_mut(styles);
            for row in rows {
                styles.remove(row);
            }
            if styles.is_empty() {
                table.remove(&name);
            }
        }
    }
    for (name, answer) in answers {
        let styles = Arc::make_mut(table.entry(*name).or_default());
        for &row in rows {
            styles.insert(row, answer.clone());
        }
    }
}

/// Gives a row the tree build made what the published highlight pseudo-element records of
/// `element` say highlighted text paints with: `element`'s own row, or a text row whose parent
/// element has no box.
pub(crate) fn note_built_row_highlight_pseudo_styles(
    arena: &mut LayoutNodeArena,
    row: NodeSlotId,
    element: StyleNodeID,
) {
    let (style_records, custom_style_records) = arena.with_style_store(|engine| {
        (
            HighlightPseudoElement::ALL.map(|highlight| {
                engine
                    .pseudo_published_style_record(element, highlight.pseudo_kind())
                    .unwrap_or(0)
            }),
            engine.pseudo_published_custom_highlight_records(element),
        )
    });
    let answers = highlight_pseudo_styles_of_records(arena, element, style_records);
    let custom_answers = custom_highlight_pseudo_styles_of_records(arena, element, &custom_style_records);
    let rows: Vec<NodeSlotId> = arena.rows_sharing_dom_node_with(row).collect();
    set_highlight_pseudo_styles_of_rows(arena, &rows, &answers);
    set_custom_highlight_pseudo_styles_of_rows(arena, &rows, &custom_answers);
}

fn set_highlight_pseudo_styles_of_rows(
    arena: &mut LayoutNodeArena,
    rows: &[NodeSlotId],
    answers: &HighlightStyleAnswers,
) {
    let paint_state = arena.paint_state_mut();
    for (highlight, answer) in HighlightPseudoElement::ALL.into_iter().zip(answers) {
        let styles = paint_state.highlight_pseudo_styles_mut(highlight);
        for &row in rows {
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
}
