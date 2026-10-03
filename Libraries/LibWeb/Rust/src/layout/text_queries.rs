/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::LayoutNodeArena;
use super::node_data::{GENERATED_FOR_FIRST_LETTER, NodeSlotId};
use super::node_facts::{kind_is_box, kind_is_text, node_style_view};
use super::rendered_text::{FfiTextSourceRange, RenderedTextBoundary, ensure_text_content};
use crate::css::css_enums::{visibility, white_space_collapse};
use crate::css::style::tree::StyleNodeID;
use std::ops::Range;

use RenderedTextBoundary::{End, Start};

crate::render_state::held_node_entries!();

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct TextPosition {
    node: NodeSlotId,
    offset: usize,
}

// A run normally maps linearly into one rendered snapshot. A collapsed space
// instead covers the entire whitespace range, which can cross text nodes.
struct TextRun {
    text: Range<usize>,
    start: TextPosition,
    end: TextPosition,
}

impl TextRun {
    fn is_linear(&self) -> bool {
        self.start.node == self.end.node && self.end.offset - self.start.offset == self.text.len()
    }
}

/// Text assembled from rendered snapshots. Runs retain rendered offsets, so
/// all DOM conversion continues to use the snapshots' transform edit maps.
#[derive(Default)]
pub(super) struct MappedText {
    text: Vec<u16>,
    runs: Vec<TextRun>,
}

impl MappedText {
    fn append(&mut self, text: &[u16], start: TextPosition, end: TextPosition) {
        if text.is_empty() {
            return;
        }
        let text_start = self.text.len();
        self.text.extend_from_slice(text);
        if let Some(last) = self.runs.last_mut()
            && last.end == start
            && last.is_linear()
            && start.node == end.node
            && end.offset - start.offset == text.len()
        {
            last.text.end = self.text.len();
            last.end = end;
            return;
        }
        self.runs.push(TextRun {
            text: text_start..self.text.len(),
            start,
            end,
        });
    }

    fn position(&self, offset: usize, boundary: RenderedTextBoundary) -> Option<TextPosition> {
        if offset > self.text.len() || self.runs.is_empty() {
            return None;
        }
        let index = self
            .runs
            .partition_point(|run| match boundary {
                Start => run.text.end <= offset,
                End => run.text.end < offset,
            })
            .min(self.runs.len() - 1);
        let run = &self.runs[index];
        if offset == run.text.end {
            return Some(run.end);
        }
        Some(TextPosition {
            node: run.start.node,
            offset: run.start.offset + offset - run.text.start,
        })
    }

    fn dom_position(
        &self,
        arena: &LayoutNodeArena,
        offset: usize,
        boundary: RenderedTextBoundary,
    ) -> Option<TextPosition> {
        let position = self.position(offset, boundary)?;
        let content = arena.text_content(position.node)?;
        Some(TextPosition {
            node: position.node,
            offset: content.dom_offset_for_rendered_text_offset(position.offset, boundary),
        })
    }

    fn dom_range(&self, arena: &LayoutNodeArena, range: Range<usize>) -> Option<FfiDomTextRange> {
        let start = self.dom_position(arena, range.start, Start)?;
        let end = self.dom_position(arena, range.end, End)?;
        Some(FfiDomTextRange {
            start_text_node: text_node_identity(arena, start.node),
            start_offset: start.offset,
            end_text_node: text_node_identity(arena, end.node),
            end_offset: end.offset,
        })
    }
}

fn collapses_whitespace(arena: &LayoutNodeArena, node: NodeSlotId) -> bool {
    let parent = arena.data(node).parent.get();
    matches!(
        node_style_view(arena.data(parent))
            .expect("text parent has style")
            .inherited_text()
            .white_space_collapse,
        white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
    )
}

unsafe fn ensure_text_fragments(arena: *mut LayoutNodeArena, primary: NodeSlotId) {
    // SAFETY: The caller lends the live arena; the IDs do not borrow it.
    let fragments = unsafe { &*arena }.text_fragments(primary);
    for &node in fragments.as_slice() {
        // SAFETY: The caller lends the live arena, which nothing else borrows meanwhile.
        ensure_text_content(unsafe { &mut *arena }, node);
    }
}

/// The text the rows of the text node whose primary row is `primary` render, with whitespace
/// collapsed where their style collapses it if `collapse_whitespace`.
pub(crate) fn rendered_text(arena: &mut LayoutNodeArena, primary: NodeSlotId, collapse_whitespace: bool) -> Vec<u16> {
    // SAFETY: The arena is borrowed exclusively.
    unsafe { ensure_text_fragments(arena, primary) };
    let mut text = Vec::new();
    for &node in arena.text_fragments(primary).as_slice() {
        let content = arena.text_content(node).expect("fragment was refreshed");
        if !collapse_whitespace || !collapses_whitespace(arena, node) {
            text.extend_from_slice(&content.text);
            continue;
        }
        let mut previous_is_space = false;
        for &unit in &content.text {
            let is_space = matches!(unit, 0x09..=0x0d | 0x20);
            if !is_space || !previous_is_space {
                text.push(unit);
            }
            previous_is_space = is_space;
        }
    }
    text
}

fn word_range(
    arena: &LayoutNodeArena,
    primary: NodeSlotId,
    dom_offset: usize,
    boundaries: impl FnOnce(&[u16], usize) -> Range<usize>,
) -> Range<usize> {
    let primary_content = arena.text_content(primary).expect("primary was refreshed");
    if primary_content.is_password_input() {
        return 0..primary_content.dom_range().end;
    }
    let mut text = MappedText::default();
    let mut hit_offset = None;
    for &node in arena.text_fragments(primary).as_slice() {
        let content = arena.text_content(node).expect("fragment was refreshed");
        let dom_range = content.dom_range();
        if hit_offset.is_none() && dom_offset >= dom_range.start && dom_offset <= dom_range.end {
            hit_offset = Some(text.text.len() + content.rendered_text_offset_for_dom_offset(dom_offset, Start));
        }
        text.append(
            &content.text,
            TextPosition { node, offset: 0 },
            TextPosition {
                node,
                offset: content.text.len(),
            },
        );
    }
    if text.text.is_empty() {
        return dom_offset..dom_offset;
    }
    let range = boundaries(&text.text, hit_offset.unwrap_or(text.text.len()));
    let start = text
        .dom_position(arena, range.start, Start)
        .expect("word starts in the rendered text");
    let end = text
        .dom_position(arena, range.end, End)
        .expect("word ends in the rendered text");
    start.offset..end.offset
}

/// The DOM range of the word at `dom_offset` in the text the rows of the text node whose primary
/// row is `primary` render.
pub(crate) fn text_word_range(
    arena: &mut LayoutNodeArena,
    primary: NodeSlotId,
    dom_offset: usize,
) -> FfiTextSourceRange {
    // SAFETY: The arena is borrowed exclusively.
    unsafe { ensure_text_fragments(arena, primary) };
    let range = word_range(arena, primary, dom_offset, super::text_chunker::word_boundaries);
    FfiTextSourceRange {
        start: range.start,
        length: range.len(),
    }
}

#[derive(Default)]
struct SearchTextBuilder {
    blocks: Vec<MappedText>,
    current: MappedText,
    pending_space: Option<(TextPosition, TextPosition)>,
}

impl SearchTextBuilder {
    fn flush(&mut self) {
        if !self.current.text.is_empty() {
            self.blocks.push(std::mem::take(&mut self.current));
        }
        self.pending_space = None;
    }

    fn append(&mut self, node: NodeSlotId, text: &[u16], collapse: bool) {
        for (offset, &unit) in text.iter().enumerate() {
            let start = TextPosition { node, offset };
            let end = TextPosition {
                node,
                offset: offset + 1,
            };
            if collapse && matches!(unit, 0x09 | 0x0b..=0x0d | 0x20) {
                self.pending_space.get_or_insert((start, end)).1 = end;
                continue;
            }
            if let Some((start, end)) = self.pending_space.take()
                && !self.current.text.is_empty()
            {
                self.current.append(&[0x20], start, end);
            }
            self.current.append(&[unit], start, end);
        }
    }
}

/// The DOM text node a text row renders, by identity, or zero for generated text, which renders
/// none.
fn text_node_identity(arena: &LayoutNodeArena, node: NodeSlotId) -> u32 {
    arena.node_style_node(node).map_or(0, StyleNodeID::raw)
}

enum SearchNode {
    Skip,
    Break,
    Text(StyleNodeID),
}

impl SearchNode {
    fn text(arena: &LayoutNodeArena, node: NodeSlotId) -> Self {
        // Generated text renders no DOM text, so only a row built for a DOM text node can be
        // searched.
        match arena.node_style_node(node) {
            Some(text) => SearchNode::Text(text),
            None => SearchNode::Skip,
        }
    }
}

fn search_node(arena: &LayoutNodeArena, node: NodeSlotId) -> SearchNode {
    let data = arena.data(node);
    if node_style_view(data).is_some_and(|style| style.display().is_none()) {
        return SearchNode::Skip;
    }
    let pseudo = data.generated_for.get();
    if kind_is_box(data.kind.get()) || (pseudo != 0 && pseudo != GENERATED_FOR_FIRST_LETTER) {
        return SearchNode::Break;
    }
    if kind_is_text(data.kind.get()) {
        let style = node_style_view(arena.data(data.parent.get())).expect("text parent has style");
        if style.visibility() == visibility::VISIBLE && style.effects().opacity != 0.0 {
            return SearchNode::text(arena, node);
        }
    }
    SearchNode::Skip
}

/// The identities of the DOM text nodes whose rows below `viewport` find-in-page would search, in
/// tree order, for the host to say which are searchable. None where the searchable text is built.
pub(crate) fn search_candidates(arena: &LayoutNodeArena, viewport: NodeSlotId) -> Vec<StyleNodeID> {
    if arena.searchable_text.is_some() {
        return Vec::new();
    }
    let mut candidates = Vec::new();
    arena.for_each_node_in_layout_subtree_in_pre_order(viewport, |node| {
        if let SearchNode::Text(text) = search_node(arena, node) {
            candidates.push(text);
        }
    });
    candidates
}

fn ensure_searchable_text(
    arena: &mut LayoutNodeArena,
    viewport: NodeSlotId,
    mut is_searchable: impl FnMut(StyleNodeID) -> bool,
) {
    if arena.searchable_text.is_some() {
        return;
    }
    let mut nodes = Vec::new();
    arena.for_each_node_in_layout_subtree_in_pre_order(viewport, |node| nodes.push(node));
    let mut builder = SearchTextBuilder::default();
    for node in nodes {
        match search_node(arena, node) {
            SearchNode::Skip => {}
            SearchNode::Break => builder.flush(),
            SearchNode::Text(text) => {
                if !is_searchable(text) {
                    continue;
                }
                ensure_text_content(arena, node);
                let content = arena.text_content(node).expect("search text was refreshed");
                builder.append(node, &content.text, collapses_whitespace(arena, node));
            }
        }
    }
    builder.flush();
    arena.searchable_text = Some(builder.blocks);
}

/// A range in the DOM text the layout rows render, between the DOM text nodes the identities
/// name.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct FfiDomTextRange {
    pub start_text_node: u32,
    pub start_offset: usize,
    pub end_text_node: u32,
    pub end_offset: usize,
}

fn find_text(text: &[u16], query: &[u16], offset: usize, case_sensitive: bool) -> Option<usize> {
    if case_sensitive {
        return text
            .get(offset..)?
            .windows(query.len())
            .position(|window| window == query)
            .map(|index| offset + index);
    }
    // Match AK::Utf16View's ASCII-only folding and code-point stepping.
    let end = text.len().checked_sub(query.len())?;
    let mut index = offset;
    while index <= end {
        let candidate = &text[index..index + query.len()];
        let lowercase = |unit: u16| {
            if (0x41..=0x5a).contains(&unit) {
                unit + 0x20
            } else {
                unit
            }
        };
        if candidate.iter().zip(query).all(|(&a, &b)| lowercase(a) == lowercase(b)) {
            return Some(index);
        }
        index += super::text_chunker::code_unit_length_for_code_point(super::text_chunker::code_point_at(candidate, 0));
    }
    None
}

/// Where `query` occurs in the searchable text below `viewport`, which, where it is built for the
/// query, leaves out the DOM text nodes in `excluded`, sorted.
pub(crate) fn find_matching_text(
    arena: &mut LayoutNodeArena,
    viewport: NodeSlotId,
    query: &[u16],
    case_sensitive: bool,
    excluded: &[StyleNodeID],
) -> Vec<FfiDomTextRange> {
    if query.is_empty() {
        return Vec::new();
    }
    ensure_searchable_text(arena, viewport, |text| excluded.binary_search(&text).is_err());
    let mut matches = Vec::new();
    for block in arena.searchable_text.as_ref().expect("search cache was prepared") {
        let mut offset = 0;
        while let Some(index) = find_text(&block.text, query, offset, case_sensitive) {
            if let Some(range) = block.dom_range(arena, index..index + query.len())
                && matches.last() != Some(&range)
            {
                matches.push(range);
            }
            offset = index + query.len();
            if offset >= block.text.len() {
                break;
            }
        }
    }
    matches
}

/// The DOM range of the word at `dom_offset` in the text the rows of the text node whose primary row is `primary`
/// render, which input reads in `read` as it selects the word at a point.
///
/// # Safety
///
/// `host` must be a live document host, on its document's thread, and the primary and its fragments live text rows
/// with styled parents.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_text_word_range(
    host: *mut crate::render_state::DocumentHost,
    primary: NodeSlotId,
    dom_offset: usize,
) -> FfiTextSourceRange {
    use crate::render_state::{ArenaAnswer, ArenaQuery, ask};
    assert!(!host.is_null(), "document host is null");
    let query = ArenaQuery::WordRange { primary, dom_offset };
    // SAFETY: Guaranteed by the caller.
    let host = unsafe { &*host };
    let ArenaAnswer::Range(range) = ask(node_read(), host, query) else {
        unreachable!("a word range is answered with a range");
    };
    range
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeKind;
    use crate::layout::rendered_text::{RenderedTextEdit, TextContent};

    fn node(
        arena: &mut LayoutNodeArena,
        text: &str,
        start: usize,
        length: usize,
        edits: Vec<RenderedTextEdit>,
    ) -> NodeSlotId {
        let node = arena.allocate_for_test().slot;
        arena.write_shape(node).set_kind(NodeKind::TextNode);
        arena.set_text_content(node, TextContent::for_test(text, start, length, edits));
        node
    }

    fn edit(dom_start: usize, dom_length: usize, rendered_start: usize, rendered_length: usize) -> RenderedTextEdit {
        RenderedTextEdit {
            dom_start_offset: dom_start,
            dom_length_in_code_units: dom_length,
            rendered_start_offset: rendered_start,
            rendered_length_in_code_units: rendered_length,
        }
    }

    #[test]
    fn word_selection_segments_across_first_letter_slices_and_maps_both_boundaries() {
        let mut arena = LayoutNodeArena::new();
        let first = node(&mut arena, "SS", 0, 1, vec![edit(0, 1, 0, 2)]);
        let remainder = node(&mut arena, "a bc", 1, 4, Vec::new());
        arena.set_first_letter_slices(first, remainder, 1, 5);

        let select_word = |text: &[u16], offset| {
            assert_eq!(String::from_utf16_lossy(text), "SSa bc");
            assert_eq!(offset, 4);
            4..6
        };
        assert_eq!(word_range(&arena, remainder, 3, select_word), 3..5);
        assert_eq!(
            word_range(&arena, remainder, 0, |_, offset| {
                assert_eq!(offset, 0);
                0..3
            }),
            0..2
        );
    }

    #[test]
    fn search_ranges_use_transform_edits_including_partial_expansions_and_contractions() {
        let mut arena = LayoutNodeArena::new();
        let expanded = node(&mut arena, "ASS BC", 0, 5, vec![edit(1, 1, 1, 2)]);
        let mut builder = SearchTextBuilder::default();
        builder.append(expanded, &arena.text_content(expanded).unwrap().text, false);
        for rendered in [1..2, 2..3, 1..3] {
            let range = builder.current.dom_range(&arena, rendered).unwrap();
            assert_eq!((range.start_offset, range.end_offset), (1, 2));
        }
        let range = builder.current.dom_range(&arena, 4..6).unwrap();
        assert_eq!((range.start_offset, range.end_offset), (3, 5));

        let contracted = node(&mut arena, "ix", 0, 3, vec![edit(0, 2, 0, 1)]);
        let mut builder = SearchTextBuilder::default();
        builder.append(contracted, &arena.text_content(contracted).unwrap().text, false);
        let range = builder.current.dom_range(&arena, 0..1).unwrap();
        assert_eq!((range.start_offset, range.end_offset), (0, 2));
    }

    #[test]
    fn collapsed_spaces_cover_source_whitespace_across_nodes_without_per_character_maps() {
        let mut arena = LayoutNodeArena::new();
        let left = node(&mut arena, "alpha   ", 0, 8, Vec::new());
        let right = node(&mut arena, "  beta", 0, 6, Vec::new());
        let mut builder = SearchTextBuilder::default();
        builder.append(left, &arena.text_content(left).unwrap().text, true);
        builder.append(right, &arena.text_content(right).unwrap().text, true);
        assert_eq!(String::from_utf16_lossy(&builder.current.text), "alpha beta");
        assert_eq!(builder.current.runs.len(), 3);
        assert_eq!(
            builder.current.position(5, Start),
            Some(TextPosition { node: left, offset: 5 })
        );
        assert_eq!(
            builder.current.position(6, End),
            Some(TextPosition { node: right, offset: 2 })
        );
        assert_eq!(
            builder.current.position(6, Start),
            Some(TextPosition { node: right, offset: 2 })
        );
        assert_eq!(
            builder.current.position(10, End),
            Some(TextPosition { node: right, offset: 6 })
        );
        builder.flush();
        builder.append(right, &[0x20, 0x20], true);
        builder.flush();
        assert_eq!(builder.blocks.len(), 1);
    }

    #[test]
    fn search_cache_is_discarded_when_its_snapshot_changes_or_a_node_is_freed() {
        let mut arena = LayoutNodeArena::new();
        let text = node(&mut arena, "hello", 0, 5, Vec::new());
        let mut builder = SearchTextBuilder::default();
        builder.append(text, &arena.text_content(text).unwrap().text, false);
        builder.flush();
        arena.searchable_text = Some(builder.blocks);
        arena.set_text_content(text, TextContent::for_test("world", 0, 5, Vec::new()));
        assert!(arena.searchable_text.is_none());
        arena.searchable_text = Some(Vec::new());
        arena
            .free_subtree(text)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(arena.searchable_text.is_none());
    }

    #[test]
    fn search_finds_a_match_that_starts_where_the_previous_match_ends() {
        let mut arena = LayoutNodeArena::new();
        let text = node(&mut arena, "foofoo", 0, 6, Vec::new());
        let mut builder = SearchTextBuilder::default();
        builder.append(text, &arena.text_content(text).unwrap().text, false);
        builder.flush();
        arena.searchable_text = Some(builder.blocks);
        let query: Vec<u16> = "foo".encode_utf16().collect();
        let offsets: Vec<_> = find_matching_text(&mut arena, text, &query, true, &[])
            .iter()
            .map(|range| (range.start_offset, range.end_offset))
            .collect();
        assert_eq!(offsets, [(0, 3), (3, 6)]);
    }

    #[test]
    fn search_retains_case_sensitivity_and_utf16_offsets() {
        let text: Vec<u16> = "a😀Bc".encode_utf16().collect();
        let query: Vec<u16> = "😀b".encode_utf16().collect();
        assert_eq!(find_text(&text, &query, 0, false), Some(1));
        assert_eq!(find_text(&text, &query, 0, true), None);
        assert_eq!(find_text(&text, &query, 2, false), None);
    }
}
