/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use std::borrow::Cow;

#[repr(C)]
struct ResolvedParagraph {
    base_level: u8,
    has_strong_direction: bool,
}

unsafe extern "C" {
    fn unicode_bidi_find_paragraph_separator(text: *const u16, length: usize) -> usize;
    fn unicode_bidi_resolve_paragraph(
        text: *const u16,
        length: i32,
        base_level: u8,
        first_strong: bool,
        levels: *mut u8,
    ) -> ResolvedParagraph;
    fn unicode_bidi_reorder_line(paragraph_levels: *const u8, length: i32, base_level: u8, indices: *mut i32) -> i32;
}

// The level encoding from LibUnicode/Bidi.h. Layout uses the tags to split shaping runs.
const RESETS_AT_LINE_END: u8 = 0x80;
const LEVEL_MASK: u8 = !RESETS_AT_LINE_END;
pub(super) const PARAGRAPH_SEPARATOR_LEVEL: u8 = 0xff;

pub(super) fn direction_of_level(level: u8) -> u8 {
    if level & 1 == 0 { direction::LTR } else { direction::RTL }
}

pub(super) fn resolved_level(level: u8) -> u8 {
    level & LEVEL_MASK
}

pub(super) fn preserves_newlines(collapse: u8) -> bool {
    matches!(
        collapse,
        white_space_collapse::PRESERVE | white_space_collapse::PRESERVE_BREAKS | white_space_collapse::BREAK_SPACES
    )
}

fn next_paragraph_separator(text: &[u16]) -> usize {
    // SAFETY: LibUnicode only reads the supplied slice during this call.
    unsafe { unicode_bidi_find_paragraph_separator(text.as_ptr(), text.len()) }
}

pub(super) fn replaced_inline_is_strong(facts: NodeFacts<'_>, style: StyleValues<'_>) -> bool {
    let display = style.display();
    facts.is_replaced_box()
        && display == crate::css::display::FfiDisplay::inline()
        && matches!(style.unicode_bidi(), unicode_bidi::EMBED | unicode_bidi::BIDI_OVERRIDE)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct Position {
    pub(super) paragraph: usize,
    pub(super) offset: usize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct EmptyInlinePosition {
    pub(super) position: Position,
    pub(super) requires_whitespace_collapse: bool,
}

/// A fragmented inline box's edge, measured with an item during line breaking.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) struct InlineBoxEdge {
    pub(super) owner: Node,
    pub(super) advance: CssPixels,
    pub(super) is_left: bool,
}

#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(super) struct InlineBoxEdges {
    pub(super) leading: Vec<InlineBoxEdge>,
    pub(super) trailing: Vec<InlineBoxEdge>,
}

impl InlineBoxEdges {
    pub(super) fn iter(&self) -> impl Iterator<Item = (&InlineBoxEdge, bool)> {
        self.leading
            .iter()
            .map(|edge| (edge, true))
            .chain(self.trailing.iter().map(|edge| (edge, false)))
    }
}

#[derive(Debug, PartialEq, Eq)]
pub(super) struct Paragraph {
    levels: Box<[u8]>,
    pub(super) base_level: u8,
    /// The line's inline base direction for alignment and other line-relative layout.
    pub(super) alignment_direction: u8,
    pub(super) is_unidirectional_ltr: bool,
    unwrapped_inlines: Option<UnwrappedInlines>,
}

#[derive(Debug, PartialEq, Eq)]
struct UnwrappedInlines {
    ranges: HashMap<Node, std::ops::Range<usize>>,
    visual_indices: Box<[i32]>,
    /// Logical offsets in visual order, with -1 for inserted formatting controls.
    content_order: Box<[i32]>,
}

impl crate::heap_size::HeapSize for Paragraph {
    fn heap_size(&self, _: &mut crate::heap_size::HeapSizer) -> usize {
        size_of_val(&*self.levels)
            + self.unwrapped_inlines.as_ref().map_or(0, |inlines| {
                crate::heap_size::map_bytes(&inlines.ranges)
                    + size_of_val(&*inlines.visual_indices)
                    + size_of_val(&*inlines.content_order)
            })
    }
}

impl Paragraph {
    pub(super) fn level_at(&self, offset: usize) -> u8 {
        self.levels[offset]
    }

    pub(super) fn shaping_level_at(&self, offset: usize) -> u8 {
        let level = self.level_at(offset);
        if resolved_level(level) == self.base_level {
            self.base_level
        } else {
            level
        }
    }

    /// The visual index of each UTF-16 code unit on a line, including the controls between styled runs.
    pub(super) fn visual_indices(&self, range: std::ops::Range<usize>) -> (Vec<i32>, usize) {
        let start = range.start;
        let paragraph_levels = &self.levels[range];
        let mut indices = vec![0; paragraph_levels.len()];
        let length = i32::try_from(paragraph_levels.len()).expect("a bidi line fits in its paragraph");
        // SAFETY: Both buffers contain length elements and the call only uses them synchronously.
        let trailing_start = unsafe {
            unicode_bidi_reorder_line(paragraph_levels.as_ptr(), length, self.base_level, indices.as_mut_ptr())
        };
        (indices, start + trailing_start as usize)
    }

    pub(super) fn same_unwrapped_inline_piece(&self, node: Node, first: usize, second: usize) -> bool {
        let Some(unwrapped) = &self.unwrapped_inlines else {
            return true;
        };
        let Some(range) = unwrapped.ranges.get(&node) else {
            return true;
        };
        if !range.contains(&first) || !range.contains(&second) {
            return true;
        }
        let first = unwrapped.visual_indices[first] as usize;
        let second = unwrapped.visual_indices[second] as usize;
        unwrapped.content_order[first.min(second)..first.max(second)]
            .iter()
            .all(|&offset| offset == -1 || range.contains(&(offset as usize)))
    }
}

#[derive(Clone, Copy)]
struct NodeRange {
    start: usize,
    length: usize,
    position: Position,
}

#[derive(Default)]
pub(super) struct InlineContent {
    pub(super) paragraphs: Vec<Paragraph>,
    pub(super) empty_inline_positions: HashMap<Node, EmptyInlinePosition>,
    pub(super) inline_edge_basis: Option<CssPixels>,
    nodes: HashMap<Node, Vec<NodeRange>>,
}

impl InlineContent {
    pub(super) fn position(&self, node: Node, offset: usize) -> Option<Position> {
        let ranges = self.nodes.get(&node)?;
        let range = ranges[ranges.partition_point(|range| range.start <= offset).checked_sub(1)?];
        (offset < range.start + range.length).then(|| Position {
            offset: range.position.offset + offset - range.start,
            ..range.position
        })
    }

    pub(super) fn levels_for_node(&self, node: Node) -> Option<Cow<'_, [u8]>> {
        let ranges = self.nodes.get(&node)?;
        if let [range] = ranges.as_slice() {
            let paragraph = &self.paragraphs[range.position.paragraph];
            let levels = &paragraph.levels[range.position.offset..][..range.length];
            if !levels.contains(&(paragraph.base_level | RESETS_AT_LINE_END)) {
                return Some(Cow::Borrowed(levels));
            }
        }
        Some(Cow::Owned(
            ranges
                .iter()
                .flat_map(|range| {
                    let paragraph = &self.paragraphs[range.position.paragraph];
                    (range.position.offset..range.position.offset + range.length)
                        .map(move |offset| paragraph.shaping_level_at(offset))
                })
                .collect(),
        ))
    }

    pub(super) fn resolve(pass: &LayoutPass<'_>, block: Node, inline_basis: CssPixels) -> Self {
        let style = StyleValues::for_node(pass, block);
        let mut builder = ParagraphBuilder {
            content: Self::default(),
            text: Vec::new(),
            controls: Vec::new(),
            content_count: 0,
            non_collapsible_count: 0,
            last_collapsible_position: None,
            last_collapsed_inline_position: None,
            inline_ranges: HashMap::default(),
            content_ranges: Vec::new(),
            empty_inline_anchors: Vec::new(),
            hard_line_starts: Vec::new(),
            base_level: u8::from(style.direction() == direction::RTL),
            alignment_direction: style.direction(),
            first_strong: style.unicode_bidi() == unicode_bidi::PLAINTEXT,
            inline_basis,
        };
        if matches!(
            style.unicode_bidi(),
            unicode_bidi::BIDI_OVERRIDE | unicode_bidi::ISOLATE_OVERRIDE
        ) {
            builder.enter(
                block,
                Controls::for_style(unicode_bidi::BIDI_OVERRIDE, style.direction()),
            );
        }

        let mut node = pass.first_child(block);
        while !node.is_invalid() {
            let facts = NodeFacts::new(pass, node);
            if facts.is_text_node() {
                let style = StyleValues::for_node(pass, pass.parent(node));
                let text = &pass.text_content(node).text;
                builder.append_text(node, text, style);
            } else if facts.is_floating_or_absolutely_positioned() || facts.is_svg_mask_box() {
                // Out-of-flow boxes do not affect the direction of the surrounding text.
            } else if facts.is_break_node() {
                builder.end_paragraph(Some((node, 0, &[u16::from(b'\n')])));
            } else if facts.is_inline_flow_interrupting_block() {
                builder.end_paragraph(None);
            } else if facts.is_word_break_opportunity_node() {
                builder.text.push(0x200b); // ZERO WIDTH SPACE
            } else if facts.is_fragmented_inline() {
                let style = StyleValues::for_node(pass, node);
                builder.enter(node, Controls::for_style(style.unicode_bidi(), style.direction()));
            } else if facts.is_box()
                && (!facts.is_list_item_marker_box()
                    || facts.list_marker_is_inside()
                    || NodeFacts::new(pass, pass.parent(node)).is_fragmented_inline())
            {
                let style = StyleValues::for_node(pass, node);
                let character = if replaced_inline_is_strong(facts, style) {
                    if style.direction() == direction::RTL {
                        0x200f
                    } else {
                        0x200e
                    }
                } else {
                    0xfffc // OBJECT REPLACEMENT CHARACTER
                };
                builder.append(node, 0, &[character], false);
            }

            let child = first_inline_child(pass, node);
            if !child.is_invalid() && !facts.is_svg_mask_box() {
                node = child;
                continue;
            }
            loop {
                builder.leave(pass, node);
                let sibling = pass.next_sibling(node);
                if !sibling.is_invalid() {
                    node = sibling;
                    break;
                }
                node = pass.parent(node);
                if node == block || node.is_invalid() {
                    builder.finish_paragraph();
                    return builder.content;
                }
            }
        }
        builder.content
    }
}

/// The child an inline formatting context visits before proceeding to the next sibling.
pub(super) fn first_inline_child(pass: &LayoutPass<'_>, node: Node) -> Node {
    let child = pass.first_child(node);
    if child.is_invalid() {
        return child;
    }
    let facts = NodeFacts::new(pass, node);
    let child_facts = NodeFacts::new(pass, child);
    if (child_facts.is_inline()
        || child_facts.is_inline_flow_interrupting_block()
        || child_facts.is_floating_or_absolutely_positioned())
        && StyleValues::for_node(pass, node).display().is_flow_inside()
        && !facts.is_inline_flow_interrupting_block()
        && !facts.is_atomic_inline()
        && (!facts.is_box() || !facts.is_floating_or_absolutely_positioned())
    {
        child
    } else {
        NodeSlotId::INVALID
    }
}

struct Controls {
    opening: &'static [u16],
    closing: &'static [u16],
}

impl Controls {
    fn for_style(bidi: u8, direction: u8) -> Self {
        let rtl = direction == direction::RTL;
        let (opening, closing): (&[u16], &[u16]) = match (bidi, rtl) {
            (unicode_bidi::EMBED, false) => (&[0x202a], &[0x202c]), // LRE, PDF
            (unicode_bidi::EMBED, true) => (&[0x202b], &[0x202c]),  // RLE, PDF
            (unicode_bidi::BIDI_OVERRIDE, false) => (&[0x202d], &[0x202c]), // LRO, PDF
            (unicode_bidi::BIDI_OVERRIDE, true) => (&[0x202e], &[0x202c]), // RLO, PDF
            (unicode_bidi::ISOLATE, false) => (&[0x2066], &[0x2069]), // LRI, PDI
            (unicode_bidi::ISOLATE, true) => (&[0x2067], &[0x2069]), // RLI, PDI
            (unicode_bidi::ISOLATE_OVERRIDE, false) => (&[0x2068, 0x202d], &[0x202c, 0x2069]),
            (unicode_bidi::ISOLATE_OVERRIDE, true) => (&[0x2068, 0x202e], &[0x202c, 0x2069]),
            (unicode_bidi::PLAINTEXT, _) => (&[0x2068], &[0x2069]), // FSI, PDI
            _ => (&[], &[]),
        };
        Self { opening, closing }
    }
}

struct ParagraphBuilder {
    content: InlineContent,
    text: Vec<u16>,
    controls: Vec<OpenInline>,
    content_count: usize,
    non_collapsible_count: usize,
    last_collapsible_position: Option<Position>,
    last_collapsed_inline_position: Option<Position>,
    inline_ranges: HashMap<Node, std::ops::Range<usize>>,
    content_ranges: Vec<std::ops::Range<usize>>,
    /// Empty inline offsets and the preceding content offset, if any, within the same bidi controls.
    empty_inline_anchors: Vec<(usize, Option<usize>)>,
    hard_line_starts: Vec<usize>,
    base_level: u8,
    alignment_direction: u8,
    first_strong: bool,
    inline_basis: CssPixels,
}

struct OpenInline {
    node: Node,
    controls: Controls,
    content_at_start: usize,
    non_collapsible_at_start: usize,
    start: usize,
}

impl ParagraphBuilder {
    fn enter(&mut self, node: Node, controls: Controls) {
        self.text.extend_from_slice(controls.opening);
        self.controls.push(OpenInline {
            node,
            controls,
            content_at_start: self.content_count,
            non_collapsible_at_start: self.non_collapsible_count,
            start: self.text.len(),
        });
    }

    fn leave(&mut self, pass: &LayoutPass<'_>, node: Node) {
        if let Some(box_) = self.controls.pop_if(|box_| box_.node == node) {
            let has_anchored_child = self.last_collapsed_inline_position.is_some_and(|position| {
                position.paragraph == self.content.paragraphs.len() && position.offset >= box_.start
            });
            let needs_anchor = box_.non_collapsible_at_start == self.non_collapsible_count
                && !has_anchored_child
                && self.has_inline_edges(StyleValues::for_node(pass, node));
            if needs_anchor && box_.content_at_start == self.content_count {
                // Use a boundary-neutral anchor for the box's edges. UAX #9 rule X9 ignores it during
                // resolution, so separators and digits on either side still resolve together.
                let offset = self.text.len();
                if offset == 0 || self.hard_line_starts.last() == Some(&offset) {
                    self.empty_inline_anchors.push((offset, None));
                } else if self.content_ranges.last().is_some_and(|range| range.end == offset) {
                    self.empty_inline_anchors.push((offset, Some(offset - 1)));
                }
                self.content.empty_inline_positions.insert(
                    node,
                    EmptyInlinePosition {
                        position: Position {
                            paragraph: self.content.paragraphs.len(),
                            offset,
                        },
                        requires_whitespace_collapse: false,
                    },
                );
                self.append(node, 0, &[0x200b], false);
            } else if needs_anchor {
                // If line layout discards all this whitespace, its position still anchors the box's edges.
                if let Some(position) = self.last_collapsible_position {
                    // Ancestor edges can use the same fragment through their usual visual placement.
                    self.last_collapsed_inline_position = Some(position);
                    self.content.empty_inline_positions.insert(
                        node,
                        EmptyInlinePosition {
                            position,
                            requires_whitespace_collapse: true,
                        },
                    );
                }
            }
            if box_.start < self.text.len() {
                self.inline_ranges.insert(node, box_.start..self.text.len());
            }
            self.text.extend_from_slice(box_.controls.closing);
        }
    }

    fn has_inline_edges(&mut self, style: StyleValues<'_>) -> bool {
        let mut sides = used_values::RootInputCells::default();
        sides.resolve_horizontal_box_model(&style, self.inline_basis);
        if style.margin_left().contains_percentage()
            || style.margin_right().contains_percentage()
            || style.padding_left().contains_percentage()
            || style.padding_right().contains_percentage()
        {
            self.content.inline_edge_basis = Some(self.inline_basis);
        }
        [
            sides.margin_left,
            sides.margin_right,
            sides.border_left,
            sides.border_right,
            sides.padding_left,
            sides.padding_right,
        ]
        .into_iter()
        .any(|side| side != CssPixels::default())
    }

    fn append(&mut self, node: Node, start: usize, text: &[u16], collapsible: bool) {
        if text.is_empty() {
            return;
        }
        let position = Position {
            paragraph: self.content.paragraphs.len(),
            offset: self.text.len(),
        };
        self.content.nodes.entry(node).or_default().push(NodeRange {
            start,
            length: text.len(),
            position,
        });
        self.text.extend_from_slice(text);
        if let Some(last) = self.content_ranges.last_mut()
            && last.end == position.offset
        {
            last.end = self.text.len();
        } else {
            self.content_ranges.push(position.offset..self.text.len());
        }
        self.content_count += 1;
        if collapsible {
            self.last_collapsible_position = Some(position);
        } else {
            self.non_collapsible_count += 1;
        }
    }

    fn append_text(&mut self, node: Node, text: &[u16], style: StyleValues<'_>) {
        let upright = style.has_upright_vertical_text();
        let preserves_newlines = preserves_newlines(style.white_space_collapse());
        let collapses_spaces = matches!(
            style.white_space_collapse(),
            white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
        );
        let mut start = 0;
        while start < text.len() {
            let offset = start + next_paragraph_separator(&text[start..]);
            self.append_text_run(node, start, &text[start..offset], upright, collapses_spaces);
            if offset == text.len() {
                break;
            }
            // CSS breaks at the LF of a preserved CRLF. Any CR or unpreserved LF stays whitespace for bidi.
            let next = offset + 1;
            if text_chunker::next_forced_line_break(&text[offset..next], preserves_newlines) == 0 {
                self.end_paragraph(Some((node, offset, &text[offset..next])));
            } else if matches!(text[offset], 0x0a | 0x0d) {
                self.append_text_run(node, offset, &[u16::from(b' ')], upright, collapses_spaces);
            } else {
                // CSS renders these controls as visible symbols. Resolve them as neutral characters within
                // the paragraph while keeping the original code units for shaping.
                self.append_text_run(node, offset, &[0xfffd], upright, collapses_spaces);
            }
            start = next;
        }
    }

    fn append_text_run(&mut self, node: Node, start: usize, text: &[u16], upright: bool, collapses_spaces: bool) {
        let offset = self.text.len();
        let collapsible = collapses_spaces && text.iter().all(|unit| matches!(*unit, 0x09 | 0x20));
        self.append(node, start, text, collapsible);
        let mut line_start = 0;
        while line_start < text.len() {
            let next = line_start + text_chunker::next_forced_line_break(&text[line_start..], false);
            if next == text.len() {
                break;
            }
            line_start = next + 1;
            self.hard_line_starts.push(offset + line_start);
        }
        // CSS gives upright text strong LTR direction. Keep its UTF-16 offsets for shaping and DOM ranges.
        if upright {
            self.text[offset..].fill(u16::from(b'A'));
        }
    }

    fn end_paragraph(&mut self, newline: Option<(Node, usize, &[u16])>) {
        // A break closes the controls opened by CSS. The same boxes open them again in the new paragraph.
        self.close_paragraph_inlines();
        if let Some((node, offset, separator)) = newline {
            self.append(node, offset, separator, false);
        }
        self.content_count += 1;
        self.non_collapsible_count += 1;
        self.resolve_levels();
        for box_ in &mut self.controls {
            self.text.extend_from_slice(box_.controls.opening);
            box_.start = self.text.len();
        }
    }

    fn close_paragraph_inlines(&mut self) {
        for box_ in self.controls.iter().rev() {
            if box_.start < self.text.len() {
                self.inline_ranges.insert(box_.node, box_.start..self.text.len());
            }
            self.text.extend_from_slice(box_.controls.closing);
        }
    }

    fn finish_paragraph(&mut self) {
        self.close_paragraph_inlines();
        self.resolve_levels();
    }

    fn resolve_levels(&mut self) {
        if self.text.is_empty() {
            return;
        }
        let length = i32::try_from(self.text.len()).expect("bidi paragraph length fits in 32 bits");
        let mut levels = vec![0; self.text.len()].into_boxed_slice();
        // SAFETY: Both buffers contain length elements and the call only uses them synchronously.
        let ResolvedParagraph {
            base_level,
            has_strong_direction,
        } = unsafe {
            unicode_bidi_resolve_paragraph(
                self.text.as_ptr(),
                length,
                self.base_level,
                self.first_strong,
                levels.as_mut_ptr(),
            )
        };
        // https://drafts.csswg.org/css-text-3/#bidi-linebox
        // Weak-only plaintext paragraphs retain the preceding line's alignment direction, even across columns.
        // The initial fallback is the containing block's direction. The base level still controls reordering.
        if has_strong_direction {
            self.alignment_direction = direction_of_level(base_level);
        }
        // ICU assigns a boundary neutral the level of the following run. At a run boundary the empty box's
        // edges belong to the outer run. This also keeps leading empty boxes at the start of the line.
        for (offset, previous) in self.empty_inline_anchors.drain(..) {
            let previous_level = previous.map_or(base_level, |previous| levels[previous] & LEVEL_MASK);
            let next_level = levels[offset] & LEVEL_MASK;
            levels[offset] = next_level.min(previous_level) | RESETS_AT_LINE_END;
        }
        let is_unidirectional_ltr = base_level == 0
            && levels
                .iter()
                .all(|&level| level == PARAGRAPH_SEPARATOR_LEVEL || level & 1 == 0);
        let mut paragraph = Paragraph {
            levels,
            base_level,
            alignment_direction: self.alignment_direction,
            is_unidirectional_ltr,
            unwrapped_inlines: None,
        };
        if !is_unidirectional_ltr && !self.inline_ranges.is_empty() {
            // https://drafts.csswg.org/css-writing-modes-4/#bidi-fragmentation
            // CSS Writing Modes preserves splits that exist before soft wrapping. Ignore formatting controls
            // that were inserted in the bidi input without corresponding inline content.
            let mut visual_indices = Vec::with_capacity(self.text.len());
            let mut line_start = 0;
            for line_end in self
                .hard_line_starts
                .iter()
                .copied()
                .chain(std::iter::once(self.text.len()))
            {
                if line_end > line_start {
                    let (indices, _) = paragraph.visual_indices(line_start..line_end);
                    visual_indices.extend(indices.into_iter().map(|index| line_start as i32 + index));
                }
                line_start = line_end;
            }
            let mut content_order = vec![-1; self.text.len()];
            for range in self.content_ranges.drain(..) {
                for offset in range {
                    content_order[visual_indices[offset] as usize] = offset as i32;
                }
            }
            paragraph.unwrapped_inlines = Some(UnwrappedInlines {
                ranges: std::mem::take(&mut self.inline_ranges),
                visual_indices: visual_indices.into_boxed_slice(),
                content_order: content_order.into_boxed_slice(),
            });
        } else {
            self.inline_ranges.clear();
            self.content_ranges.clear();
        }
        self.hard_line_starts.clear();
        self.content.paragraphs.push(paragraph);
        self.text.clear();
    }
}

#[cfg(test)]
mod ffi_stubs {
    use super::ResolvedParagraph;

    #[unsafe(no_mangle)]
    extern "C" fn unicode_bidi_find_paragraph_separator(text: *const u16, length: usize) -> usize {
        // SAFETY: The caller provides a UTF-16 slice for the duration of the call.
        let text = unsafe { std::slice::from_raw_parts(text, length) };
        text.iter()
            .position(|&unit| matches!(unit, 0x0a | 0x0d | 0x1c..=0x1e | 0x85 | 0x2029))
            .unwrap_or(length)
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_bidi_resolve_paragraph(
        _: *const u16,
        _: i32,
        _: u8,
        _: bool,
        _: *mut u8,
    ) -> ResolvedParagraph {
        unreachable!("Rust layout unit tests do not resolve bidi paragraphs")
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_bidi_reorder_line(_: *const u8, _: i32, _: u8, _: *mut i32) -> i32 {
        unreachable!("Rust layout unit tests do not reorder bidi paragraphs")
    }
}
