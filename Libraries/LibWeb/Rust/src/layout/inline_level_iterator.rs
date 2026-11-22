/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use prepared_inline_content::{
    InlineBoxEvent, InlineContentPreparationBuffers, PreparedInlineContent, PreparedItem, PreparedTabStop,
};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ItemType {
    Text,
    Element,
    BlockLevelBox,
    ForcedBreak,
    AbsolutelyPositionedElement,
    FloatingElement,
    InlineEdge,
}

impl ItemType {
    /// Whether an item of this type takes its place on the lines alone, unlike floats and block-level boxes, which lay
    /// out beside or between the lines.
    pub(crate) fn stays_on_lines(self) -> bool {
        matches!(
            self,
            Self::Text | Self::Element | Self::InlineEdge | Self::ForcedBreak | Self::AbsolutelyPositionedElement
        )
    }
}

#[derive(Clone, Debug)]
pub(crate) struct Item {
    pub(crate) type_: ItemType,
    pub(crate) node: Node,
    /// Which item of the prepared inline content this one was resolved from. A text item's font, text type and, unless
    /// it owns a glyph run, glyphs are that item's.
    prepared_item_index: u32,
    /// The glyphs of a text item that differ from the shape of its prepared item: moved past tab stops, or split off.
    owned_glyph_run: Option<Box<OwnedGlyphRun>>,
    pub(crate) offset_in_node: usize,
    pub(crate) length_in_node: usize,
    pub(crate) inline_size: CssPixels,
    pub(crate) padding_start: CssPixels,
    pub(crate) padding_end: CssPixels,
    pub(crate) border_start: CssPixels,
    pub(crate) border_end: CssPixels,
    pub(crate) margin_start: CssPixels,
    pub(crate) margin_end: CssPixels,
    pub(crate) entered_right_to_left_end_margin: CssPixels,
    pub(crate) exited_right_to_left_end_margin: CssPixels,
    pub(crate) is_collapsible_whitespace: bool,
    pub(crate) can_break_before: bool,
    pub(crate) preceded_by_unattached_inline_start_edges: bool,
    pub(crate) trailing_whitespace: line_box_fragment::TrailingWhitespace,
    atomic: Option<Box<AtomicItemMetrics>>,
}

const _: () = assert!(size_of::<Item>() == 96);

#[derive(Clone, Debug)]
pub(crate) struct OwnedGlyphRun {
    pub(crate) glyphs: libgfx_rust::text_layout::GlyphBuffer,
    pub(crate) width: f32,
    is_upright: bool,
}

#[derive(Clone, Copy, Debug, Default)]
struct AtomicItemMetrics {
    min_content_inline_size: Option<CssPixels>,
    content_baselines: DerivedBaselines,
    /// For an element item laid out on a line, the sizes and box sides of its box, as its layout left them.
    box_cells: Option<used_values::RootInputCells>,
}

/// The glyphs of `shape` with their pen positions starting at `baseline_start_x`.
fn shaped_glyphs_from(
    shape: &libgfx_rust::text_layout::SharedShape,
    baseline_start_x: f32,
) -> libgfx_rust::text_layout::GlyphBuffer {
    shape.with_baseline_start(baseline_start_x).into_glyphs()
}

/// `glyphs`, the glyphs of a text item past its leading tabs, after a glyph for each of those tabs that paints nothing
/// and advances to the tab's stop by the tab's entry in `tab_advances`, so that the code units of the item's glyphs line
/// up with those of its text.
fn glyphs_after_tab_spacers(
    glyphs: &libgfx_rust::text_layout::GlyphBuffer,
    tab_advances: &[f32],
) -> libgfx_rust::text_layout::GlyphBuffer {
    use libgfx_rust::text_layout::{DrawGlyph, GlyphBuffer, PlacedGlyph};
    let tab_count_u32 = u32::try_from(tab_advances.len()).expect("tab counts fit in 32 bits");
    let mut with_spacer = GlyphBuffer::default();
    let y = glyphs.placed().next().map_or(0.0, |placed| placed.y);
    let mut x = 0.0;
    for (index, &tab_advance) in tab_advances.iter().enumerate() {
        with_spacer.push_placed(PlacedGlyph {
            glyph: DrawGlyph::new(tab_advance, 0, 1, false),
            x,
            y,
            cluster_start: u32::try_from(index).expect("tab counts fit in 32 bits"),
            is_rtl: false,
        });
        x += tab_advance;
    }
    for placed in glyphs.placed() {
        with_spacer.push_placed(PlacedGlyph {
            cluster_start: placed.cluster_start + tab_count_u32,
            ..placed
        });
    }
    with_spacer
}

/// The advance of each tab of `tab_stop`, for tabs that start `position` from the start of their line.
fn advances_of_tabs(tab_stop: &PreparedTabStop, position: CssPixels) -> Vec<f32> {
    let tab_inline_size = tab_stop.tab_inline_size;
    // https://drafts.csswg.org/css-text-3/#white-space-phase-2
    // "each preserved tab is rendered as a horizontal shift that lines up the start edge of the next glyph with the
    // next tab stop. If this distance is less than 0.5ch, then the subsequent tab stop is used instead."
    // NB: So each tab of a run advances from where the tab before it ended, to a tab stop of its own.
    let distance_to_tab_stop = |position: CssPixels| {
        // https://drafts.csswg.org/css-text-3/#white-space-phase-2
        // "If the tab size is zero, preserved tabs are not rendered."
        if tab_inline_size == CssPixels::default() {
            return CssPixels::default();
        }
        // NB: The next tab stop is the first one after the position, even if the position is on a tab stop.
        let mut distance = position.div_as_fraction(tab_inline_size).ceil() * tab_inline_size - position;
        if distance <= CssPixels::default() {
            distance += tab_inline_size;
        }
        if distance.to_double() < f64::from(tab_stop.zero_advance) * 0.5 {
            distance += tab_inline_size;
        }
        distance
    };
    let mut advances = Vec::with_capacity(tab_stop.tab_count as usize);
    let mut end = position;
    let mut previous_end = 0.0f32;
    for _ in 0..tab_stop.tab_count {
        end += distance_to_tab_stop(end);
        let end_from_start = (end - position).to_double() as f32;
        advances.push(end_from_start - previous_end);
        previous_end = end_from_start;
    }
    advances
}

fn reshaped_glyph_run(
    text: &[u16],
    range: std::ops::Range<usize>,
    font: &libgfx_rust::font::FontHandle,
    text_type: u8,
    letter_spacing: f32,
    word_spacing: f32,
    is_upright: bool,
) -> OwnedGlyphRun {
    let shape = libgfx_rust::text_layout::shape_text_shared(
        font,
        &text[range.clone()],
        libgfx_rust::text_layout::TextType::try_from(text_type).expect("invalid Gfx::GlyphRun::TextType"),
        letter_spacing,
        word_spacing,
        text_chunker::joining_context(text, range),
        is_upright,
    );
    OwnedGlyphRun {
        glyphs: shape.glyphs().clone(),
        width: shape.width(),
        is_upright,
    }
}

/// The hyphen that ends a line broken after a soft hyphen: U+2010 HYPHEN where the font has a glyph for it, and U+002D
/// HYPHEN-MINUS otherwise.
// https://drafts.csswg.org/css-text-4/#valdef-hyphenate-character-auto
pub(crate) fn hyphen_glyph_run(
    font: &libgfx_rust::font::FontHandle,
    text_type: u8,
    letter_spacing: f32,
    word_spacing: f32,
) -> OwnedGlyphRun {
    let hyphen = [if font.contains_glyph(0x2010) { 0x2010 } else { 0x2d }];
    reshaped_glyph_run(&hyphen, 0..1, font, text_type, letter_spacing, word_spacing, false)
}

/// Whether a line that breaks after a soft hyphen in text of this style ends in a hyphen.
pub(crate) fn soft_hyphen_hyphenates(style: StyleValues<'_>) -> bool {
    // https://drafts.csswg.org/css-text-3/#valdef-word-break-break-all
    // Hyphenation is not applied.
    // https://drafts.csswg.org/css-text-3/#valdef-line-break-anywhere
    // Hyphenation is not applied.
    style.hyphens() != hyphens::NONE
        && style.word_break() != word_break::BREAK_ALL
        && style.line_break() != line_break::ANYWHERE
}

/// The inline size of the hyphen that a line ends in if it breaks right after `item`: none, unless the item's text ends
/// in a soft hyphen that its hyphens lets the line break after.
pub(crate) fn hyphen_inline_size_after(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    prepared: &PreparedInlineContent,
    item: &Item,
) -> CssPixels {
    if item.type_ != ItemType::Text || item.length_in_node == 0 {
        return CssPixels::default();
    }
    let text_content = context.pass.text_content(item.node);
    if !text_chunker::breaks_at_soft_hyphen(&text_content.text, item.offset_in_node + item.length_in_node) {
        return CssPixels::default();
    }
    let style = context.style(context.style_source(item.node));
    if !soft_hyphen_hyphenates(style) {
        return CssPixels::default();
    }
    let prepared_item = item.prepared_item(prepared);
    let run = hyphen_glyph_run(
        prepared.font_of(prepared_item),
        prepared_item.text_type(),
        style.letter_spacing().to_double() as f32,
        style.word_spacing().to_double() as f32,
    );
    CssPixels::nearest_value_for_f32(run.width)
}

/// Whether text in the given writing mode and text orientation that starts with `code_point` is set with its glyphs
/// upright, stacked along the vertical inline axis, rather than turned sideways like a horizontal line of text.
/// https://drafts.csswg.org/css-writing-modes-4/#text-orientation
pub(crate) fn text_is_set_upright(writing_mode: u8, text_orientation: u8, code_point: u32) -> bool {
    // NB: The sideways writing modes set all text sideways, whatever its text orientation.
    if !matches!(writing_mode, writing_mode::VERTICAL_RL | writing_mode::VERTICAL_LR) {
        return false;
    }
    match text_orientation {
        // upright: Typographic character units from horizontal-only scripts are typeset upright, i.e. in their
        // standard horizontal orientation. Typographic character units from vertical scripts are typeset with their
        // intrinsic orientation and shaped normally.
        // FIXME: This value causes the used value of 'direction' to be 'ltr', and for the purposes of bidi
        //        reordering, causes all characters to be treated as strong LTR.
        text_orientation::UPRIGHT => true,
        // mixed: Typographic character units from horizontal-only scripts are typeset sideways, i.e. 90° clockwise
        // from their standard orientation in horizontal text. Typographic character units from vertical scripts are
        // typeset with their intrinsic orientation.
        text_orientation::MIXED => text_chunker::code_point_is_upright_in_mixed_orientation(code_point),
        _ => false,
    }
}

/// Sets shaped glyphs upright and returns the inline size they take: each of them takes one em along the inline axis,
/// which runs downwards in the frame of a vertical writing mode, while keeping its own horizontal width, which painting
/// centers it within. Invisible characters such as soft hyphens take no space.
// FIXME: Use the vertical metrics of the font, where it has them.
fn set_glyphs_upright(glyphs: &mut libgfx_rust::text_layout::GlyphBuffer, inline_offset: f32, advance: f32) -> f32 {
    let mut pen = inline_offset;
    let mut count = 0usize;
    glyphs.update_positions(|glyph, x, _| {
        if glyph.length_in_code_units() != 0 && glyph.should_paint() {
            if count > 0 {
                pen += advance;
            }
            count += 1;
        }
        *x = pen;
    });
    advance * count as f32
}

fn shape_set_upright(
    shape: &libgfx_rust::text_layout::SharedShape,
    advance: f32,
) -> libgfx_rust::text_layout::SharedShape {
    let mut glyphs = shape.glyphs().clone();
    let width = set_glyphs_upright(&mut glyphs, 0.0, advance);
    libgfx_rust::text_layout::SharedShape::from_glyphs(
        glyphs,
        width,
        shape.trailing_whitespace_length_in_code_units(),
        shape.trailing_whitespace_advance(),
        shape.trailing_ascii_whitespace_advance(),
    )
}

impl Item {
    const NOT_RESOLVED_FROM_A_PREPARED_ITEM: usize = u32::MAX as usize;

    /// Moves the tabs that start this text item to the tab stops after `line_position`, where the item starts on its
    /// line: Resolving the item placed them as if its paragraph never wrapped. Only an item with tabs asks for that
    /// position.
    pub(crate) fn place_tabs_at_line_position(
        &mut self,
        prepared: &PreparedInlineContent,
        line_position: impl FnOnce() -> CssPixels,
    ) {
        let Some((prepared_item, tab_stop)) = self.leading_tabs(prepared) else {
            return;
        };
        let shape = prepared_item.shape();
        let tab_advances = advances_of_tabs(tab_stop, line_position());
        let inline_offset: f32 = tab_advances.iter().sum();
        let inline_size = CssPixels::nearest_value_for_f32(shape.width() + inline_offset);
        if inline_size == self.inline_size {
            return;
        }
        let glyphs = glyphs_after_tab_spacers(&shaped_glyphs_from(shape, inline_offset), &tab_advances);
        self.owned_glyph_run = Some(Box::new(OwnedGlyphRun {
            glyphs,
            width: shape.width(),
            is_upright: false,
        }));
        self.inline_size = inline_size;
        if prepared_item.has(PreparedItem::TRAILING_WHITESPACE_IS_WHOLE_ITEM) {
            self.trailing_whitespace.inline_size = inline_size;
            self.trailing_whitespace.ascii_inline_size = inline_size;
        }
    }

    /// The prepared item and tab stop of the tabs that start this text item, if it starts with any that move with
    /// where it starts on its line. Upright text keeps its tabs as they are, and only an item that still starts where
    /// its prepared item does holds the tabs at all.
    fn leading_tabs<'prepared>(
        &self,
        prepared: &'prepared PreparedInlineContent,
    ) -> Option<(&'prepared PreparedItem, &'prepared PreparedTabStop)> {
        if self.type_ != ItemType::Text {
            return None;
        }
        let prepared_item = &prepared.items[self.prepared_item_index as usize];
        if !prepared_item.has(PreparedItem::TAB_STOP)
            || prepared_item.has(PreparedItem::UPRIGHT)
            || self.offset_in_node != prepared_item.offset_in_node()
            || self.length_in_node != prepared_item.length_in_node()
        {
            return None;
        }
        Some((prepared_item, prepared.tab_stop_of_item(self.prepared_item_index)?))
    }

    pub(crate) fn prepared_item_index(&self) -> u32 {
        self.prepared_item_index
    }

    pub(crate) fn starts_with_tabs(&self, prepared: &PreparedInlineContent) -> bool {
        self.leading_tabs(prepared).is_some()
    }

    /// This item's inline size with the tabs that start it placed after `line_position`, where it would start on its
    /// line.
    pub(crate) fn inline_size_with_tabs_at(
        &self,
        prepared: &PreparedInlineContent,
        line_position: CssPixels,
    ) -> CssPixels {
        let Some((prepared_item, tab_stop)) = self.leading_tabs(prepared) else {
            return self.inline_size;
        };
        let inline_offset: f32 = advances_of_tabs(tab_stop, line_position).iter().sum();
        CssPixels::nearest_value_for_f32(prepared_item.shape().width() + inline_offset)
    }

    fn new(type_: ItemType, node: Node, prepared_item_index: usize) -> Self {
        Self {
            type_,
            node,
            prepared_item_index: u32::try_from(prepared_item_index).expect("prepared item indices fit in 32 bits"),
            owned_glyph_run: None,
            offset_in_node: 0,
            length_in_node: 0,
            inline_size: CssPixels::default(),
            padding_start: CssPixels::default(),
            padding_end: CssPixels::default(),
            border_start: CssPixels::default(),
            border_end: CssPixels::default(),
            margin_start: CssPixels::default(),
            margin_end: CssPixels::default(),
            entered_right_to_left_end_margin: CssPixels::default(),
            exited_right_to_left_end_margin: CssPixels::default(),
            is_collapsible_whitespace: false,
            can_break_before: false,
            preceded_by_unattached_inline_start_edges: false,
            trailing_whitespace: line_box_fragment::TrailingWhitespace::default(),
            atomic: None,
        }
    }

    fn prepared_item<'prepared>(&self, prepared: &'prepared PreparedInlineContent) -> &'prepared PreparedItem {
        let prepared_item = &prepared.items[self.prepared_item_index as usize];
        debug_assert_eq!(
            prepared_item.node(),
            self.node,
            "an item is read against the content it was resolved from"
        );
        prepared_item
    }

    /// The glyphs of a text item, read against the content the item was resolved from.
    pub(crate) fn glyph_run<'run>(
        &'run self,
        prepared: &'run PreparedInlineContent,
    ) -> Option<line_box_fragment::ItemGlyphRun<'run>> {
        if self.type_ != ItemType::Text {
            return None;
        }
        let prepared_item = self.prepared_item(prepared);
        let font = prepared.font_of(prepared_item);
        let text_type = prepared_item.text_type();
        Some(match &self.owned_glyph_run {
            Some(owned) => line_box_fragment::ItemGlyphRun {
                glyphs: std::borrow::Cow::Borrowed(&owned.glyphs),
                font,
                text_type,
                width: owned.width,
                is_upright: owned.is_upright,
            },
            None => line_box_fragment::ItemGlyphRun {
                glyphs: std::borrow::Cow::Borrowed(prepared_item.shape().glyphs()),
                font,
                text_type,
                width: prepared_item.shape().width(),
                is_upright: prepared_item.has(PreparedItem::UPRIGHT),
            },
        })
    }

    /// The glyph run of a text item, which the line it goes on takes from it: the item's own glyphs, moved out of it,
    /// or the shape of its prepared item, borrowed.
    pub(crate) fn take_glyph_run<'prepared>(
        &mut self,
        prepared: &'prepared PreparedInlineContent,
    ) -> line_box_fragment::ItemGlyphRun<'prepared> {
        let prepared_item = self.prepared_item(prepared);
        let font = prepared.font_of(prepared_item);
        let text_type = prepared_item.text_type();
        match self.owned_glyph_run.take() {
            Some(owned) => line_box_fragment::ItemGlyphRun {
                glyphs: std::borrow::Cow::Owned(owned.glyphs),
                font,
                text_type,
                width: owned.width,
                is_upright: owned.is_upright,
            },
            None => line_box_fragment::ItemGlyphRun {
                glyphs: std::borrow::Cow::Borrowed(prepared_item.shape().glyphs()),
                font,
                text_type,
                width: prepared_item.shape().width(),
                is_upright: prepared_item.has(PreparedItem::UPRIGHT),
            },
        }
    }

    /// The glyph run of a text item as one it owns, sharing the glyphs of its prepared item's shape if it has none.
    fn take_owned_glyph_run(&mut self, prepared: &PreparedInlineContent) -> OwnedGlyphRun {
        if let Some(owned) = self.owned_glyph_run.take() {
            return *owned;
        }
        let prepared_item = self.prepared_item(prepared);
        OwnedGlyphRun {
            glyphs: prepared_item.shape().glyphs().clone(),
            width: prepared_item.shape().width(),
            is_upright: prepared_item.has(PreparedItem::UPRIGHT),
        }
    }

    pub(crate) fn min_content_inline_size(&self) -> Option<CssPixels> {
        self.atomic.as_ref().and_then(|atomic| atomic.min_content_inline_size)
    }

    pub(crate) fn content_baselines(&self) -> DerivedBaselines {
        self.atomic
            .as_ref()
            .map(|atomic| atomic.content_baselines)
            .unwrap_or_default()
    }

    /// The sizes and box sides of the box of an element item laid out on a line.
    #[track_caller]
    pub(crate) fn box_cells(&self) -> used_values::RootInputCells {
        self.atomic
            .as_ref()
            .and_then(|atomic| atomic.box_cells)
            .expect("an element item laid out on a line carries the cells of its box")
    }

    pub(crate) fn set_box_cells(&mut self, box_cells: used_values::RootInputCells) {
        self.atomic.get_or_insert_default().box_cells = Some(box_cells);
    }

    pub(crate) fn border_box_inline_size(&self) -> CssPixels {
        self.border_start + self.padding_start + self.inline_size + self.padding_end + self.border_end
    }

    /// Whether the item carries the edges of any inline box it starts or ends.
    fn carries_inline_box_edges(&self) -> bool {
        let none = CssPixels::default();
        self.margin_start != none
            || self.border_start != none
            || self.padding_start != none
            || self.padding_end != none
            || self.border_end != none
            || self.margin_end != none
            || self.entered_right_to_left_end_margin != none
            || self.exited_right_to_left_end_margin != none
    }

    pub(crate) fn is_ascii_whitespace(&self, context: &inline_formatting_context::InlineFormattingContext<'_>) -> bool {
        assert_eq!(self.type_, ItemType::Text);
        let text = &context.pass.text_content(self.node).text;
        text[self.offset_in_node..self.offset_in_node + self.length_in_node]
            .iter()
            .all(|unit| *unit <= 0x7f && (*unit as u8).is_ascii_whitespace())
    }

    pub(crate) fn ends_in_ascii_space(&self, context: &inline_formatting_context::InlineFormattingContext<'_>) -> bool {
        assert_eq!(self.type_, ItemType::Text);
        let text = &context.pass.text_content(self.node).text;
        text[self.offset_in_node..self.offset_in_node + self.length_in_node]
            .last()
            .is_some_and(|unit| line_box_fragment::is_ascii_space(*unit))
    }

    pub(crate) fn allows_overflow_break(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> bool {
        self.type_ == ItemType::Text
            && !self.is_collapsible_whitespace
            && !self.is_ascii_whitespace(context)
            && context.overflow_break_applies(self.node)
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn split_for_overflow_break(
        &mut self,
        prepared: &PreparedInlineContent,
        text: &[u16],
        segmenter: &text_chunker::GraphemeSegmenter,
        letter_spacing: f32,
        word_spacing: f32,
        max_border_box_inline_size: CssPixels,
        take_first_grapheme_when_none_fits: bool,
    ) -> Option<Item> {
        assert_eq!(self.type_, ItemType::Text);
        (!self.prepared_item(prepared).has(PreparedItem::SOFT_WRAPS))
            .then(|| {
                self.split_at_shaped_glyph(
                    prepared,
                    segmenter,
                    max_border_box_inline_size,
                    take_first_grapheme_when_none_fits,
                )
            })
            .flatten()
            .or_else(|| {
                self.split_by_reshaping(
                    prepared,
                    text,
                    segmenter,
                    letter_spacing,
                    word_spacing,
                    max_border_box_inline_size,
                    take_first_grapheme_when_none_fits,
                )
            })
    }

    pub(crate) fn soft_wrap_offsets<'a>(
        &self,
        prepared: &'a PreparedInlineContent,
    ) -> impl Iterator<Item = usize> + 'a {
        self.soft_wrap_offsets_after(prepared, self.offset_in_node)
    }

    fn soft_wrap_offsets_after<'a>(
        &self,
        prepared: &'a PreparedInlineContent,
        start: usize,
    ) -> impl Iterator<Item = usize> + 'a {
        let end = self.offset_in_node + self.length_in_node;
        let item_index = self.prepared_item_index;
        let wraps = if self.type_ == ItemType::Text && self.prepared_item(prepared).has(PreparedItem::SOFT_WRAPS) {
            let first = prepared
                .soft_wraps
                .partition_point(|(index, offset)| (*index, *offset as usize) <= (item_index, start));
            &prepared.soft_wraps[first..]
        } else {
            &[]
        };
        wraps
            .iter()
            .take_while(move |(index, _)| *index == item_index)
            .map(|(_, offset)| *offset as usize)
            .filter(move |offset| *offset > start && *offset < end)
    }

    #[inline]
    pub(crate) fn has_soft_wraps(&self, prepared: &PreparedInlineContent) -> bool {
        if prepared.soft_wraps.is_empty()
            || self.type_ != ItemType::Text
            || !self.prepared_item(prepared).has(PreparedItem::SOFT_WRAPS)
        {
            return false;
        }
        self.soft_wrap_offsets(prepared).next().is_some()
    }

    pub(crate) fn soft_wrap_segment_inline_sizes(
        &self,
        prepared: &PreparedInlineContent,
        text: &[u16],
        letter_spacing: f32,
        word_spacing: f32,
        hyphenates: bool,
        breaks_after_item: bool,
    ) -> Vec<CssPixels> {
        let item_end = self.offset_in_node + self.length_in_node;
        let mut start = self.offset_in_node;
        let item = self.prepared_item(prepared);
        let mut hyphen_width = None;
        self.soft_wrap_offsets(prepared)
            .chain(std::iter::once(start + self.length_in_node))
            .map(|end| {
                let mut width = libgfx_rust::text_layout::shaped_text_width(
                    prepared.font_of(item),
                    &text[start..end],
                    libgfx_rust::text_layout::TextType::try_from(item.text_type())
                        .expect("invalid Gfx::GlyphRun::TextType"),
                    letter_spacing,
                    word_spacing,
                    text_chunker::joining_context(text, start..end),
                    item.has(PreparedItem::UPRIGHT),
                );
                // NB: A segment that ends in a soft hyphen ends its line in a hyphen, unless it ends the item and no break
                //     follows the item.
                if hyphenates
                    && end > start
                    && (end < item_end || breaks_after_item)
                    && text_chunker::breaks_at_soft_hyphen(text, end)
                {
                    width += *hyphen_width.get_or_insert_with(|| {
                        hyphen_glyph_run(prepared.font_of(item), item.text_type(), letter_spacing, word_spacing).width
                    });
                }
                start = end;
                CssPixels::nearest_value_for_f32(width)
            })
            .collect()
    }

    fn reshaped_range(
        &self,
        prepared: &PreparedInlineContent,
        text: &[u16],
        range: std::ops::Range<usize>,
        letter_spacing: f32,
        word_spacing: f32,
    ) -> OwnedGlyphRun {
        let item = self.prepared_item(prepared);
        reshaped_glyph_run(
            text,
            range,
            prepared.font_of(item),
            item.text_type(),
            letter_spacing,
            word_spacing,
            item.has(PreparedItem::UPRIGHT),
        )
    }

    pub(crate) fn reshaped_fragment(
        &self,
        prepared: &PreparedInlineContent,
        text: &[u16],
        range: std::ops::Range<usize>,
        letter_spacing: f32,
        word_spacing: f32,
    ) -> Item {
        let end = self.offset_in_node + self.length_in_node;
        if range == (self.offset_in_node..end) {
            return self.clone();
        }
        let shape = self.reshaped_range(prepared, text, range.clone(), letter_spacing, word_spacing);
        let mut fragment = self.clone();
        fragment.offset_in_node = range.start;
        fragment.length_in_node = range.len();
        fragment.inline_size = CssPixels::nearest_value_for_f32(shape.width);
        fragment.owned_glyph_run = Some(Box::new(shape));
        if range.start != self.offset_in_node {
            fragment.margin_start = CssPixels::default();
            fragment.border_start = CssPixels::default();
            fragment.padding_start = CssPixels::default();
            fragment.entered_right_to_left_end_margin = CssPixels::default();
            fragment.can_break_before = false;
        }
        if range.end != end {
            fragment.margin_end = CssPixels::default();
            fragment.border_end = CssPixels::default();
            fragment.padding_end = CssPixels::default();
            fragment.exited_right_to_left_end_margin = CssPixels::default();
            fragment.trailing_whitespace = Default::default();
        }
        fragment
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn fragment_at_soft_wrap(
        &self,
        prepared: &PreparedInlineContent,
        text: &[u16],
        start: usize,
        letter_spacing: f32,
        word_spacing: f32,
        available: CssPixels,
        following_inline_size: CssPixels,
        hyphenates: bool,
    ) -> Item {
        let end = self.offset_in_node + self.length_in_node;
        let mut chosen = None;
        let mut hyphen_inline_size = None;
        for offset in self
            .soft_wrap_offsets_after(prepared, start)
            .chain(std::iter::once(end))
        {
            let fragment = self.reshaped_fragment(prepared, text, start..offset, letter_spacing, word_spacing);
            let following = if offset == end {
                following_inline_size
            } else if hyphenates && text_chunker::breaks_at_soft_hyphen(text, offset) {
                // NB: A line that breaks after a soft hyphen ends in a hyphen, which has to fit too.
                *hyphen_inline_size.get_or_insert_with(|| {
                    let prepared_item = self.prepared_item(prepared);
                    let run = hyphen_glyph_run(
                        prepared.font_of(prepared_item),
                        prepared_item.text_type(),
                        letter_spacing,
                        word_spacing,
                    );
                    CssPixels::nearest_value_for_f32(run.width)
                })
            } else {
                CssPixels::default()
            };
            if inline_extent([&fragment]) + following > available {
                return chosen.unwrap_or(fragment);
            }
            chosen = Some(fragment);
        }
        chosen.expect("a text fragment ends at a soft wrap or the end of its item")
    }

    pub(crate) fn drop_prefix_before(
        &mut self,
        prepared: &PreparedInlineContent,
        split_offset_in_node: usize,
        text: &[u16],
        letter_spacing: f32,
        word_spacing: f32,
    ) {
        let end = self.offset_in_node + self.length_in_node;
        if split_offset_in_node <= self.offset_in_node || split_offset_in_node >= end || self.type_ != ItemType::Text {
            return;
        }
        let remainder_glyphs = (!self.prepared_item(prepared).has(PreparedItem::SOFT_WRAPS))
            .then(|| self.shaped_glyphs_after(prepared, split_offset_in_node - self.offset_in_node))
            .flatten()
            .unwrap_or_else(|| {
                let prepared_item = self.prepared_item(prepared);
                reshaped_glyph_run(
                    text,
                    split_offset_in_node..end,
                    prepared.font_of(prepared_item),
                    prepared_item.text_type(),
                    letter_spacing,
                    word_spacing,
                    prepared_item.has(PreparedItem::UPRIGHT),
                )
            });
        self.drop_prefix(split_offset_in_node, remainder_glyphs);
    }

    fn shaped_glyphs_after(&self, prepared: &PreparedInlineContent, code_units: usize) -> Option<OwnedGlyphRun> {
        let run = self.glyph_run(prepared)?;
        if run.text_type == line_box_fragment::GLYPH_TEXT_TYPE_RTL
            || run
                .glyphs
                .iter()
                .map(|glyph| glyph.length_in_code_units())
                .sum::<usize>()
                != self.length_in_node
        {
            return None;
        }
        let mut consumed = 0;
        let split_glyph_index = run.glyphs.iter().position(|glyph| {
            let at_split = consumed == code_units && glyph.length_in_code_units() != 0;
            consumed += glyph.length_in_code_units();
            at_split
        })?;
        let split_x = run.glyphs.placed().nth(split_glyph_index)?.x;
        Some(OwnedGlyphRun {
            glyphs: run.glyphs.remainder_from(split_glyph_index),
            width: run.width - split_x,
            is_upright: run.is_upright,
        })
    }

    fn take_prefix_before(
        &mut self,
        split_offset_in_node: usize,
        prefix_glyphs: OwnedGlyphRun,
        remainder_glyphs: OwnedGlyphRun,
    ) -> Item {
        let mut prefix = Item::new(ItemType::Text, self.node, self.prepared_item_index as usize);
        prefix.offset_in_node = self.offset_in_node;
        prefix.length_in_node = split_offset_in_node - self.offset_in_node;
        prefix.inline_size = CssPixels::nearest_value_for_f32(prefix_glyphs.width);
        prefix.margin_start = self.margin_start;
        prefix.border_start = self.border_start;
        prefix.padding_start = self.padding_start;
        prefix.entered_right_to_left_end_margin = self.entered_right_to_left_end_margin;
        prefix.can_break_before = self.can_break_before;
        prefix.owned_glyph_run = Some(Box::new(prefix_glyphs));
        self.drop_prefix(split_offset_in_node, remainder_glyphs);
        prefix
    }

    fn drop_prefix(&mut self, split_offset_in_node: usize, remainder_glyphs: OwnedGlyphRun) {
        self.length_in_node -= split_offset_in_node - self.offset_in_node;
        self.offset_in_node = split_offset_in_node;
        self.inline_size = CssPixels::nearest_value_for_f32(remainder_glyphs.width);
        self.owned_glyph_run = Some(Box::new(remainder_glyphs));
        self.margin_start = CssPixels::default();
        self.border_start = CssPixels::default();
        self.padding_start = CssPixels::default();
        self.entered_right_to_left_end_margin = CssPixels::default();
        self.can_break_before = false;
    }

    fn split_at_shaped_glyph(
        &mut self,
        prepared: &PreparedInlineContent,
        segmenter: &text_chunker::GraphemeSegmenter,
        max_border_box_inline_size: CssPixels,
        take_first_grapheme_when_none_fits: bool,
    ) -> Option<Item> {
        let run = self.glyph_run(prepared)?;
        if run.glyphs.len() < 2 {
            return None;
        }
        let shaped_length_in_code_units: usize = run.glyphs.iter().map(|glyph| glyph.length_in_code_units()).sum();
        if shaped_length_in_code_units != self.length_in_node {
            return None;
        }
        if run.text_type == line_box_fragment::GLYPH_TEXT_TYPE_RTL {
            return self.split_right_to_left_at_shaped_glyph(
                prepared,
                segmenter,
                max_border_box_inline_size,
                take_first_grapheme_when_none_fits,
            );
        }

        let leading_edge_inline_size = self.margin_start + self.border_start + self.padding_start;
        let mut split: Option<(usize, usize)> = None;
        let mut code_units = run.glyphs[0].length_in_code_units();
        for (glyph_index, placed) in run.glyphs.placed().enumerate().skip(1) {
            if placed.glyph.length_in_code_units() == 0 {
                continue;
            }
            let offset = self.offset_in_node + code_units;
            if segmenter.next_boundary(offset, true) == Some(offset) {
                let prefix_inline_size = CssPixels::nearest_value_for_f32(placed.x);
                if leading_edge_inline_size + prefix_inline_size > max_border_box_inline_size {
                    if split.is_none() && take_first_grapheme_when_none_fits {
                        split = Some((glyph_index, code_units));
                    }
                    break;
                }
                split = Some((glyph_index, code_units));
            }
            code_units += placed.glyph.length_in_code_units();
        }
        let (split_glyph_index, split_code_units) = split?;

        let mut prefix = self.take_owned_glyph_run(prepared);
        let split_x = prefix.glyphs.placed().nth(split_glyph_index)?.x;
        let remainder_glyphs = prefix.glyphs.remainder_from(split_glyph_index);
        prefix.glyphs.truncate(split_glyph_index);
        let remainder = OwnedGlyphRun {
            glyphs: remainder_glyphs,
            width: prefix.width - split_x,
            is_upright: prefix.is_upright,
        };
        prefix.width = split_x;

        Some(self.take_prefix_before(self.offset_in_node + split_code_units, prefix, remainder))
    }

    /// Splits off the longest logical prefix of a right-to-left run that fits `max_border_box_inline_size`, from its
    /// shaped glyphs. Those are in visual order, so the logical prefix is the glyphs at the right end of the run.
    fn split_right_to_left_at_shaped_glyph(
        &mut self,
        prepared: &PreparedInlineContent,
        segmenter: &text_chunker::GraphemeSegmenter,
        max_border_box_inline_size: CssPixels,
        take_first_grapheme_when_none_fits: bool,
    ) -> Option<Item> {
        let run = self.glyph_run(prepared)?;
        let placed: Vec<_> = run.glyphs.placed().collect();
        // NB: The spacer glyphs of leading tabs come first, with ascending clusters, so in a run that has them, the
        //     logical prefix isn't the glyphs at the right end. Leave such a run to reshaping.
        if placed
            .windows(2)
            .any(|pair| pair[0].cluster_start < pair[1].cluster_start)
        {
            return None;
        }
        let leading_edge_inline_size = self.margin_start + self.border_start + self.padding_start;
        // NB: The prefix is the glyphs from the split on, and the cluster just left of the split starts where it ends.
        let mut split: Option<(usize, usize)> = None;
        for glyph_index in (1..placed.len()).rev() {
            let (left, right) = (&placed[glyph_index - 1], &placed[glyph_index]);
            if left.cluster_start <= right.cluster_start {
                continue;
            }
            let prefix_length = left.cluster_start as usize;
            let offset = self.offset_in_node + prefix_length;
            if segmenter.next_boundary(offset, true) != Some(offset) {
                continue;
            }
            let prefix_inline_size = CssPixels::nearest_value_for_f32(run.width - right.x);
            if leading_edge_inline_size + prefix_inline_size > max_border_box_inline_size {
                if split.is_none() && take_first_grapheme_when_none_fits {
                    split = Some((glyph_index, prefix_length));
                }
                break;
            }
            split = Some((glyph_index, prefix_length));
        }
        let (split_glyph_index, prefix_length) = split?;

        let run = self.take_owned_glyph_run(prepared);
        let split_x = run.glyphs.placed().nth(split_glyph_index)?.x;
        let mut prefix_glyphs = libgfx_rust::text_layout::GlyphBuffer::default();
        let mut remainder_glyphs = libgfx_rust::text_layout::GlyphBuffer::default();
        for (glyph_index, placed) in run.glyphs.placed().enumerate() {
            if glyph_index >= split_glyph_index {
                prefix_glyphs.push_placed(libgfx_rust::text_layout::PlacedGlyph {
                    x: placed.x - split_x,
                    ..placed
                });
            } else {
                remainder_glyphs.push_placed(libgfx_rust::text_layout::PlacedGlyph {
                    cluster_start: placed.cluster_start - prefix_length as u32,
                    ..placed
                });
            }
        }
        let prefix = OwnedGlyphRun {
            glyphs: prefix_glyphs,
            width: run.width - split_x,
            is_upright: run.is_upright,
        };
        let remainder = OwnedGlyphRun {
            glyphs: remainder_glyphs,
            width: split_x,
            is_upright: run.is_upright,
        };
        Some(self.take_prefix_before(self.offset_in_node + prefix_length, prefix, remainder))
    }

    #[allow(clippy::too_many_arguments)]
    fn split_by_reshaping(
        &mut self,
        prepared: &PreparedInlineContent,
        text: &[u16],
        segmenter: &text_chunker::GraphemeSegmenter,
        letter_spacing: f32,
        word_spacing: f32,
        max_border_box_inline_size: CssPixels,
        take_first_grapheme_when_none_fits: bool,
    ) -> Option<Item> {
        if self.type_ != ItemType::Text {
            return None;
        }
        let prepared_item = self.prepared_item(prepared);
        let text_type = prepared_item.text_type();
        // NB: A right-to-left run splits at a grapheme boundary from its logical start too, so the part that fits is the
        //     run's logical beginning, which the line then draws at its right end.
        let font = prepared.font_of(prepared_item);
        let is_upright = prepared_item.has(PreparedItem::UPRIGHT);
        let start = self.offset_in_node;
        let end = start + self.length_in_node;

        let mut boundaries = Vec::with_capacity(end - start);
        let mut boundary = segmenter.next_boundary(start, false)?;
        while boundary < end {
            boundaries.push(boundary);
            let Some(next) = segmenter.next_boundary(boundary, false) else {
                break;
            };
            boundary = next;
        }
        if boundaries.is_empty() {
            return None;
        }

        let shape = |range: std::ops::Range<usize>| {
            reshaped_glyph_run(text, range, font, text_type, letter_spacing, word_spacing, is_upright)
        };

        let leading_edge_inline_size = self.margin_start + self.border_start + self.padding_start;
        let prefix_fits = |boundary: usize| {
            let width = libgfx_rust::text_layout::shaped_text_width(
                font,
                &text[start..boundary],
                libgfx_rust::text_layout::TextType::try_from(text_type).expect("invalid Gfx::GlyphRun::TextType"),
                letter_spacing,
                word_spacing,
                text_chunker::joining_context(text, start..boundary),
                is_upright,
            );
            leading_edge_inline_size + CssPixels::nearest_value_for_f32(width) <= max_border_box_inline_size
        };
        let mut fitting_count = boundaries.partition_point(|boundary| prefix_fits(*boundary));
        if fitting_count == 0 {
            if !take_first_grapheme_when_none_fits {
                return None;
            }
            fitting_count = 1;
        }
        let split = boundaries[fitting_count - 1];

        Some(self.take_prefix_before(split, shape(start..split), shape(split..end)))
    }

    pub(crate) fn first_grapheme_cluster_inline_size(
        &self,
        prepared: &PreparedInlineContent,
        text: &[u16],
        segmenter: &text_chunker::GraphemeSegmenter,
        letter_spacing: f32,
        word_spacing: f32,
    ) -> Option<CssPixels> {
        assert_eq!(self.type_, ItemType::Text);
        let run = self.glyph_run(prepared)?;
        let start = self.offset_in_node;
        let end = start + self.length_in_node;
        let boundary = segmenter
            .next_boundary(start, false)
            .filter(|boundary| *boundary < end)?;
        if run.text_type != line_box_fragment::GLYPH_TEXT_TYPE_RTL
            && let Some(first_glyph) = run.glyphs.first()
        {
            let mut code_units = first_glyph.length_in_code_units();
            for placed in run.glyphs.placed().skip(1) {
                if placed.glyph.length_in_code_units() == 0 {
                    continue;
                }
                let offset = start + code_units;
                if offset > boundary {
                    break;
                }
                if offset == boundary {
                    return Some(CssPixels::nearest_value_for_f32(placed.x));
                }
                code_units += placed.glyph.length_in_code_units();
            }
        }
        let width = libgfx_rust::text_layout::shaped_text_width(
            run.font,
            &text[start..boundary],
            libgfx_rust::text_layout::TextType::try_from(run.text_type).expect("invalid Gfx::GlyphRun::TextType"),
            letter_spacing,
            word_spacing,
            text_chunker::joining_context(text, start..boundary),
            run.is_upright,
        );
        Some(CssPixels::nearest_value_for_f32(width))
    }

    pub(crate) fn grapheme_cluster_inline_sizes(
        &self,
        prepared: &PreparedInlineContent,
        text: &[u16],
        segmenter: &text_chunker::GraphemeSegmenter,
        letter_spacing: f32,
        word_spacing: f32,
    ) -> Vec<CssPixels> {
        assert_eq!(self.type_, ItemType::Text);
        let mut remainder = self.clone();
        let mut sizes = Vec::new();
        while remainder.border_box_inline_size() > CssPixels::default()
            && let Some(prefix) = remainder.split_for_overflow_break(
                prepared,
                text,
                segmenter,
                letter_spacing,
                word_spacing,
                CssPixels::default(),
                true,
            )
        {
            sizes.push(prefix.inline_size);
        }
        sizes.push(remainder.inline_size);
        sizes
    }
}

#[derive(Clone, Copy, Default)]
struct ExtraBoxMetrics {
    margin: CssPixels,
    border: CssPixels,
    padding: CssPixels,
}

#[derive(Clone, Copy)]
enum InlineEdgeBoundary {
    BeforeBlock(Node),
    BeforeBreak(Option<Node>),
    Closing(Node, Option<Node>),
    End,
}

struct PendingInlineEdge {
    node: Node,
    margin: CssPixels,
    border: CssPixels,
    padding: CssPixels,
    physical: ExtraBoxMetrics,
    right_to_left_end_margin: CssPixels,
    is_start: bool,
    is_low: bool,
    takes_leading_metrics: bool,
}

/// An inline box that holds no in-flow item.
#[derive(Clone, Copy)]
pub(crate) struct EmptyInlineBox {
    /// Where the box is among the items: the index of the item it comes before or, once the items are resolved, the
    /// number of items from that one to the last one.
    position: usize,
    pub(crate) node: Node,
    /// The start edges between the content before the box and the content of the box, which the item after them
    /// takes: those of the box, of the boxes around it that begin with it, and the end edges of empty boxes before it.
    pub(crate) leading_edges: CssPixels,
}

struct TextNodeContext<'pass> {
    chunks: std::sync::Arc<super::rendered_text::CachedTextChunks>,
    text: &'pass [u16],
    next_chunk_index: usize,
    independent_chunks_until: usize,
    can_memoize_independent_chunks: bool,
    style: StyleValues<'pass>,
    letter_spacing: f32,
    word_spacing: f32,
    writing_mode: u8,
    text_orientation: u8,
    should_wrap_lines: bool,
    should_collapse_whitespace: bool,
    should_respect_linebreaks: bool,
    is_generated_for_pseudo_element: bool,
    produces_line_box_fragment_when_empty: bool,
    is_text_control_text: bool,
    last_known_direction: Option<u8>,
    /// The direction of the first chunk with a strong direction from the chunk at the first index on, which is the
    /// chunk at the second index.
    next_known_direction: std::cell::Cell<Option<(usize, usize, Option<u8>)>>,
    /// The direction each bracket that is open at this point in the paragraph took, if it took one.
    open_bracket_directions: std::cell::RefCell<Vec<Option<u8>>>,
}

impl TextNodeContext<'_> {
    /// The direction of the first chunk with a strong direction among the chunks not yet taken.
    fn next_known_direction(&self) -> Option<u8> {
        // NB: The answer holds until the chunk that gave it is taken, so a run of neutral chunks looks ahead once.
        if let Some((from, until, direction)) = self.next_known_direction.get()
            && (from..=until).contains(&self.next_chunk_index)
        {
            return direction;
        }
        let found = self
            .chunks
            .iter()
            .enumerate()
            .skip(self.next_chunk_index)
            .find_map(|(index, chunk)| {
                // NB: A preserved newline ends the paragraph, and the text after it has no say in this one.
                if self.should_respect_linebreaks && chunk.has_breaking_newline {
                    return Some((index, None));
                }
                matches!(
                    chunk.text_type,
                    line_box_fragment::GLYPH_TEXT_TYPE_LTR | line_box_fragment::GLYPH_TEXT_TYPE_RTL
                )
                .then_some((index, Some(chunk.text_type)))
            });
        let (until, direction) = found.unwrap_or((usize::MAX, None));
        self.next_known_direction
            .set(Some((self.next_chunk_index, until, direction)));
        direction
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum AtomicInlineSizing {
    Layout,
    Contributions,
}

struct InlineContentPreparer<'pass> {
    pass: LayoutPass<'pass>,
    block_container: Node,
    buffers: InlineContentPreparationBuffers,
    current_node: Node,
    next_node: Node,
    text_node_context: Option<TextNodeContext<'pass>>,
    is_unidirectional_left_to_right: bool,
    walks_within_item: bool,
    previous_chunk_can_break_after: bool,
    /// The text of the text node before the current one, when nothing but inline box boundaries and out-of-flow
    /// boxes lies between them.
    previous_text: Option<&'pass [u16]>,
    /// The text node that `previous_text` comes from, or the atomic inline that it stands for.
    previous_node: Option<Node>,
    /// Whether the previous text wraps with `word-break: break-all`.
    previous_text_breaks_all: bool,
    /// The number of regional indicators that end the previous text, including any in the text nodes before it.
    previous_text_regional_indicators: usize,
    /// The number of regional indicators that end the text before the current text node.
    regional_indicators_before_text: usize,
    /// The last code unit of the text before the previous text.
    code_unit_before_previous_text: Option<u16>,
    /// The last code unit of the text before the current text node.
    code_unit_before_text: Option<u16>,
}

impl PreparedInlineContent {
    fn orders_atomic_items_right_to_left(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> bool {
        let style = context.style(context.containing_block);
        if style.direction() != direction::RTL || style.unicode_bidi() == unicode_bidi::PLAINTEXT {
            return false;
        }
        if self
            .box_events
            .iter()
            .filter_map(InlineBoxEvent::entered_box)
            .any(|node| {
                let style = context.style(node);
                style.unicode_bidi() != unicode_bidi::NORMAL || style.direction() != direction::RTL
            })
        {
            return false;
        }

        // https://drafts.csswg.org/css-writing-modes-4/#bidi-atomic-inlines
        // All other atomic inline-level boxes are treated as neutral characters always.
        // FIXME: Resolve paragraph-wide bidi levels for mixed text and atomic boxes, and for explicit embeddings.
        let mut has_atomic_box = false;
        self.items.iter().all(|item| match item.type_() {
            ItemType::Element => {
                let facts = context.facts(item.node());
                if !facts.is_atomic_inline()
                    || (facts.is_replaced_box()
                        && matches!(
                            context.style(item.node()).unicode_bidi(),
                            unicode_bidi::EMBED | unicode_bidi::BIDI_OVERRIDE | unicode_bidi::ISOLATE_OVERRIDE
                        ))
                {
                    return false;
                }
                has_atomic_box = true;
                true
            }
            ItemType::Text => item.has(PreparedItem::TRAILING_WHITESPACE_IS_WHOLE_ITEM),
            ItemType::ForcedBreak | ItemType::AbsolutelyPositionedElement | ItemType::FloatingElement => true,
            ItemType::BlockLevelBox | ItemType::InlineEdge => false,
        }) && has_atomic_box
    }

    pub(super) fn prepare(pass: LayoutPass<'_>, block_container: Node) -> Self {
        InlineContentPreparer::prepare(pass, block_container)
    }

    pub(crate) fn resolve_items_for_run(
        self: std::sync::Arc<Self>,
        context: &mut inline_formatting_context::InlineFormattingContext<'_>,
        atomic_sizing: AtomicInlineSizing,
    ) -> InlineLevelIterator {
        context.atomic_items_are_right_to_left = self.orders_atomic_items_right_to_left(context);
        let (entered_box_model_nodes, empty_inline_boxes, items) = RunItemResolver {
            context,
            prepared: &self,
            atomic_sizing,
            pending_inline_edges: Vec::new(),
            box_model_node_stack: Vec::new(),
            entered_box_model_nodes: Vec::new(),
            in_flow_item_count: 0,
            item_count: 0,
            empty_inline_boxes: Vec::new(),
            empty_inline_boxes_after_item: Vec::new(),
            inline_boxes_ending_with_item: Vec::new(),
            accumulated_inline_size_for_tabs: CssPixels::default(),
            next_box_event: 0,
            next_tab_stop: 0,
            forced_break_owner: NodeSlotId::INVALID,
        }
        .resolve();
        let item_count = items.len();
        InlineLevelIterator {
            entered_box_model_nodes,
            empty_inline_boxes: empty_inline_boxes
                .into_iter()
                .map(|empty_box| EmptyInlineBox {
                    position: item_count - empty_box.position,
                    ..empty_box
                })
                .collect(),
            items: ItemQueue::Owned(items.into_iter()),
            items_kept_for_looking_ahead: 0,
            prepared: self,
        }
    }
}

impl<'pass> InlineContentPreparer<'pass> {
    fn prepare(pass: LayoutPass<'pass>, block_container: Node) -> PreparedInlineContent {
        let mut preparer = Self {
            pass,
            block_container,
            buffers: pass.take_inline_content_preparation_buffers(),
            next_node: pass.first_child(block_container),
            current_node: NodeSlotId::INVALID,
            text_node_context: None,
            is_unidirectional_left_to_right: false,
            walks_within_item: false,
            previous_chunk_can_break_after: false,
            previous_text: None,
            previous_node: None,
            previous_text_breaks_all: false,
            previous_text_regional_indicators: 0,
            regional_indicators_before_text: 0,
            code_unit_before_previous_text: None,
            code_unit_before_text: None,
        };
        preparer.is_unidirectional_left_to_right = preparer.compute_is_unidirectional_left_to_right();
        preparer.skip_to_next();
        while let Some(item) = preparer.prepare_next_item() {
            preparer.buffers.items.push(item);
        }
        let prepared = preparer
            .buffers
            .take_prepared_inline_content(super::fc_run_cache::run_root_validity(&pass, block_container));
        pass.return_inline_content_preparation_buffers(preparer.buffers);
        prepared
    }

    fn style(&self, node: Node) -> StyleValues<'pass> {
        StyleValues::for_node(&self.pass, node)
    }

    /// The nearest node that's an ancestor of both `a` and `b`.
    fn nearest_common_ancestor(&self, a: Node, b: Node) -> Node {
        let mut ancestor = self.pass.parent(b);
        while !ancestor.is_invalid() {
            let mut node = a;
            while !node.is_invalid() && node != ancestor {
                node = self.pass.parent(node);
            }
            if node == ancestor {
                return ancestor;
            }
            ancestor = self.pass.parent(ancestor);
        }
        self.pass.parent(b)
    }

    fn facts(&self, node: Node) -> NodeFacts<'pass> {
        NodeFacts::new(&self.pass, node)
    }

    fn is_out_of_flow(&self, node: Node) -> bool {
        self.facts(node).is_floating_or_absolutely_positioned()
    }

    fn compute_is_unidirectional_left_to_right(&mut self) -> bool {
        let containing_block = self.block_container;
        if self.style(containing_block).direction() == direction::RTL {
            return false;
        }

        let mut node = self.pass.first_child(containing_block);
        while !node.is_invalid() {
            let facts = self.facts(node);
            if !facts.is_text_node() {
                let style = self.style(node);
                if style.direction() == direction::RTL || style.unicode_bidi() != unicode_bidi::NORMAL {
                    return false;
                }
            } else if self.pass.text_content(node).may_require_bidi_processing {
                return false;
            }

            loop {
                node = self.next_inline_node_in_pre_order(node, containing_block);
                if !node.is_invalid() && self.facts(node).is_svg_mask_box() {
                    node = self.pass.next_sibling(node);
                }
                if node.is_invalid() {
                    break;
                }
                let facts = self.facts(node);
                if facts.is_inline() || self.is_out_of_flow(node) || facts.is_inline_flow_interrupting_block() {
                    break;
                }
            }
        }
        true
    }

    fn inline_box_event_position(&self) -> u32 {
        InlineBoxEvent::position_at(self.buffers.items.len(), self.walks_within_item)
    }

    fn enter_node_with_box_model_metrics(&mut self, node: Node) {
        let event = InlineBoxEvent::entering(self.inline_box_event_position(), node);
        self.buffers.box_events.push(event);
        self.buffers.box_stack.push(node);
    }

    fn exit_node_with_box_model_metrics(&mut self) {
        self.buffers.box_stack.pop().unwrap();
        // The walk looks one node ahead, so a box usually ends while its last item is the current node. An inline box
        // or an empty text node without a caret fragment has no item of its own. Its end edges precede the next item
        // rather than follow that item's content.
        let before_item = !self.current_node.is_invalid() && {
            let facts = self.facts(self.current_node);
            facts.is_fragmented_inline()
                || (facts.is_text_node()
                    && !facts.produces_line_box_fragment_when_empty()
                    && self.pass.text_content(self.current_node).text.is_empty())
        };
        // NB: A wbr has no item, so a box that ends right after one ends with the item before the wbr instead, and its
        //     end edges stay on the line of that item when the line breaks at the wbr.
        let mut position = self.inline_box_event_position();
        if !self.current_node.is_invalid()
            && self.facts(self.current_node).is_word_break_opportunity_node()
            && let Some(previous_item_index) = self.buffers.items.len().checked_sub(1)
        {
            let previous_item_position = InlineBoxEvent::position_at(previous_item_index, false);
            if self
                .buffers
                .box_events
                .last()
                .is_none_or(|event| event.position() <= previous_item_position)
            {
                position = previous_item_position;
            }
        }
        let event = InlineBoxEvent::leaving_innermost_box(position, before_item);
        self.buffers.box_events.push(event);
    }

    fn next_inline_node_in_pre_order(&mut self, current: Node, stay_within: Node) -> Node {
        let first_child = self.pass.first_child(current);
        let current_facts = self.facts(current);
        if !first_child.is_invalid() {
            let child_facts = self.facts(first_child);
            let current_style = self.style(current);
            if (child_facts.is_inline()
                || child_facts.is_inline_flow_interrupting_block()
                || self.is_out_of_flow(first_child))
                && current_style.display().is_flow_inside()
                && !current_facts.is_inline_flow_interrupting_block()
                && !current_facts.is_atomic_inline()
                && (!current_facts.is_box() || !self.is_out_of_flow(current))
            {
                return first_child;
            }
        }

        let mut node = current;
        loop {
            if self.buffers.box_stack.last().copied() == Some(node) {
                self.exit_node_with_box_model_metrics();
            }
            let next = self.pass.next_sibling(node);
            if !next.is_invalid() {
                return next;
            }
            node = self.pass.parent(node);
            if node.is_invalid() || node == stay_within {
                return NodeSlotId::INVALID;
            }
        }
    }

    fn compute_next(&mut self) {
        if self.next_node.is_invalid() {
            return;
        }
        loop {
            self.next_node = self.next_inline_node_in_pre_order(self.next_node, self.block_container);
            if !self.next_node.is_invalid() && self.facts(self.next_node).is_svg_mask_box() {
                self.next_node = self.pass.next_sibling(self.next_node);
            }
            if self.next_node.is_invalid() {
                break;
            }
            let facts = self.facts(self.next_node);
            if facts.is_inline() || self.is_out_of_flow(self.next_node) || facts.is_inline_flow_interrupting_block() {
                break;
            }
        }
    }

    fn skip_to_next(&mut self) {
        if !self.next_node.is_invalid() {
            let facts = self.facts(self.next_node);
            if facts.is_inline()
                && facts.has_box_model_metrics()
                && !facts.is_break_node()
                && !facts.is_word_break_opportunity_node()
                && self.style(self.next_node).display().is_flow_inside()
                && !self.is_out_of_flow(self.next_node)
                && !facts.is_atomic_inline()
            {
                self.enter_node_with_box_model_metrics(self.next_node);
            }
        }
        self.current_node = self.next_node;
        self.compute_next();
    }

    fn skip_to_next_within_item(&mut self) {
        self.walks_within_item = true;
        self.skip_to_next();
        self.walks_within_item = false;
    }

    fn enter_text_node(&mut self, text_node: Node) {
        let style = self.style(self.pass.parent(text_node));
        let should_wrap_lines = style.text_wrap_mode() == text_wrap_mode::WRAP;
        let should_respect_linebreaks = matches!(
            style.white_space_collapse(),
            white_space_collapse::PRESERVE | white_space_collapse::PRESERVE_BREAKS | white_space_collapse::BREAK_SPACES
        );
        let should_collapse_whitespace = matches!(
            style.white_space_collapse(),
            white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
        );
        let chunks = text_chunker::text_chunks(
            &self.pass,
            text_node,
            should_wrap_lines,
            should_respect_linebreaks,
            self.is_unidirectional_left_to_right,
        );
        let facts = self.facts(text_node);
        let letter_spacing = style.letter_spacing().to_double() as f32;
        let word_spacing = style.word_spacing().to_double() as f32;
        let can_memoize_independent_chunks =
            self.is_unidirectional_left_to_right && style.writing_mode() == writing_mode::HORIZONTAL_TB;
        let independent_chunks_until = if can_memoize_independent_chunks
            && chunks.independent_chunks_for_spacing.get() == Some(&(letter_spacing.to_bits(), word_spacing.to_bits()))
        {
            usize::MAX
        } else {
            0
        };
        self.text_node_context = Some(TextNodeContext {
            chunks,
            text: &self.pass.text_content(text_node).text,
            next_chunk_index: 0,
            independent_chunks_until,
            can_memoize_independent_chunks,
            style,
            letter_spacing,
            word_spacing,
            writing_mode: style.writing_mode(),
            text_orientation: style.text_orientation(),
            should_wrap_lines,
            should_collapse_whitespace,
            should_respect_linebreaks,
            is_generated_for_pseudo_element: facts.is_generated_for_pseudo_element(),
            produces_line_box_fragment_when_empty: facts.produces_line_box_fragment_when_empty(),
            is_text_control_text: facts.is_text_control_text(),
            last_known_direction: None,
            next_known_direction: Default::default(),
            open_bracket_directions: Default::default(),
        });
    }

    fn resolve_text_direction_from_context(&self, context: &TextNodeContext<'_>) -> u8 {
        let next_known_direction = context.next_known_direction();
        if let (Some(last), Some(next)) = (context.last_known_direction, next_known_direction)
            && last != next
        {
            return match self.style(self.block_container).direction() {
                direction::LTR => line_box_fragment::GLYPH_TEXT_TYPE_LTR,
                direction::RTL => line_box_fragment::GLYPH_TEXT_TYPE_RTL,
                _ => unreachable!(),
            };
        }
        context
            .last_known_direction
            .or(next_known_direction)
            .unwrap_or(line_box_fragment::GLYPH_TEXT_TYPE_CONTEXT_DEPENDENT)
    }

    // https://www.unicode.org/reports/tr9/#N1
    // A sequence of NIs takes the direction of the surrounding strong text if the text on both sides has the same
    // direction.
    // NB: Otherwise the sequence keeps its neutral type, which takes the direction of the fragment it lands in.
    // FIXME: Look at the strong text in the other text nodes of the paragraph as well.
    fn direction_of_strong_text_around(&self, context: &TextNodeContext<'_>, neutral_text: &[u16]) -> Option<u8> {
        let next_known_direction = context.next_known_direction();
        let direction_of_text_around = context
            .last_known_direction
            .filter(|last| Some(*last) == next_known_direction);

        // NB: The two brackets of a pair get one direction, as in https://www.unicode.org/reports/tr9/#N0. So a
        //     closing bracket takes what its opening bracket took: the direction of the strong text before the pair
        //     and at the start of it, where those agree.
        // FIXME: Look at all the strong text inside the pair, and pair brackets across text nodes.
        let mut open_bracket_directions = context.open_bracket_directions.borrow_mut();
        let mut direction_of_first_bracket = None;
        for code_unit in neutral_text {
            let direction = match u8::try_from(*code_unit) {
                Ok(b'(' | b'[' | b'{') => {
                    open_bracket_directions.push(direction_of_text_around);
                    direction_of_text_around
                }
                Ok(b')' | b']' | b'}') => open_bracket_directions.pop().flatten(),
                _ => continue,
            };
            direction_of_first_bracket.get_or_insert(direction);
        }
        direction_of_first_bracket.unwrap_or(direction_of_text_around)
    }

    /// The text node's next item, or none once it has no more and the preparer has moved past it.
    fn prepare_text_item(&mut self, text_node: Node) -> Option<PreparedItem> {
        if self.text_node_context.is_none() {
            self.enter_text_node(text_node);
        }
        let mut text_context = self.text_node_context.take().unwrap();
        let chunks = &text_context.chunks;
        let is_first_chunk = text_context.next_chunk_index == 0;
        let chunk = chunks.get(text_context.next_chunk_index);
        if chunk.is_some() {
            text_context.next_chunk_index += 1;
        }
        let mut is_last_chunk = text_context.next_chunk_index >= chunks.len();
        let synthesize_zero_length_chunk = chunk.is_none()
            && is_first_chunk
            && is_last_chunk
            && text_context.text.is_empty()
            && text_context.produces_line_box_fragment_when_empty;
        // INTEROP: A text control's text that ends in a preserved newline keeps the empty line after it, which gives
        //          that line its height and the caret on it a fragment.
        let synthesize_chunk_after_trailing_newline = chunk.is_none()
            && text_context.next_chunk_index == chunks.len()
            && text_context.should_respect_linebreaks
            && chunks.last().is_some_and(|last| last.has_breaking_newline)
            && text_context.is_text_control_text;
        let (mut chunk, font) = if let Some(chunk) = chunk {
            (*chunk, chunks.font_of(chunk))
        } else if synthesize_zero_length_chunk || synthesize_chunk_after_trailing_newline {
            text_context.next_chunk_index = chunks.len() + 1;
            let chunk = text_chunker::TextChunk {
                start: text_context.text.len(),
                length: 0,
                font_index: text_chunker::TextChunk::FONT_INDEX_OUTSIDE_ANY_CHUNKED_TEXT,
                has_breaking_newline: false,
                has_breaking_tab: false,
                is_all_whitespace: true,
                can_break_after: false,
                text_type: line_box_fragment::GLYPH_TEXT_TYPE_COMMON,
            };
            let first_available_font = text_context
                .style
                .font()
                .first_available_font()
                .expect("layout read a font group that never received a frozen font list");
            (chunk, first_available_font)
        } else {
            if text_context.can_memoize_independent_chunks {
                let _ = chunks.independent_chunks_for_spacing.set((
                    text_context.letter_spacing.to_bits(),
                    text_context.word_spacing.to_bits(),
                ));
            }
            if !text_context.text.is_empty() {
                let regional_indicators = text_chunker::trailing_regional_indicator_count(text_context.text);
                // NB: Each regional indicator is a surrogate pair.
                self.previous_text_regional_indicators = if regional_indicators * 2 == text_context.text.len() {
                    self.regional_indicators_before_text + regional_indicators
                } else {
                    regional_indicators
                };
                self.code_unit_before_previous_text = self.code_unit_before_text;
                self.previous_text = Some(text_context.text);
                self.previous_node = Some(text_node);
                self.previous_text_breaks_all =
                    text_context.should_wrap_lines && text_context.style.word_break() == word_break::BREAK_ALL;
            }
            self.text_node_context = None;
            self.previous_chunk_can_break_after = false;
            self.skip_to_next();
            return None;
        };

        // A possible line break doesn't end the shaping context. Keep compatible non-whitespace chunks together,
        // and retain their break offsets so that line fitting can reshape the text at a break it actually takes.
        let first_chunk = chunk;
        let next_chunk_index = text_context.next_chunk_index;
        let first_soft_wrap = self.buffers.soft_wraps.len();
        let mut has_soft_wraps = false;
        while next_chunk_index >= text_context.independent_chunks_until
            && !chunk.is_all_whitespace
            && !chunk.has_breaking_newline
            && !chunk.has_breaking_tab
        {
            let Some(next) = chunks.get(text_context.next_chunk_index) else {
                break;
            };
            if next.is_all_whitespace
                || next.has_breaking_newline
                || next.has_breaking_tab
                || next.font_index != chunk.font_index
                || next.text_type != chunk.text_type
                || next.start != chunk.start + chunk.length
                || text_is_set_upright(
                    text_context.writing_mode,
                    text_context.text_orientation,
                    text_chunker::code_point_at(text_context.text, chunk.start),
                )
                || text_is_set_upright(
                    text_context.writing_mode,
                    text_context.text_orientation,
                    text_chunker::code_point_at(text_context.text, next.start),
                )
            {
                break;
            }
            if chunk.can_break_after {
                self.buffers.soft_wraps.push((
                    u32::try_from(self.buffers.items.len()).expect("item indices fit in 32 bits"),
                    u32::try_from(next.start).expect("text offsets fit in 32 bits"),
                ));
                has_soft_wraps = true;
            }
            chunk.length += next.length;
            chunk.can_break_after = next.can_break_after;
            text_context.next_chunk_index += 1;
        }
        is_last_chunk = text_context.next_chunk_index >= chunks.len();

        let mut text_type = chunk.text_type;
        if matches!(
            text_type,
            line_box_fragment::GLYPH_TEXT_TYPE_LTR | line_box_fragment::GLYPH_TEXT_TYPE_RTL
        ) {
            text_context.last_known_direction = Some(text_type);
        }
        if text_context.should_respect_linebreaks && chunk.has_breaking_newline {
            is_last_chunk = true;
            if chunk.is_all_whitespace {
                text_type = line_box_fragment::GLYPH_TEXT_TYPE_END_PADDING;
            }
        }
        if text_type == line_box_fragment::GLYPH_TEXT_TYPE_CONTEXT_DEPENDENT {
            text_type = self.resolve_text_direction_from_context(&text_context);
        } else if text_type == line_box_fragment::GLYPH_TEXT_TYPE_COMMON
            && let Some(direction) = self.direction_of_strong_text_around(
                &text_context,
                &text_context.text[chunk.start..chunk.start + chunk.length],
            )
        {
            text_type = direction;
        }
        if text_context.should_respect_linebreaks && chunk.has_breaking_newline {
            // NB: The strong text before a preserved newline has no say in the paragraph after it either.
            text_context.last_known_direction = None;
            text_context.open_bracket_directions.borrow_mut().clear();
            self.text_node_context = Some(text_context);
            return Some(PreparedItem::newline(text_node, chunk.start));
        }

        let full_text = text_context.text;
        let mut shaped_start = chunk.start;
        let mut shaped_length = chunk.length;
        let mut tab_stop = None;
        if chunk.has_breaking_tab {
            let style = text_context.style;
            // https://drafts.csswg.org/css-text-3/#tab-size-property
            // "Represents the measure as a multiple of the advance width of the space character (U+0020) of the
            // nearest block container ancestor of the preserved tab, including its associated letter-spacing and
            // word-spacing."
            // NB: That's the space of the block container's first available font, whatever font the tab itself would
            //     be drawn with. The zeros that set the least distance to a tab stop are of that font too.
            let text_font = self
                .style(self.block_container)
                .font()
                .first_available_font()
                .expect("layout read a font group that never received a frozen font list");
            let tab_inline_size = if style.tab_size_is_number() {
                let space = text_font.glyph_width(b' ' as u32);
                CssPixels::nearest_value_for(
                    style.tab_size_number() * (space + text_context.word_spacing + text_context.letter_spacing) as f64,
                )
            } else {
                style.tab_size()
            };
            let tab_count = full_text[chunk.start..chunk.start + chunk.length]
                .iter()
                .take_while(|unit| **unit == b'\t' as u16)
                .count();
            shaped_start += tab_count;
            shaped_length -= tab_count;
            tab_stop = Some(PreparedTabStop {
                tab_count: u32::try_from(tab_count).expect("tab counts fit in 32 bits"),
                tab_inline_size,
                zero_advance: text_font.glyph_width(b'0' as u32),
                upright_advance: None,
            });
        }
        let shaped_range = shaped_start..shaped_start + shaped_length;
        let mut shaped_text = &full_text[shaped_range.clone()];
        let is_set_upright = |text: &[u16]| {
            !text.is_empty()
                && text_is_set_upright(
                    text_context.writing_mode,
                    text_context.text_orientation,
                    text_chunker::code_point_at(text, 0),
                )
        };
        let mut shape = libgfx_rust::text_layout::shape_text_shared(
            font,
            shaped_text,
            libgfx_rust::text_layout::TextType::try_from(text_type).expect("invalid Gfx::GlyphRun::TextType"),
            text_context.letter_spacing,
            text_context.word_spacing,
            text_chunker::joining_context(full_text, shaped_range),
            is_set_upright(shaped_text),
        );
        // Remember the decision only if every combined run is restored to its original chunks. Text, font, and break-rule
        // changes replace the chunk memo, while spacing is checked separately.
        if text_context.next_chunk_index != next_chunk_index && (!has_soft_wraps || shape.requires_context_for_breaks())
        {
            text_context.can_memoize_independent_chunks = false;
        }
        if has_soft_wraps && !shape.requires_context_for_breaks() {
            // Independent glyphs can keep their small, separately measured chunks. Remember the end of this run so
            // that preparing each following chunk doesn't shape the same suffix again.
            text_context.independent_chunks_until = text_context.next_chunk_index;
            text_context.next_chunk_index = next_chunk_index;
            chunk = first_chunk;
            is_last_chunk = next_chunk_index >= chunks.len();
            self.buffers.soft_wraps.truncate(first_soft_wrap);
            has_soft_wraps = false;
            shaped_text = &full_text[chunk.start..chunk.start + chunk.length];
            shape = libgfx_rust::text_layout::shape_text_shared(
                font,
                shaped_text,
                libgfx_rust::text_layout::TextType::try_from(text_type).expect("invalid Gfx::GlyphRun::TextType"),
                text_context.letter_spacing,
                text_context.word_spacing,
                text_chunker::joining_context(full_text, chunk.start..chunk.start + chunk.length),
                is_set_upright(shaped_text),
            );
        }
        let mut flags = if has_soft_wraps { PreparedItem::SOFT_WRAPS } else { 0 };
        if is_set_upright(shaped_text) {
            // NB: The font's own size takes any font-size-adjust into account.
            let advance = font.facts().pixel_size + text_context.letter_spacing;
            match &mut tab_stop {
                Some(tab_stop) => tab_stop.upright_advance = Some(advance),
                None => shape = shape_set_upright(&shape, advance),
            }
            flags |= PreparedItem::UPRIGHT;
        }
        let generated_empty = synthesize_zero_length_chunk
            || synthesize_chunk_after_trailing_newline
            || (text_context.is_generated_for_pseudo_element && chunk.length == 0);
        if chunk.is_all_whitespace {
            flags |= PreparedItem::TRAILING_WHITESPACE_IS_WHOLE_ITEM;
        }
        if text_context.should_collapse_whitespace && chunk.is_all_whitespace && !generated_empty {
            flags |= PreparedItem::COLLAPSIBLE_WHITESPACE;
        }
        let mut can_break_before = self.previous_chunk_can_break_after;
        // NB: Look for a soft wrap opportunity between the texts of two text nodes, which the chunker of either text
        //     cannot see. This uses the line breaking properties of the text after it, and `word-break: break-all` text
        //     before it also breaks there.
        if is_first_chunk {
            self.regional_indicators_before_text = if self.previous_text.is_some() {
                self.previous_text_regional_indicators
            } else {
                0
            };
            self.code_unit_before_text = self.previous_text.and_then(|text| text.last().copied());
        }
        // https://drafts.csswg.org/css-text-3/#valdef-white-space-nowrap
        // "For soft wrap opportunities defined by the boundary between two characters or atomic inlines, the
        // white-space property on the nearest common ancestor of the two characters controls breaking"
        // NB: The caret chunk of an empty text node has no characters to break before, so the boundary stays for the
        //     text after it.
        let keeps_boundary = text_context.text.is_empty();
        let previous_node = if keeps_boundary {
            None
        } else {
            self.previous_node.take()
        };
        let wraps_at_boundary = match previous_node {
            Some(previous_node) if is_first_chunk => {
                self.style(self.nearest_common_ancestor(previous_node, text_node))
                    .text_wrap_mode()
                    == text_wrap_mode::WRAP
            }
            _ => text_context.should_wrap_lines,
        };
        if is_first_chunk
            && !can_break_before
            && !keeps_boundary
            && let Some(previous_text) = self.previous_text.take()
            && wraps_at_boundary
        {
            if previous_node.is_some_and(|node| {
                let facts = self.facts(node);
                !facts.is_text_node() && !facts.is_word_break_opportunity_node()
            }) {
                can_break_before = text_chunker::soft_wrap_opportunity_after_atomic_inline(&full_text[chunk.start..]);
            } else {
                let style = text_context.style;
                let line_break_language = if style.line_break() == line_break::LOOSE {
                    self.pass.arena().text_line_break_language(text_node)
                } else {
                    text_chunker::LineBreakLanguage::Other
                };
                // NB: A soft hyphen ending the previous text is a hyphenation opportunity of that text, by its own style.
                let breaks_after_soft_hyphen =
                    previous_node.is_none_or(|node| self.style(self.pass.parent(node)).hyphens() != hyphens::NONE);
                let opportunity_with = |word_break| {
                    text_chunker::soft_wrap_opportunity_between(
                        previous_text,
                        &full_text[chunk.start..],
                        self.code_unit_before_previous_text,
                        self.regional_indicators_before_text,
                        word_break,
                        style.line_break(),
                        line_break_language,
                        breaks_after_soft_hyphen,
                    )
                };
                can_break_before = opportunity_with(style.word_break())
                    || (self.previous_text_breaks_all && opportunity_with(word_break::BREAK_ALL));
            }
        }
        if !keeps_boundary {
            self.previous_text = None;
            self.previous_chunk_can_break_after = chunk.can_break_after;
        }
        if can_break_before {
            flags |= PreparedItem::CAN_BREAK_BEFORE;
        }
        if is_first_chunk {
            flags |= PreparedItem::TAKES_LEADING_EDGES;
        }
        if is_last_chunk {
            flags |= PreparedItem::TAKES_TRAILING_EDGES;
        }
        if let Some(tab_stop) = tab_stop {
            flags |= PreparedItem::TAB_STOP;
            self.buffers.tab_stops.push(tab_stop);
            self.buffers
                .tab_stop_items
                .push(u32::try_from(self.buffers.items.len()).expect("prepared item indices fit in 32 bits"));
        }
        let font_index = self.buffers.index_of_font(font);
        let item = PreparedItem::text(
            text_node,
            chunk.start..chunk.start + chunk.length,
            shape,
            text_type,
            font_index,
            flags,
        );
        self.text_node_context = Some(text_context);
        Some(item)
    }

    fn prepare_next_item(&mut self) -> Option<PreparedItem> {
        // A node that produces no item moves on to the next one in this loop rather than in a call: a line of ten
        // thousand empty spans would otherwise take ten thousand frames of the stack.
        loop {
            if self.current_node.is_invalid() {
                return None;
            }
            let node = self.current_node;
            let facts = self.facts(node);
            if facts.is_text_node() {
                match self.prepare_text_item(node) {
                    Some(item) => return Some(item),
                    None => continue,
                }
            }
            if facts.is_absolutely_positioned() {
                self.skip_to_next_within_item();
                return Some(PreparedItem::new(ItemType::AbsolutelyPositionedElement, node, 0));
            }
            if facts.is_floating() {
                self.skip_to_next_within_item();
                return Some(PreparedItem::new(ItemType::FloatingElement, node, 0));
            }
            if facts.is_break_node() {
                self.previous_text = None;
                self.previous_node = None;
                self.skip_to_next_within_item();
                return Some(PreparedItem::new(ItemType::ForcedBreak, node, 0));
            }
            if facts.is_fragmented_inline() {
                self.skip_to_next();
                continue;
            }
            if facts.is_list_item_marker_box() && !facts.list_marker_is_inside() {
                let parent = self.pass.parent(node);
                if parent.is_invalid()
                    || !self.facts(parent).is_list_item_box()
                    || !self.facts(parent).is_fragmented_inline()
                {
                    self.skip_to_next();
                    continue;
                }
            }
            if facts.is_word_break_opportunity_node() {
                // https://html.spec.whatwg.org/multipage/text-level-semantics.html#the-wbr-element
                // "The wbr element represents a line break opportunity."
                // NB: The wbr stands in the text before the text after it as U+200B ZERO WIDTH SPACE, which the line
                //     breaker allows a break after, if the nearest common ancestor of the wbr and that text wraps.
                self.previous_text = Some(&ZERO_WIDTH_SPACE);
                self.previous_node = Some(node);
                self.previous_text_breaks_all = false;
                self.previous_text_regional_indicators = 0;
                self.code_unit_before_previous_text = None;
                self.skip_to_next();
                continue;
            }
            if !facts.is_box() {
                self.skip_to_next();
                continue;
            }
            if facts.is_inline_flow_interrupting_block() {
                self.previous_text = None;
                self.previous_node = None;
                self.skip_to_next_within_item();
                return Some(PreparedItem::new(ItemType::BlockLevelBox, node, 0));
            }
            // https://drafts.csswg.org/css-text-3/#line-break-details
            // "For Web-compatibility there is a soft wrap opportunity before and after each replaced element or other
            // atomic inline, even when adjacent to a character that would normally suppress them, including U+00A0
            // NO-BREAK SPACE. However, with the exception of U+00A0 NO-BREAK SPACE, there must be no soft wrap
            // opportunity between atomic inlines and adjacent characters belonging to the Unicode GL, WJ, or ZWJ line
            // breaking classes."
            // https://drafts.csswg.org/css-text-3/#valdef-white-space-nowrap
            // "For soft wrap opportunities defined by the boundary between two characters or atomic inlines, the
            // white-space property on the nearest common ancestor of the two characters controls breaking"
            // NB: The opportunity before the box is decided here, and the text after it looks for the one after it.
            //     The box stands in the text before that text as U+FFFC OBJECT REPLACEMENT CHARACTER.
            let can_break_before = self.previous_node.is_some_and(|previous_node| {
                self.style(self.nearest_common_ancestor(previous_node, node))
                    .text_wrap_mode()
                    == text_wrap_mode::WRAP
            });
            self.previous_text = Some(&OBJECT_REPLACEMENT_CHARACTER);
            self.previous_node = Some(node);
            self.previous_text_breaks_all = false;
            self.previous_text_regional_indicators = 0;
            self.code_unit_before_previous_text = None;
            self.skip_to_next_within_item();
            let mut flags = PreparedItem::TAKES_LEADING_EDGES | PreparedItem::TAKES_TRAILING_EDGES;
            if can_break_before {
                flags |= PreparedItem::CAN_BREAK_BEFORE;
            }
            return Some(PreparedItem::new(ItemType::Element, node, flags));
        }
    }
}

struct RunItemResolver<'resolver, 'context> {
    context: &'resolver mut inline_formatting_context::InlineFormattingContext<'context>,
    prepared: &'resolver PreparedInlineContent,
    atomic_sizing: AtomicInlineSizing,
    pending_inline_edges: Vec<PendingInlineEdge>,
    /// The inline boxes the resolver is inside, each with the number of in-flow items there were when it began.
    box_model_node_stack: Vec<(Node, usize)>,
    entered_box_model_nodes: Vec<Node>,
    in_flow_item_count: usize,
    item_count: usize,
    empty_inline_boxes: Vec<EmptyInlineBox>,
    /// The inline boxes that hold no in-flow item and come right after the item being resolved.
    empty_inline_boxes_after_item: Vec<EmptyInlineBox>,
    /// The inline boxes that end with the item about to be resolved, and hold no in-flow item before it.
    inline_boxes_ending_with_item: Vec<EmptyInlineBox>,
    accumulated_inline_size_for_tabs: CssPixels,
    next_box_event: usize,
    next_tab_stop: usize,
    forced_break_owner: Node,
}

impl<'context> RunItemResolver<'_, 'context> {
    fn context(&self) -> &inline_formatting_context::InlineFormattingContext<'context> {
        self.context
    }

    /// The inline boxes the items enter, the ones among them that hold no in-flow item (each with the index of the
    /// item it comes before), and the items.
    fn resolve(mut self) -> (Vec<Node>, Vec<EmptyInlineBox>, Vec<Item>) {
        let prepared = self.prepared;
        let mut items: Vec<Item> = Vec::with_capacity(prepared.items.len());
        let mut next_non_whitespace = 0;
        for (index, prepared_item) in prepared.items.iter().enumerate() {
            self.item_count = items.len();
            self.apply_inline_box_events_at(index, false);
            if !self.pending_inline_edges.is_empty()
                && prepared_item.type_() == ItemType::Text
                && prepared_item.has(PreparedItem::COLLAPSIBLE_WHITESPACE)
            {
                if next_non_whitespace <= index {
                    next_non_whitespace = index + 1;
                    while let Some(next) = prepared.items.get(next_non_whitespace)
                        && next.type_() == ItemType::Text
                        && next.has(PreparedItem::COLLAPSIBLE_WHITESPACE)
                    {
                        next_non_whitespace += 1;
                    }
                }
                if prepared
                    .items
                    .get(next_non_whitespace)
                    .is_none_or(|next| matches!(next.type_(), ItemType::BlockLevelBox | ItemType::ForcedBreak))
                {
                    // Whitespace at a line end disappears, but its inline box edges still belong on that line.
                    self.keep_inline_boxes_ending_with_item_as_empty();
                    continue;
                }
            }
            if let Some(last) = items.last()
                && last.type_ == ItemType::ForcedBreak
            {
                let boundary = InlineEdgeBoundary::Closing(self.forced_break_owner, Some(prepared_item.node()));
                self.append_unattached_edges(&mut items, boundary);
            }
            if prepared_item.type_() == ItemType::BlockLevelBox {
                self.append_unattached_edges(&mut items, InlineEdgeBoundary::BeforeBlock(prepared_item.node()));
            } else if prepared_item.type_() == ItemType::ForcedBreak {
                let next = prepared.items.get(index + 1).map(PreparedItem::node);
                self.append_unattached_edges(&mut items, InlineEdgeBoundary::BeforeBreak(next));
            }
            // NB: The boxes that begin or end within an item do so after its content, so they count it.
            if matches!(
                prepared_item.type_(),
                ItemType::Text | ItemType::Element | ItemType::ForcedBreak | ItemType::BlockLevelBox
            ) {
                self.in_flow_item_count += 1;
                self.inline_boxes_ending_with_item.clear();
            } else {
                self.keep_inline_boxes_ending_with_item_as_empty();
            }
            let item = self.resolve_item(index, prepared_item);
            if matches!(item.type_, ItemType::ForcedBreak | ItemType::BlockLevelBox) {
                self.accumulated_inline_size_for_tabs = CssPixels::default();
            } else if item.type_ != ItemType::FloatingElement {
                self.accumulated_inline_size_for_tabs += item.border_box_inline_size();
            }
            let closes_boundary = matches!(item.type_, ItemType::BlockLevelBox | ItemType::ForcedBreak);
            if item.type_ == ItemType::ForcedBreak {
                self.forced_break_owner = prepared_item.node();
            }
            items.push(item);
            let position = items.len();
            self.empty_inline_boxes.extend(
                self.empty_inline_boxes_after_item
                    .drain(..)
                    .map(|empty_box| EmptyInlineBox { position, ..empty_box }),
            );
            if closes_boundary {
                let next = prepared.items.get(index + 1).map(PreparedItem::node);
                self.append_unattached_edges(&mut items, InlineEdgeBoundary::Closing(prepared_item.node(), next));
            }
        }
        self.item_count = items.len();
        self.apply_inline_box_events_at(prepared.items.len(), false);
        self.keep_inline_boxes_ending_with_item_as_empty();
        self.append_unattached_edges(&mut items, InlineEdgeBoundary::End);
        debug_assert_eq!(self.next_box_event, prepared.box_events.len());
        (self.entered_box_model_nodes, self.empty_inline_boxes, items)
    }

    /// The item that these inline boxes end with isn't in-flow content, so they hold none.
    fn keep_inline_boxes_ending_with_item_as_empty(&mut self) {
        self.empty_inline_boxes.append(&mut self.inline_boxes_ending_with_item);
    }

    fn append_unattached_edges(&mut self, items: &mut Vec<Item>, boundary: InlineEdgeBoundary) {
        // Nonzero inline-axis edges keep a line box even when it contains no text. Preserve their presence
        // independently of their sum, since negative margins can cancel other margins, borders, or padding.
        // https://drafts.csswg.org/css-inline-3/#invisible-line-boxes
        let context = &*self.context;
        let is_ancestor = |ancestor: Node, mut node: Node| {
            while !node.is_invalid() {
                node = context.parent_node(node);
                if node == ancestor {
                    return true;
                }
            }
            false
        };
        let closing_prefix = if let InlineEdgeBoundary::Closing(node, next) = boundary {
            self.pending_inline_edges.iter().rposition(|edge| {
                !edge.is_start && is_ancestor(edge.node, node) && next.is_none_or(|next| !is_ancestor(edge.node, next))
            })
        } else {
            None
        };
        let mut edge_index = 0;
        let mut advance = CssPixels::default();
        self.pending_inline_edges.retain(|edge| {
            let emit = match boundary {
                InlineEdgeBoundary::End => true,
                InlineEdgeBoundary::BeforeBlock(node) => edge.is_start || !is_ancestor(edge.node, node),
                InlineEdgeBoundary::BeforeBreak(next) => {
                    edge.is_start || next.is_none_or(|next| !is_ancestor(edge.node, next))
                }
                InlineEdgeBoundary::Closing(_, _) => closing_prefix.is_some_and(|last| edge_index <= last),
            };
            edge_index += 1;
            if !emit {
                return true;
            }
            if edge.margin == CssPixels::default()
                && edge.border == CssPixels::default()
                && edge.padding == CssPixels::default()
            {
                return false;
            }
            let mut item = Item::new(ItemType::InlineEdge, edge.node, Item::NOT_RESOLVED_FROM_A_PREPARED_ITEM);
            if edge.is_low != context.atomic_items_are_right_to_left {
                item.margin_start = edge.margin;
                item.border_start = edge.border;
                item.padding_start = edge.padding;
            } else {
                item.margin_end = edge.margin;
                item.border_end = edge.border;
                item.padding_end = edge.padding;
            }
            if !edge.is_start
                && items.last().is_some_and(|last| {
                    last.type_ == ItemType::ForcedBreak && is_ancestor(edge.node, self.forced_break_owner)
                })
            {
                let forced_break_index = items.len() - 1;
                items.insert(forced_break_index, item);
                for empty_box in &mut self.empty_inline_boxes {
                    if empty_box.position > forced_break_index {
                        empty_box.position += 1;
                    }
                }
            } else {
                advance += item.border_box_inline_size();
                items.push(item);
            }
            false
        });
        self.accumulated_inline_size_for_tabs += advance;
    }

    fn keep_inline_edge(
        &mut self,
        node: Node,
        sides: &used_values::RootInputCells,
        is_start: bool,
        takes_leading_metrics: bool,
    ) {
        let reversed = self.context().facts(node).inline_axis_is_reverse();
        let (margin, border, padding) = if is_start != reversed {
            (sides.margin_left, sides.border_left, sides.padding_left)
        } else {
            (sides.margin_right, sides.border_right, sides.padding_right)
        };
        let physical = if is_start != self.context().atomic_items_are_right_to_left {
            ExtraBoxMetrics {
                margin: sides.margin_left,
                border: sides.border_left,
                padding: sides.padding_left,
            }
        } else {
            ExtraBoxMetrics {
                margin: sides.margin_right,
                border: sides.border_right,
                padding: sides.padding_right,
            }
        };
        let right_to_left_end_margin = self.negative_right_to_left_end_margin(node, sides.margin_left);
        if margin != CssPixels::default()
            || border != CssPixels::default()
            || padding != CssPixels::default()
            || physical.margin != CssPixels::default()
            || physical.border != CssPixels::default()
            || physical.padding != CssPixels::default()
        {
            self.pending_inline_edges.push(PendingInlineEdge {
                node,
                margin,
                border,
                padding,
                physical,
                right_to_left_end_margin,
                is_start,
                is_low: is_start != reversed,
                takes_leading_metrics,
            });
        }
    }

    fn apply_inline_box_events_at(&mut self, item_index: usize, within_item: bool) {
        let position = InlineBoxEvent::position_at(item_index, within_item);
        let prepared = self.prepared;
        while let Some(event) = prepared.box_events.get(self.next_box_event)
            && event.position() == position
        {
            self.next_box_event += 1;
            match event.entered_box() {
                Some(node) => self.enter_node_with_box_model_metrics(node),
                None => self.exit_node_with_box_model_metrics(event.leaves_before_item(), within_item),
            }
        }
    }

    fn enter_node_with_box_model_metrics(&mut self, node: Node) {
        self.entered_box_model_nodes.push(node);
        let sides = lay_out_entered_inline_box(self.context, node);
        self.keep_inline_edge(node, &sides, true, true);
        self.box_model_node_stack.push((node, self.in_flow_item_count));
    }

    fn exit_node_with_box_model_metrics(&mut self, before_item: bool, within_item: bool) {
        let (node, in_flow_item_count_at_start) = self.box_model_node_stack.pop().unwrap();
        let sides = self.context().sides_of_entered_inline_box(node);
        // NB: No item tells the line builder about an inline box that holds no in-flow item, so it's kept apart, for
        //     the line it's on to place it and take its strut from.
        if in_flow_item_count_at_start == self.in_flow_item_count {
            let empty_box = EmptyInlineBox {
                position: self.item_count,
                node,
                leading_edges: self
                    .pending_inline_edges
                    .iter()
                    .filter(|edge| edge.takes_leading_metrics)
                    .map(|edge| edge.physical.margin + edge.physical.border + edge.physical.padding)
                    .fold(CssPixels::default(), |sum, edge| sum + edge),
            };
            if within_item {
                self.empty_inline_boxes_after_item.push(empty_box);
            } else if before_item {
                self.empty_inline_boxes.push(empty_box);
            } else {
                self.inline_boxes_ending_with_item.push(empty_box);
            }
        }
        self.keep_inline_edge(node, &sides, false, before_item);
    }

    fn negative_right_to_left_end_margin(&self, node: Node, margin_left: CssPixels) -> CssPixels {
        if margin_left < CssPixels::default() && self.context().style(node).direction() == direction::RTL {
            margin_left
        } else {
            CssPixels::default()
        }
    }

    /// The end edges of inline boxes that end right after an out-of-flow box are pending when the resolver reaches
    /// that box. It takes no edges itself, so they precede the next item rather than follow its content.
    fn carry_trailing_metrics_past_out_of_flow_box(&mut self) {
        for edge in &mut self.pending_inline_edges {
            edge.takes_leading_metrics = true;
        }
    }

    fn add_extra_box_model_metrics_to_item(
        &mut self,
        item: &mut Item,
        add_leading_metrics: bool,
        add_trailing_metrics: bool,
    ) {
        self.pending_inline_edges.retain(|edge| {
            if edge.takes_leading_metrics && add_leading_metrics {
                item.margin_start += edge.physical.margin;
                item.border_start += edge.physical.border;
                item.padding_start += edge.physical.padding;
            } else if !edge.takes_leading_metrics && add_trailing_metrics {
                item.margin_end += edge.physical.margin;
                item.border_end += edge.physical.border;
                item.padding_end += edge.physical.padding;
            } else {
                return true;
            }
            if edge.is_start {
                item.entered_right_to_left_end_margin += edge.right_to_left_end_margin;
            } else {
                item.exited_right_to_left_end_margin += edge.right_to_left_end_margin;
            }
            false
        });
    }

    fn resolve_item(&mut self, item_index: usize, prepared_item: &PreparedItem) -> Item {
        let node = prepared_item.node();
        match prepared_item.type_() {
            ItemType::InlineEdge => unreachable!("unattached inline edges are emitted while resolving items"),
            ItemType::Text => self.resolve_text_item(item_index, prepared_item),
            ItemType::ForcedBreak => {
                self.apply_inline_box_events_at(item_index, true);
                Item::new(
                    ItemType::ForcedBreak,
                    if self.context().facts(node).is_text_node() {
                        NodeSlotId::INVALID
                    } else {
                        node
                    },
                    item_index,
                )
            }
            ItemType::AbsolutelyPositionedElement => {
                let leading = self
                    .pending_inline_edges
                    .iter()
                    .filter(|edge| edge.takes_leading_metrics)
                    .fold(ExtraBoxMetrics::default(), |mut sum, edge| {
                        sum.margin += edge.physical.margin;
                        sum.border += edge.physical.border;
                        sum.padding += edge.physical.padding;
                        sum
                    });
                let preceded = leading.margin != CssPixels::default()
                    || leading.border != CssPixels::default()
                    || leading.padding != CssPixels::default();
                self.carry_trailing_metrics_past_out_of_flow_box();
                self.apply_inline_box_events_at(item_index, true);
                let mut item = Item::new(ItemType::AbsolutelyPositionedElement, node, item_index);
                item.preceded_by_unattached_inline_start_edges = preceded;
                item
            }
            ItemType::FloatingElement => {
                self.carry_trailing_metrics_past_out_of_flow_box();
                self.apply_inline_box_events_at(item_index, true);
                let mut item = Item::new(ItemType::FloatingElement, node, item_index);
                if self.atomic_sizing == AtomicInlineSizing::Contributions {
                    self.size_item_by_outer_inline_contributions(&mut item);
                }
                item
            }
            ItemType::BlockLevelBox => {
                self.apply_inline_box_events_at(item_index, true);
                let mut item = Item::new(ItemType::BlockLevelBox, node, item_index);
                if self.atomic_sizing == AtomicInlineSizing::Contributions {
                    self.size_item_by_outer_inline_contributions(&mut item);
                }
                item
            }
            ItemType::Element => {
                let item = self.resolve_element_item(item_index, prepared_item);
                self.apply_inline_box_events_at(item_index, true);
                item
            }
        }
    }

    fn resolve_text_item(&mut self, item_index: usize, prepared_item: &PreparedItem) -> Item {
        let prepared = self.prepared;
        let mut inline_offset = 0.0f32;
        let mut upright_advance_after_tabs = None;
        let mut tab_advances = Vec::new();
        if prepared_item.has(PreparedItem::TAB_STOP) {
            let tab_stop = prepared.tab_stops[self.next_tab_stop];
            self.next_tab_stop += 1;
            // NB: The tabs are placed as if the paragraph didn't wrap, and the line builder moves them once it knows
            //     where the item starts on its line.
            tab_advances = advances_of_tabs(&tab_stop, self.accumulated_inline_size_for_tabs);
            inline_offset = tab_advances.iter().sum();
            upright_advance_after_tabs = tab_stop.upright_advance;
        }
        let shape = prepared_item.shape();
        let mut width = shape.width();
        let mut owned_glyph_run = None;
        // NB: Tabs that tab-size: 0 doesn't render still get the spacer glyph, so that the code units of the item's
        //     glyphs line up with those of its text.
        if inline_offset != 0.0 || upright_advance_after_tabs.is_some() || !tab_advances.is_empty() {
            let mut glyphs = shaped_glyphs_from(shape, inline_offset);
            if let Some(advance) = upright_advance_after_tabs {
                width = set_glyphs_upright(&mut glyphs, inline_offset, advance);
            } else if !tab_advances.is_empty() {
                glyphs = glyphs_after_tab_spacers(&glyphs, &tab_advances);
            }
            owned_glyph_run = Some(Box::new(OwnedGlyphRun {
                glyphs,
                width,
                is_upright: prepared_item.has(PreparedItem::UPRIGHT),
            }));
        }
        let shaped_trailing_whitespace = line_box_fragment::TrailingWhitespace {
            length_in_code_units: shape.trailing_whitespace_length_in_code_units(),
            inline_size: CssPixels::nearest_value_for_f32(shape.trailing_whitespace_advance()),
            ascii_inline_size: CssPixels::nearest_value_for_f32(shape.trailing_ascii_whitespace_advance()),
        };
        let chunk_inline_size = CssPixels::nearest_value_for_f32(width + inline_offset);
        let length_in_node = prepared_item.length_in_node();
        let mut item = Item::new(ItemType::Text, prepared_item.node(), item_index);
        item.owned_glyph_run = owned_glyph_run;
        item.offset_in_node = prepared_item.offset_in_node();
        item.length_in_node = length_in_node;
        item.inline_size = chunk_inline_size;
        item.trailing_whitespace = if prepared_item.has(PreparedItem::TRAILING_WHITESPACE_IS_WHOLE_ITEM) {
            line_box_fragment::TrailingWhitespace {
                length_in_code_units: length_in_node,
                inline_size: chunk_inline_size,
                ascii_inline_size: chunk_inline_size,
            }
        } else {
            shaped_trailing_whitespace
        };
        item.is_collapsible_whitespace = prepared_item.has(PreparedItem::COLLAPSIBLE_WHITESPACE);
        item.can_break_before = prepared_item.has(PreparedItem::CAN_BREAK_BEFORE);
        self.add_extra_box_model_metrics_to_item(
            &mut item,
            prepared_item.has(PreparedItem::TAKES_LEADING_EDGES),
            prepared_item.has(PreparedItem::TAKES_TRAILING_EDGES),
        );
        item
    }

    fn size_item_by_outer_inline_contributions(&self, item: &mut Item) {
        let context = self.context();
        let contributions = context.sizing().outer_inline_contributions(
            item.node,
            context.input.available_space.block_size,
            context.input.containing_block_constraints,
        );
        item.inline_size = contributions.max_content_inline_size;
        item.atomic.get_or_insert_default().min_content_inline_size = Some(contributions.min_content_inline_size);
        item.padding_start = contributions.padding_start;
        item.padding_end = contributions.padding_end;
        item.border_start = contributions.border_start;
        item.border_end = contributions.border_end;
        item.margin_start = contributions.margin_start;
        item.margin_end = contributions.margin_end;
        item.entered_right_to_left_end_margin = self.negative_right_to_left_end_margin(item.node, item.margin_start);
        item.exited_right_to_left_end_margin = item.entered_right_to_left_end_margin;
    }

    fn resolve_element_item(&mut self, item_index: usize, prepared_item: &PreparedItem) -> Item {
        let node = prepared_item.node();
        let mut item = Item::new(ItemType::Element, node, item_index);
        item.can_break_before = prepared_item.has(PreparedItem::CAN_BREAK_BEFORE);
        if self.atomic_sizing == AtomicInlineSizing::Contributions {
            self.size_item_by_outer_inline_contributions(&mut item);
        } else {
            let cells = self.context().hold_atomic_inline(node);
            let (content_baselines, box_cells) = self.context().dimension_box_on_line(node, cells);
            item.inline_size = box_cells.content_inline_size;
            item.padding_start = box_cells.padding_left;
            item.padding_end = box_cells.padding_right;
            item.border_start = box_cells.border_left;
            item.border_end = box_cells.border_right;
            item.margin_start = box_cells.margin_left;
            item.margin_end = box_cells.margin_right;
            item.entered_right_to_left_end_margin = self.negative_right_to_left_end_margin(node, item.margin_start);
            item.exited_right_to_left_end_margin = item.entered_right_to_left_end_margin;
            item.atomic = Some(Box::new(AtomicItemMetrics {
                min_content_inline_size: None,
                content_baselines,
                box_cells: Some(box_cells),
            }));
        }
        if self.context().atomic_items_are_right_to_left {
            std::mem::swap(&mut item.margin_start, &mut item.margin_end);
            std::mem::swap(&mut item.border_start, &mut item.border_end);
            std::mem::swap(&mut item.padding_start, &mut item.padding_end);
        }
        self.add_extra_box_model_metrics_to_item(
            &mut item,
            prepared_item.has(PreparedItem::TAKES_LEADING_EDGES),
            prepared_item.has(PreparedItem::TAKES_TRAILING_EDGES),
        );
        item
    }
}

/// Lays out `node`, a fragmented inline the context enters, resolves its box sides and returns them.
fn lay_out_entered_inline_box(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    node: Node,
) -> used_values::RootInputCells {
    let constraints = context.input.containing_block_constraints;
    let mut sides = used_values::cells_before_sizing(&context.pass, node, constraints);
    let style = context.style(node);
    let basis = constraints.inline_basis();
    sides.resolve_box_model(&style, basis);
    let insets = context.compute_inset(node);
    context.keep_entered_inline_box(node, sides, insets);
    if context.run.fragments.is_some() && context.run.fragmentainer.is_none() {
        formatting_context::place_child(
            context.run,
            node,
            formatting_context::PlacedChild::new(fragment_tree::FragmentGeometry {
                cells: sides.in_frame(context.pass.frame, context.run.pass.frame),
                line_data: None,
            })
            .with_in_flow_box_insets(insets),
            FfiCssPixelPoint::default(),
            None,
            None,
        );
    }
    sides
}

struct SharedInlineItems {
    items: std::rc::Rc<[Item]>,
    entered_box_model_nodes: std::rc::Rc<[Node]>,
    empty_inline_boxes: std::rc::Rc<[EmptyInlineBox]>,
    prepared: std::sync::Arc<PreparedInlineContent>,
}

#[derive(Default)]
pub(crate) struct InlineItemCache {
    items_by_block_container: RefCell<HashMap<Node, SharedInlineItems>>,
}

enum ItemQueue {
    Owned(std::vec::IntoIter<Item>),
    Shared { items: std::rc::Rc<[Item]>, next: usize },
}

impl ItemQueue {
    fn as_slice(&self) -> &[Item] {
        match self {
            Self::Owned(items) => items.as_slice(),
            Self::Shared { items, next } => &items[*next..],
        }
    }

    fn next(&mut self) -> Option<Item> {
        match self {
            Self::Owned(items) => items.next(),
            Self::Shared { items, next } => {
                let item = items.get(*next).cloned();
                *next += usize::from(item.is_some());
                item
            }
        }
    }

    fn skip(&mut self, count: usize) {
        assert!(count <= self.as_slice().len());
        match self {
            Self::Owned(items) => items.by_ref().take(count).for_each(drop),
            Self::Shared { next, .. } => *next += count,
        }
    }

    fn into_vec(self) -> Vec<Item> {
        match self {
            Self::Owned(items) => items.collect(),
            Self::Shared { items, next } => items[next..].to_vec(),
        }
    }
}

pub(crate) struct InlineLevelIterator {
    entered_box_model_nodes: Vec<Node>,
    /// The inline boxes that hold no in-flow item, in item order.
    empty_inline_boxes: std::rc::Rc<[EmptyInlineBox]>,
    items: ItemQueue,
    /// The number of items at the end that this iterator only shows to look ahead, and does not hand out.
    items_kept_for_looking_ahead: usize,
    /// The content the items were resolved from, which holds the shapes and fonts of their text.
    prepared: std::sync::Arc<PreparedInlineContent>,
}

impl InlineLevelIterator {
    pub(crate) fn new(context: &mut inline_formatting_context::InlineFormattingContext<'_>) -> Self {
        Self::reuse_or_generate(context, AtomicInlineSizing::Layout)
    }

    pub(crate) fn new_sharing_items(
        context: &mut inline_formatting_context::InlineFormattingContext<'_>,
        cache: &InlineItemCache,
    ) -> Self {
        let containing_block = context.containing_block;
        let shared = cache
            .items_by_block_container
            .borrow()
            .get(&containing_block)
            .map(|shared| {
                (
                    shared.items.clone(),
                    shared.entered_box_model_nodes.clone(),
                    shared.empty_inline_boxes.clone(),
                    shared.prepared.clone(),
                )
            });
        if let Some((items, entered_box_model_nodes, empty_inline_boxes, prepared)) = shared {
            context.atomic_items_are_right_to_left = prepared.orders_atomic_items_right_to_left(context);
            for node in entered_box_model_nodes.iter() {
                lay_out_entered_inline_box(context, *node);
            }
            return Self {
                entered_box_model_nodes: entered_box_model_nodes.to_vec(),
                empty_inline_boxes,
                items: ItemQueue::Shared { items, next: 0 },
                items_kept_for_looking_ahead: 0,
                prepared,
            };
        }
        let iterator = Self::new(context);
        if !iterator.items.as_slice().iter().all(|item| item.type_.stays_on_lines()) {
            return iterator;
        }
        let items = iterator.items.into_vec();
        let shared_items: std::rc::Rc<[Item]> = items.as_slice().into();
        cache.items_by_block_container.borrow_mut().insert(
            containing_block,
            SharedInlineItems {
                items: shared_items.clone(),
                entered_box_model_nodes: iterator.entered_box_model_nodes.as_slice().into(),
                empty_inline_boxes: iterator.empty_inline_boxes.clone(),
                prepared: iterator.prepared.clone(),
            },
        );
        let laid_out_atomic_inlines = items.iter().any(|item| item.type_ == ItemType::Element);
        Self {
            entered_box_model_nodes: iterator.entered_box_model_nodes,
            empty_inline_boxes: iterator.empty_inline_boxes,
            items: if laid_out_atomic_inlines {
                ItemQueue::Owned(items.into_iter())
            } else {
                ItemQueue::Shared {
                    items: shared_items,
                    next: 0,
                }
            },
            items_kept_for_looking_ahead: 0,
            prepared: iterator.prepared,
        }
    }

    pub(crate) fn for_intrinsic_inline_size(
        context: &mut inline_formatting_context::InlineFormattingContext<'_>,
    ) -> Self {
        Self::reuse_or_generate(context, AtomicInlineSizing::Contributions)
    }

    fn reuse_or_generate(
        context: &mut inline_formatting_context::InlineFormattingContext<'_>,
        atomic_sizing: AtomicInlineSizing,
    ) -> Self {
        PreparedInlineContent::for_block_container(context.pass, context.containing_block)
            .resolve_items_for_run(context, atomic_sizing)
    }

    pub(crate) fn next(&mut self) -> Option<Item> {
        if self.items.as_slice().len() <= self.items_kept_for_looking_ahead {
            return None;
        }
        self.items.next()
    }

    pub(crate) fn entered_inline_boxes(&self) -> &[Node] {
        &self.entered_box_model_nodes
    }

    pub(crate) fn has_empty_inline_boxes(&self) -> bool {
        !self.empty_inline_boxes.is_empty()
    }

    /// The inline boxes that hold no in-flow item and come right before the item this iterator handed out last.
    pub(crate) fn empty_inline_boxes_before_last_item(&self) -> impl Iterator<Item = EmptyInlineBox> + '_ {
        self.empty_inline_boxes_before_items_left(self.items.as_slice().len() + 1)
    }

    /// The inline boxes that hold no in-flow item and come right after the item this iterator handed out last.
    pub(crate) fn empty_inline_boxes_after_last_item(&self) -> impl Iterator<Item = EmptyInlineBox> + '_ {
        self.empty_inline_boxes_before_items_left(self.items.as_slice().len())
    }

    fn empty_inline_boxes_before_items_left(&self, item_count: usize) -> impl Iterator<Item = EmptyInlineBox> + '_ {
        let boxes = &self.empty_inline_boxes;
        let start = if boxes.is_empty() {
            0
        } else {
            boxes.partition_point(|empty_box| empty_box.position > item_count)
        };
        boxes[start..]
            .iter()
            .take_while(move |empty_box| empty_box.position == item_count)
            .copied()
    }

    /// The content the items were resolved from, which their glyph runs are read against.
    pub(crate) fn prepared_content(&self) -> &PreparedInlineContent {
        &self.prepared
    }

    /// The <br> or the preserved newline that the forced break `item` stands for.
    pub(crate) fn forced_break_position(&self, item: &Item) -> inline_content::ForcedBreakPosition {
        // NB: The item of a preserved newline has no node, so its prepared item cannot be checked against it.
        let prepared = &self.prepared.items[item.prepared_item_index as usize];
        debug_assert_eq!(prepared.type_(), ItemType::ForcedBreak);
        inline_content::ForcedBreakPosition {
            node: prepared.node(),
            rendered_offset: prepared.offset_in_node() as u32,
        }
    }

    pub(crate) fn shares_items(&self) -> bool {
        matches!(self.items, ItemQueue::Shared { .. })
    }

    pub(crate) fn items(&self) -> &[Item] {
        self.items.as_slice()
    }

    pub(crate) fn contains_only_unbreakable_text(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> bool {
        self.items().iter().all(|item| item.type_ == ItemType::Text)
            && sequence_length(context, &self.prepared, self.items(), false, false).0 == self.items().len()
    }

    pub(crate) fn skip_items(&mut self, count: usize) {
        self.items.skip(count);
    }

    /// An iterator over a copy of the items left in `range`, which leaves the items of this one in place. It looks
    /// ahead at the items after `range` up to `look_ahead_end`, without handing them out.
    pub(crate) fn duplicate(&self, range: std::ops::Range<usize>, look_ahead_end: usize) -> Self {
        let items: Vec<Item> = self.items()[range.start..look_ahead_end].to_vec();
        let empty_inline_boxes = if self.empty_inline_boxes.is_empty() {
            self.empty_inline_boxes.clone()
        } else {
            // NB: The copy ends at `look_ahead_end`, so each box counts the items left from there.
            let items_after_copy = self.items().len() - look_ahead_end;
            self.empty_inline_boxes
                .iter()
                .filter(|empty_box| (items_after_copy..=items_after_copy + items.len()).contains(&empty_box.position))
                .map(|empty_box| EmptyInlineBox {
                    position: empty_box.position - items_after_copy,
                    ..*empty_box
                })
                .collect()
        };
        Self {
            entered_box_model_nodes: self.entered_box_model_nodes.clone(),
            empty_inline_boxes,
            items: ItemQueue::Owned(items.into_iter()),
            items_kept_for_looking_ahead: look_ahead_end - range.end,
            prepared: self.prepared.clone(),
        }
    }

    pub(crate) fn next_inline_run_size(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> Option<CssPixels> {
        sequence_inline_size(context, &self.prepared, self.items(), false)
    }

    pub(crate) fn next_unbreakable_run_inline_size(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> Option<CssPixels> {
        sequence_inline_size(context, &self.prepared, self.items(), true)
    }

    /// The inline size of the content that has to fit after the current item for the line not to break there: the
    /// inline box edges up to the next text or atomic inline, past any out-of-flow items, and then the unbreakable run
    /// that text starts, or that atomic inline.
    pub(crate) fn next_line_content_inline_size(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> CssPixels {
        let items = self.items();
        let start = items
            .iter()
            .position(|item| !sits_between_line_content(item))
            .unwrap_or(items.len());
        let edges = sequence_inline_extent(
            context,
            items[..start].iter().filter(|item| item.type_ == ItemType::InlineEdge),
            None,
        );
        let content = match items.get(start) {
            Some(item) if item.type_ == ItemType::Element => inline_extent([item]),
            Some(item) if item.type_ == ItemType::Text => {
                sequence_inline_size(context, &self.prepared, &items[start..], true).unwrap_or_default()
            }
            _ => CssPixels::default(),
        };
        edges + content
    }

    /// Whether the next text or atomic inline is part of the same unbreakable run as the item before it.
    pub(crate) fn next_line_content_continues_run(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> bool {
        self.items()
            .iter()
            .find(|item| !sits_between_line_content(item))
            .is_some_and(|next| match next.type_ {
                ItemType::Element => !next.can_break_before,
                // NB: White space ends the run only where it wraps.
                ItemType::Text => {
                    !next.can_break_before
                        && (!(next.is_collapsible_whitespace || next.is_ascii_whitespace(context))
                            || context.style(context.parent_node(next.node)).text_wrap_mode() != text_wrap_mode::WRAP)
                }
                _ => false,
            })
    }

    pub(crate) fn unbreakable_run_inline_size(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
        item: &Item,
        stop_at_overflow_breakable_text: bool,
    ) -> CssPixels {
        // An item with no internal break or trailing whitespace is the whole run when the next item can break. Overflow
        // breaking stays on the segment-measuring path. The extent includes every inline edge, and the hyphen that a
        // soft hyphen at the item's end draws if the line breaks there.
        if item.type_ == ItemType::Text
            && !item.is_collapsible_whitespace
            && item.trailing_whitespace.length_in_code_units == 0
            && self.items().first().is_none_or(|next| next.can_break_before)
            && !item.has_soft_wraps(&self.prepared)
            && (!stop_at_overflow_breakable_text || !item.allows_overflow_break(context))
        {
            let hyphen = hyphen_ending_sequence(
                context,
                &self.prepared,
                SequenceEnd::Other,
                None,
                Some(item),
                starts_line_content(self.items().iter()),
            );
            return inline_extent([item]) + hyphen;
        }
        let items = std::iter::once(item).chain(self.items());
        let (length, end) = sequence_length(
            context,
            &self.prepared,
            items.clone(),
            stop_at_overflow_breakable_text,
            false,
        );
        if matches!(end, SequenceEnd::OverflowBreakableText | SequenceEnd::SoftWrapInText) {
            let first_grapheme_cluster = items
                .clone()
                .nth(length)
                .map(|item| first_breakable_segment(context, &self.prepared, item, end));
            let hyphen = hyphen_ending_sequence(
                context,
                &self.prepared,
                end,
                first_grapheme_cluster.as_ref(),
                None,
                false,
            );
            return sequence_inline_extent(context, items.take(length).chain(first_grapheme_cluster.as_ref()), None)
                + hyphen;
        }
        let whitespace_after = if end == SequenceEnd::Whitespace && length > 0 {
            items.clone().nth(length)
        } else {
            None
        };
        let length = length.max(1);
        let hyphen = hyphen_ending_sequence(
            context,
            &self.prepared,
            end,
            None,
            last_line_content(items.clone().take(length)),
            starts_line_content(items.clone().skip(length)),
        );
        sequence_inline_extent(context, items.take(length), whitespace_after) + hyphen
    }

    pub(crate) fn inline_size_following_text(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
        item: &Item,
    ) -> CssPixels {
        let items = std::iter::once(item).chain(self.items());
        let (length, end) = sequence_length(context, &self.prepared, items.clone(), false, true);
        let prefix = (end == SequenceEnd::SoftWrapInText)
            .then(|| first_breakable_segment(context, &self.prepared, self.items().get(length - 1).unwrap(), end));
        // NB: The following text ends in a hyphen where it ends at a soft hyphen that a line can break after.
        let hyphen = if prefix.is_some() || length > 1 {
            hyphen_ending_sequence(
                context,
                &self.prepared,
                end,
                prefix.as_ref(),
                last_line_content(items.clone().take(length)),
                starts_line_content(items.clone().skip(length)),
            )
        } else {
            CssPixels::default()
        };
        sequence_inline_extent(context, items.take(length).chain(prefix.as_ref()), None) + hyphen
            - inline_extent([item])
    }

    pub(crate) fn next_non_whitespace_text_allows_overflow_break(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> bool {
        self.items
            .as_slice()
            .first()
            .is_some_and(|item| item.allows_overflow_break(context))
    }

    pub(crate) fn fragmented_inlines_in_pre_order(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> Vec<Node> {
        self.entered_box_model_nodes
            .iter()
            .copied()
            .filter(|node| context.facts(*node).is_fragmented_inline())
            .collect()
    }
}

pub(crate) fn whitespace_and_sequence_inline_size(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    prepared: &PreparedInlineContent,
    whitespace: &Item,
    items: &[Item],
) -> Option<CssPixels> {
    // NB: An atomic inline that a line can break before decides that break itself when the line builder comes to it, so
    //     the white space before it stays on the line, as it would before a line break there.
    if items
        .iter()
        .find(|item| !sits_between_line_content(item))
        .is_some_and(|item| item.type_ == ItemType::Element && item.can_break_before)
    {
        return None;
    }
    let (length, end) = sequence_length(context, prepared, items, false, false);
    let prefix =
        (end == SequenceEnd::SoftWrapInText).then(|| first_breakable_segment(context, prepared, &items[length], end));
    if length == 0 && prefix.is_none() {
        return None;
    }
    let whitespace_after = (end == SequenceEnd::Whitespace && length > 0).then(|| &items[length]);
    let size = sequence_inline_extent(
        context,
        std::iter::once(whitespace)
            .chain(&items[..length])
            .chain(prefix.as_ref()),
        whitespace_after,
    );
    let hyphen = hyphen_ending_sequence(
        context,
        prepared,
        end,
        prefix.as_ref(),
        last_line_content(items[..length].iter()),
        starts_line_content(items[length..].iter()),
    );
    (size > inline_extent([whitespace])).then_some(size + hyphen)
}

pub(crate) fn sequence_inline_size(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    prepared: &PreparedInlineContent,
    items: &[Item],
    stop_at_overflow_breakable_text: bool,
) -> Option<CssPixels> {
    let (length, end) = sequence_length(context, prepared, items, stop_at_overflow_breakable_text, false);
    let first_grapheme_cluster = matches!(end, SequenceEnd::OverflowBreakableText | SequenceEnd::SoftWrapInText)
        .then(|| first_breakable_segment(context, prepared, &items[length], end));
    let whitespace_after = (end == SequenceEnd::Whitespace && length > 0).then(|| &items[length]);
    let size = sequence_inline_extent(
        context,
        items[..length].iter().chain(first_grapheme_cluster.as_ref()),
        whitespace_after,
    );
    let hyphen = hyphen_ending_sequence(
        context,
        prepared,
        end,
        first_grapheme_cluster.as_ref(),
        last_line_content(items[..length].iter()),
        starts_line_content(items[length..].iter()),
    );
    (end == SequenceEnd::ForcedBreak || size > CssPixels::default()).then_some(size + hyphen)
}

/// Whether `item` sits between the text and atomic inlines of a line without being line content of its own: an
/// out-of-flow item or an inline box edge.
fn sits_between_line_content(item: &Item) -> bool {
    matches!(
        item.type_,
        ItemType::InlineEdge | ItemType::AbsolutelyPositionedElement | ItemType::FloatingElement
    )
}

/// The last text or atomic inline of a measured run, which a hyphen after the run follows, past any out-of-flow items
/// and inline box edges after it.
fn last_line_content<'item>(items: impl Iterator<Item = &'item Item>) -> Option<&'item Item> {
    items.filter(|item| !sits_between_line_content(item)).last()
}

/// Whether the content after a measured run can start a line there: Out-of-flow items and inline box edges sit between
/// them without a line of their own, so the next text or atomic inline after them decides.
fn starts_line_content<'item>(mut items: impl Iterator<Item = &'item Item>) -> bool {
    items
        .find(|item| !sits_between_line_content(item))
        .is_some_and(|item| match item.type_ {
            ItemType::Text | ItemType::Element => item.can_break_before,
            _ => false,
        })
}

/// The inline size of the hyphen that a measured run ends in, where a line can break at its end after a soft hyphen:
/// a soft wrap inside a text item, which ends `segment`, or the start of content that can begin a line, which follows
/// `last_item`.
fn hyphen_ending_sequence(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    prepared: &PreparedInlineContent,
    end: SequenceEnd,
    segment: Option<&Item>,
    last_item: Option<&Item>,
    next_starts_line: bool,
) -> CssPixels {
    let item = match end {
        SequenceEnd::SoftWrapInText => segment,
        SequenceEnd::Other if next_starts_line => last_item,
        _ => None,
    };
    item.map_or(CssPixels::default(), |item| {
        hyphen_inline_size_after(context, prepared, item)
    })
}

#[derive(Clone, Copy, PartialEq)]
enum SequenceEnd {
    ForcedBreak,
    OverflowBreakableText,
    SoftWrapInText,
    /// White space that wraps ends the sequence: The line can break after it.
    Whitespace,
    Other,
}

fn sequence_length<'item>(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    prepared: &PreparedInlineContent,
    items: impl IntoIterator<Item = &'item Item>,
    stop_at_overflow_breakable_text: bool,
    include_first_whole_item: bool,
) -> (usize, SequenceEnd) {
    let mut length = 0;
    for item in items {
        match item.type_ {
            ItemType::ForcedBreak => return (length, SequenceEnd::ForcedBreak),
            ItemType::BlockLevelBox => break,
            // NB: The edges of an inline box that no item took, such as those after white space that was removed at
            //     the end of the block, offer no soft wrap opportunity: They stay on the line with the content
            //     before them.
            ItemType::InlineEdge => {
                length += 1;
                continue;
            }
            _ => {}
        }
        // NB: The sequence ends at its next soft wrap opportunity, such as the one after a hyphen.
        if length > 0 && item.can_break_before {
            break;
        }
        match item.type_ {
            ItemType::Text => {
                if context.style(context.style_source(item.node)).text_wrap_mode() == text_wrap_mode::WRAP {
                    if item.is_collapsible_whitespace || item.is_ascii_whitespace(context) {
                        return (length, SequenceEnd::Whitespace);
                    }
                    if stop_at_overflow_breakable_text && context.overflow_break_applies(item.node) {
                        return (length, SequenceEnd::OverflowBreakableText);
                    }
                }
            }
            // NB: Whether a line can break before an atomic inline is what its item records, as checked above.
            ItemType::Element => {}
            _ => {
                if context.style(context.style_source(item.node)).text_wrap_mode() == text_wrap_mode::WRAP {
                    break;
                }
            }
        }
        if !(length == 0 && include_first_whole_item) && item.has_soft_wraps(prepared) {
            return (length, SequenceEnd::SoftWrapInText);
        }
        length += 1;
    }
    (length, SequenceEnd::Other)
}

fn first_breakable_segment(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    prepared: &PreparedInlineContent,
    item: &Item,
    end: SequenceEnd,
) -> Item {
    let style = context.style(context.style_source(item.node));
    let text_content = context.pass.text_content(item.node);
    if end == SequenceEnd::SoftWrapInText {
        let end = item
            .soft_wrap_offsets(prepared)
            .next()
            .expect("a soft wrap ends the measured segment");
        return item.reshaped_fragment(
            prepared,
            &text_content.text,
            item.offset_in_node..end,
            style.letter_spacing().to_double() as f32,
            style.word_spacing().to_double() as f32,
        );
    }
    let first_grapheme_cluster_inline_size = context.pass.with_grapheme_segmenter(item.node, |segmenter| {
        item.first_grapheme_cluster_inline_size(
            prepared,
            &text_content.text,
            segmenter,
            style.letter_spacing().to_double() as f32,
            style.word_spacing().to_double() as f32,
        )
    });
    let mut cluster = Item::new(ItemType::Text, item.node, item.prepared_item_index as usize);
    cluster.margin_start = item.margin_start;
    cluster.border_start = item.border_start;
    cluster.padding_start = item.padding_start;
    cluster.entered_right_to_left_end_margin = item.entered_right_to_left_end_margin;
    match first_grapheme_cluster_inline_size {
        Some(inline_size) => cluster.inline_size = inline_size,
        None => {
            cluster.inline_size = item.inline_size;
            cluster.padding_end = item.padding_end;
            cluster.border_end = item.border_end;
            cluster.margin_end = item.margin_end;
            cluster.exited_right_to_left_end_margin = item.exited_right_to_left_end_margin;
        }
    }
    cluster
}

/// The inline extent of `items`, without the white space that collapses or hangs at their end. `whitespace_after` is the
/// white space that ends the sequence, if any: The line breaks after it, so the edges of the inline boxes it starts or
/// ends are on the line too, while the white space itself collapses or hangs there.
fn sequence_inline_extent<'item>(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    items: impl Iterator<Item = &'item Item> + Clone,
    whitespace_after: Option<&'item Item>,
) -> CssPixels {
    let mut last_content = None::<(usize, CssPixels)>;
    for (index, item) in items.clone().enumerate() {
        let whitespace = item.trailing_whitespace;
        if whitespace.length_in_code_units == 0 {
            last_content = Some((index, CssPixels::default()));
            continue;
        }
        let is_collapsible = item.is_collapsible_whitespace
            || matches!(
                context.style(context.style_source(item.node)).white_space_collapse(),
                white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
            );
        if !is_collapsible {
            last_content = Some((index, CssPixels::default()));
        } else if whitespace.length_in_code_units < item.length_in_node {
            last_content = Some((index, whitespace.inline_size));
        }
    }
    // NB: Under white-space-collapse: break-spaces, white space takes up its room even at a line's end. White space that
    //     collapses is never under break-spaces, so only preserved white space needs its style. And white space that
    //     hangs and carries no box edges adds nothing, so it's left out of the measurement altogether.
    let whitespace_after = whitespace_after.and_then(|item| {
        let hangs = item.is_collapsible_whitespace
            || context.style(context.style_source(item.node)).white_space_collapse()
                != white_space_collapse::BREAK_SPACES;
        (!hangs || item.carries_inline_box_edges())
            .then(|| (item, if hangs { item.inline_size } else { CssPixels::default() }))
    });
    let mut previous_placed_item = None::<&Item>;
    inline_extent_without_removed_whitespace(
        items
            .enumerate()
            .map(move |(index, item)| {
                let collapses = item.is_collapsible_whitespace
                    && previous_placed_item.is_some_and(|previous| {
                        previous.type_ == ItemType::Text && previous.ends_in_ascii_space(context)
                    });
                if !collapses {
                    previous_placed_item = Some(item);
                }
                let removed_inline_size = match last_content {
                    _ if collapses => item.inline_size,
                    Some((last_index, _)) if index < last_index => CssPixels::default(),
                    Some((last_index, whitespace_inline_size)) if index == last_index => whitespace_inline_size,
                    _ => item.inline_size,
                };
                (item, removed_inline_size)
            })
            .chain(whitespace_after),
    )
}

pub(crate) fn inline_extent<'item>(items: impl IntoIterator<Item = &'item Item>) -> CssPixels {
    inline_extent_without_removed_whitespace(items.into_iter().map(|item| (item, CssPixels::default())))
}

fn inline_extent_without_removed_whitespace<'item>(
    items: impl IntoIterator<Item = (&'item Item, CssPixels)>,
) -> CssPixels {
    let mut position = CssPixels::default();
    let mut extent = None::<CssPixels>;
    let mut held_end_margin = CssPixels::default();
    for (item, removed_inline_size) in items {
        held_end_margin += item.entered_right_to_left_end_margin;
        position += item.margin_start - item.entered_right_to_left_end_margin + item.border_box_inline_size()
            - removed_inline_size;
        let end = position.max(position + item.margin_end);
        extent = Some(extent.map_or(end, |extent| extent.max(end)));
        position += item.margin_end;
        // NB: The end margin of a box that started before these items is already part of the line.
        let released_end_margin = item.exited_right_to_left_end_margin.max(held_end_margin);
        position += released_end_margin;
        held_end_margin -= released_end_margin;
    }
    extent.unwrap_or_default()
}

/// What an atomic inline stands for in the text around it when looking for soft wrap opportunities.
static OBJECT_REPLACEMENT_CHARACTER: [u16; 1] = [0xFFFC];

/// What a `<wbr>` stands for in the text before the text after it when looking for soft wrap opportunities.
static ZERO_WIDTH_SPACE: [u16; 1] = [0x200B];
