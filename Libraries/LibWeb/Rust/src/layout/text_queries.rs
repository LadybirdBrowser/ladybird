/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::LayoutNodeArena;
use super::node_data::{GENERATED_FOR_FIRST_LETTER, NodeKind, NodeSlotId};
use super::node_facts::{kind_is_box, kind_is_text, node_style_view};
use super::rendered_text::{FfiTextSourceRange, RenderedTextBoundary, ensure_text_content};
use crate::css::css_enums::{visibility, white_space_collapse};
use crate::css::style::tree::StyleNodeID;
use crate::painting::paint_read::PaintRead;
use std::cmp::Ordering;
use std::ffi::c_void;
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
#[derive(Clone)]
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
#[derive(Clone, Default)]
pub(super) struct MappedText {
    text: Vec<u16>,
    runs: Vec<TextRun>,
    searchable_without_collator: std::sync::OnceLock<bool>,
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

    /// Where the text appended last ends.
    fn end(&self) -> Option<TextPosition> {
        self.runs.last().map(|run| run.end)
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

fn collapses_whitespace(rows: &impl PaintRead, node: NodeSlotId) -> bool {
    let parent = rows.node_parent_if_live(node);
    matches!(
        parent
            .and_then(|parent| rows.node_style_if_live(parent))
            .expect("text parent has style")
            .white_space_collapse(),
        white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
    )
}

fn ensure_text_fragments(arena: &mut LayoutNodeArena, primary: NodeSlotId) {
    for &node in arena.text_fragments(primary).as_slice() {
        ensure_text_content(arena, node);
    }
}

/// The text the rows of the text node whose primary row is `primary` render.
pub(crate) fn rendered_text(arena: &mut LayoutNodeArena, primary: NodeSlotId) -> Vec<u16> {
    ensure_text_fragments(arena, primary);
    let mut text = Vec::new();
    for &node in arena.text_fragments(primary).as_slice() {
        let content = arena.text_content(node).expect("fragment was refreshed");
        text.extend_from_slice(&content.text);
    }
    text
}

/// The rendered text of a text row, with each UTF-16 code unit mapped back to the DOM offsets that produced it.
pub(crate) fn rendered_text_with_dom_offsets(
    arena: &mut LayoutNodeArena,
    primary: NodeSlotId,
) -> (Vec<u16>, Vec<(usize, usize)>) {
    ensure_text_fragments(arena, primary);
    let mut text = MappedText::default();
    for &node in arena.text_fragments(primary).as_slice() {
        let content = arena.text_content(node).expect("fragment was refreshed");
        text.append(
            &content.text,
            TextPosition { node, offset: 0 },
            TextPosition {
                node,
                offset: content.text.len(),
            },
        );
    }

    let mut offsets = Vec::with_capacity(text.text.len());
    for offset in 0..text.text.len() {
        let start = text
            .dom_position(arena, offset, Start)
            .expect("rendered code unit has a start");
        let end = text
            .dom_position(arena, offset + 1, End)
            .expect("rendered code unit has an end");
        offsets.push((start.offset, end.offset));
    }
    (text.text, offsets)
}

/// [`rendered_text`] as `rows` carry it, where they carry the rendered text of each of the text node's rows: a text row
/// carries what [`rendered_text`] answers unless it is among `awaiting_render`, the rows that wait for their text to be
/// rendered again, by slot index.
pub(crate) fn rendered_text_of_rows(
    rows: &impl PaintRead,
    awaiting_render: &[NodeSlotId],
    primary: NodeSlotId,
) -> Option<Vec<u16>> {
    let mut text = Vec::new();
    for &node in rows.text_fragments(primary).as_slice() {
        if awaiting_render
            .binary_search_by_key(&node.index, |row| row.index)
            .is_ok()
        {
            return None;
        }
        text.extend_from_slice(&rows.rendered_text(node)?.text);
    }
    Some(text)
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
    ensure_text_fragments(arena, primary);
    let range = word_range(arena, primary, dom_offset, super::text_chunker::word_boundaries);
    FfiTextSourceRange {
        start: range.start,
        length: range.len(),
    }
}

/// The box whose inline content the text row `node` is part of.
fn containing_box(arena: &LayoutNodeArena, node: NodeSlotId) -> NodeSlotId {
    let mut current = arena.data(node).parent.get();
    while !current.is_invalid() && !kind_is_box(arena.data(current).kind.get()) {
        current = arena.data(current).parent.get();
    }
    current
}

/// The DOM range of the word at `dom_offset` in the text node whose primary row is `primary`, read across the inline
/// boxes around it: A word runs on through inline elements, but not past a box, a line break or generated content
/// within the same inline content. None where the word lies in that one text node anyway.
pub(crate) fn inline_word_range(
    arena: &mut LayoutNodeArena,
    primary: NodeSlotId,
    dom_offset: usize,
) -> Option<FfiDomTextRange> {
    if arena
        .text_content(primary)
        .is_some_and(|content| content.is_password_input())
    {
        return None;
    }
    let container = containing_box(arena, primary);
    if container.is_invalid() {
        return None;
    }
    // NB: The rows of the inline content, split into runs where something other than inline text and inline boxes
    //     comes between them.
    let mut nodes = Vec::new();
    arena.for_each_node_in_layout_subtree_in_pre_order(container, |node| nodes.push(node));
    let mut runs: Vec<Vec<NodeSlotId>> = vec![Vec::new()];
    let mut primary_run = None;
    for node in nodes {
        if node == container {
            continue;
        }
        let data = arena.data(node);
        let kind = data.kind.get();
        let pseudo = data.generated_for.get();
        if node_style_view(data).is_some_and(|style| style.display().is_none()) {
            continue;
        }
        let breaks_run = kind == NodeKind::BreakNode
            || (kind_is_box(kind) && pseudo != GENERATED_FOR_FIRST_LETTER)
            || (pseudo != 0 && pseudo != GENERATED_FOR_FIRST_LETTER);
        if breaks_run {
            if !runs.last().expect("there is always a run").is_empty() {
                runs.push(Vec::new());
            }
            continue;
        }
        // NB: A ::first-letter row is read with the rest of its text node, through that node's primary row.
        if kind != NodeKind::TextNode
            || pseudo == GENERATED_FOR_FIRST_LETTER
            || arena.node_style_node(node).is_none()
            || containing_box(arena, node) != container
        {
            continue;
        }
        if node == primary {
            primary_run = Some(runs.len() - 1);
        }
        runs.last_mut().expect("there is always a run").push(node);
    }
    let run = std::mem::take(&mut runs[primary_run?]);
    if run.len() < 2 {
        return None;
    }

    let mut text = MappedText::default();
    let mut hit_offset = None;
    for node in run {
        ensure_text_fragments(arena, node);
        for &fragment in arena.text_fragments(node).as_slice() {
            let content = arena.text_content(fragment).expect("fragment was refreshed");
            let dom_range = content.dom_range();
            if node == primary && hit_offset.is_none() && dom_offset >= dom_range.start && dom_offset <= dom_range.end {
                hit_offset = Some(text.text.len() + content.rendered_text_offset_for_dom_offset(dom_offset, Start));
            }
            text.append(
                &content.text,
                TextPosition {
                    node: fragment,
                    offset: 0,
                },
                TextPosition {
                    node: fragment,
                    offset: content.text.len(),
                },
            );
        }
    }
    if text.text.is_empty() {
        return None;
    }
    let range = super::text_chunker::word_boundaries(&text.text, hit_offset?);
    let dom_range = text.dom_range(arena, range)?;
    if dom_range.start_text_node == dom_range.end_text_node {
        return None;
    }
    Some(dom_range)
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

    /// A line break separates the words around it as collapsible white space does, and collapses with the white space
    /// next to it. The space it stands for covers no DOM text, at the end of the text before it.
    fn break_line(&mut self) {
        if self.pending_space.is_none()
            && let Some(end) = self.current.end()
        {
            self.pending_space = Some((end, end));
        }
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
            self.current.append(&[fold_quotation_mark(unit)], start, end);
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
    LineBreak,
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
    if data.kind.get() == NodeKind::BreakNode {
        return SearchNode::LineBreak;
    }
    if kind_is_box(data.kind.get()) {
        return SearchNode::Break;
    }
    // Generated content isn't the document's text, so find-in-page leaves it out, and the text on either side of an
    // inline pseudo-element reads on as one run, just as it does in the DOM.
    let pseudo = data.generated_for.get();
    if pseudo != 0 && pseudo != GENERATED_FOR_FIRST_LETTER {
        return SearchNode::Skip;
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
            SearchNode::LineBreak => builder.break_line(),
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
    *arena.searchable_text = Some(builder.blocks);
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

fn fold_quotation_mark(unit: u16) -> u16 {
    match unit {
        0x05f4 | 0x201c..=0x201f => 0x22,
        0x05f3 | 0x2018..=0x201b => 0x27,
        _ => unit,
    }
}

unsafe extern "C" {
    fn unicode_layout_text_searcher_create(needle: *const u16, length: usize, case_sensitive: bool) -> *mut c_void;
    fn unicode_layout_text_searcher_has_search_keys(searcher: *mut c_void) -> bool;
    fn unicode_layout_text_is_searchable_without_collator(text: *const u16, length: usize) -> bool;
    fn unicode_layout_text_searcher_set_text(
        searcher: *mut c_void,
        text: *const u16,
        length: usize,
        offset: usize,
        searchable_without_collator: bool,
    );
    fn unicode_layout_text_searcher_next_match(searcher: *mut c_void, start: *mut usize, length: *mut usize) -> bool;
    fn unicode_layout_text_searcher_destroy(searcher: *mut c_void);
}

#[cfg(test)]
mod unicode_test_stubs {
    use std::ffi::c_void;

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_text_searcher_create(
        _needle: *const u16,
        _length: usize,
        _case_sensitive: bool,
    ) -> *mut c_void {
        unreachable!("no unit test searches text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_text_searcher_has_search_keys(_searcher: *mut c_void) -> bool {
        unreachable!("no unit test searches text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_text_is_searchable_without_collator(_text: *const u16, _length: usize) -> bool {
        unreachable!("no unit test searches text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_text_searcher_set_text(
        _searcher: *mut c_void,
        _text: *const u16,
        _length: usize,
        _offset: usize,
        _searchable_without_collator: bool,
    ) {
        unreachable!("no unit test searches text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_text_searcher_next_match(
        _searcher: *mut c_void,
        _start: *mut usize,
        _length: *mut usize,
    ) -> bool {
        unreachable!("no unit test searches text");
    }

    #[unsafe(no_mangle)]
    extern "C" fn unicode_layout_text_searcher_destroy(_searcher: *mut c_void) {
        unreachable!("no unit test searches text");
    }
}

struct TextSearcher(*mut c_void);

impl TextSearcher {
    fn new(query: &[u16], case_sensitive: bool) -> Self {
        // SAFETY: The library copies the query during the call.
        Self(unsafe { unicode_layout_text_searcher_create(query.as_ptr(), query.len(), case_sensitive) })
    }

    fn matches<'a>(&'a mut self, block: &'a MappedText, offset: usize) -> TextMatches<'a> {
        let text = &block.text;
        // SAFETY: The searcher is live.
        let has_search_keys = unsafe { unicode_layout_text_searcher_has_search_keys(self.0) };
        let searchable_without_collator = has_search_keys
            && *block.searchable_without_collator.get_or_init(|| {
                // SAFETY: The library reads the text during the call.
                unsafe { unicode_layout_text_is_searchable_without_collator(text.as_ptr(), text.len()) }
            });
        // SAFETY: The searcher is live, and `TextMatches` borrows the text while the searcher reads it.
        unsafe {
            unicode_layout_text_searcher_set_text(
                self.0,
                text.as_ptr(),
                text.len(),
                offset,
                searchable_without_collator,
            );
        };
        TextMatches {
            searcher: self,
            text: std::marker::PhantomData,
        }
    }
}

impl Drop for TextSearcher {
    fn drop(&mut self) {
        // SAFETY: The searcher was created by the library and is destroyed once.
        unsafe { unicode_layout_text_searcher_destroy(self.0) };
    }
}

struct TextMatches<'a> {
    searcher: &'a mut TextSearcher,
    text: std::marker::PhantomData<&'a [u16]>,
}

impl Iterator for TextMatches<'_> {
    type Item = Range<usize>;

    fn next(&mut self) -> Option<Range<usize>> {
        let (mut start, mut length) = (0, 0);
        // SAFETY: The searcher is live and the text it was given is still borrowed.
        let found =
            unsafe { unicode_layout_text_searcher_next_match(self.searcher.0, &raw mut start, &raw mut length) };
        found.then(|| start..start + length)
    }
}

pub(crate) struct TextMatchCandidate {
    range: FfiDomTextRange,
    block: usize,
    text: Range<usize>,
}

#[derive(Clone, Copy, Default)]
pub(crate) struct TextSearchPosition {
    block: usize,
    offset: usize,
}

pub(crate) struct TextSearch(TextSearcher);

impl TextSearch {
    pub(crate) fn new(query: &[u16], case_sensitive: bool) -> Self {
        let query: Vec<u16> = query.iter().map(|&unit| fold_quotation_mark(unit)).collect();
        Self(TextSearcher::new(&query, case_sensitive))
    }

    /// Up to `limit` places where the query occurs in the searchable text below `viewport` from `position` on, none
    /// overlapping another, and where the search goes on from if there can be more. The searchable text, where it is
    /// built for the search, leaves out the DOM text nodes in `excluded`, sorted.
    pub(crate) fn candidates(
        &mut self,
        arena: &mut LayoutNodeArena,
        viewport: NodeSlotId,
        excluded: &[StyleNodeID],
        position: TextSearchPosition,
        limit: usize,
    ) -> (Vec<TextMatchCandidate>, Option<TextSearchPosition>) {
        ensure_searchable_text(arena, viewport, |text| excluded.binary_search(&text).is_err());
        let mut candidates = Vec::new();
        let blocks = arena.searchable_text.as_ref().expect("search cache was prepared");
        for (index, block) in blocks.iter().enumerate().skip(position.block) {
            let offset = if index == position.block { position.offset } else { 0 };
            for text in self.0.matches(block, offset) {
                let end = text.end;
                if let Some(range) = block.dom_range(arena, text.clone())
                    && (range.start_text_node, range.start_offset) != (range.end_text_node, range.end_offset)
                {
                    candidates.push(TextMatchCandidate {
                        range,
                        block: index,
                        text,
                    });
                }
                if candidates.len() == limit {
                    let position = TextSearchPosition {
                        block: index,
                        offset: end,
                    };
                    return (candidates, Some(position));
                }
            }
        }
        (candidates, None)
    }
}

#[derive(Clone, Copy)]
pub(crate) struct DomTextPosition {
    pub text_node: u32,
    pub offset: usize,
}

pub(crate) fn start_position_at(
    arena: &LayoutNodeArena,
    candidate: &TextMatchCandidate,
    offset: usize,
) -> Option<DomTextPosition> {
    let block = arena.searchable_text.as_ref()?.get(candidate.block)?;
    let position = block.dom_position(arena, offset, Start)?;
    Some(DomTextPosition {
        text_node: text_node_identity(arena, position.node),
        offset: position.offset,
    })
}

pub(crate) struct SearchStart<'a> {
    pub compare: &'a mut dyn FnMut(u32, usize) -> Ordering,
    pub start_position_at: &'a mut dyn FnMut(&TextMatchCandidate, usize) -> Option<DomTextPosition>,
}

/// A match that `take` turns down or that starts before `search_start` hides no match that overlaps it.
pub(crate) fn select_matches(
    mut candidates_from: impl FnMut(TextSearchPosition, usize) -> (Vec<TextMatchCandidate>, Option<TextSearchPosition>),
    mut search_start: Option<SearchStart<'_>>,
    first_match_only: bool,
    mut take: impl FnMut(FfiDomTextRange) -> bool,
) {
    let mut limit = if first_match_only { 1 } else { usize::MAX };
    let (mut candidates, mut next_position) = candidates_from(TextSearchPosition::default(), limit);
    let mut index = 0;
    let mut taken: Option<FfiDomTextRange> = None;
    loop {
        let Some(candidate) = candidates.get(index) else {
            let Some(position) = next_position else {
                return;
            };
            limit = limit.saturating_mul(2);
            (candidates, next_position) = candidates_from(position, limit);
            index = 0;
            continue;
        };
        if taken == Some(candidate.range) {
            index += 1;
            continue;
        }

        let range = candidate.range;
        let mut overlapping_match_offset = candidate.text.start + 1;
        let mut starts_before_search_start = false;
        if let Some(search_start) = search_start.as_mut() {
            let end = (search_start.compare)(range.end_text_node, range.end_offset);
            if end == Ordering::Less {
                index += 1;
                continue;
            }
            // A match that ends at the search start can overlap one that starts there across a line break.
            starts_before_search_start = end == Ordering::Equal
                || (search_start.compare)(range.start_text_node, range.start_offset) == Ordering::Less;
            if starts_before_search_start {
                let mut not_before_search_start = candidate.text.end;
                while overlapping_match_offset < not_before_search_start {
                    let offset = overlapping_match_offset + (not_before_search_start - overlapping_match_offset) / 2;
                    let is_before_search_start =
                        (search_start.start_position_at)(candidate, offset).is_some_and(|position| {
                            (search_start.compare)(position.text_node, position.offset) == Ordering::Less
                        });
                    if is_before_search_start {
                        overlapping_match_offset = offset + 1;
                    } else {
                        not_before_search_start = offset;
                    }
                }
            }
        }
        if !starts_before_search_start && take(range) {
            if first_match_only {
                return;
            }
            taken = Some(range);
            index += 1;
            continue;
        }

        // The matches in hand stay valid unless one overlaps the match that was not taken.
        let position = TextSearchPosition {
            block: candidate.block,
            offset: overlapping_match_offset,
        };
        let (found, position_after_found) = candidates_from(position, 1);
        if let (Some(found), Some(next)) = (found.first(), candidates.get(index + 1))
            && (found.block, &found.text) == (next.block, &next.text)
        {
            index += 1;
            continue;
        }
        (candidates, next_position) = (found, position_after_found);
        index = 0;
        limit = 1;
    }
}

/// The DOM range of the word at `dom_offset` in the text the rows of the text node whose primary row is `primary`
/// render, which input reads in `read` as it selects the word at a point.
#[unsafe(no_mangle)]
pub extern "C" fn layout_text_word_range(
    host: &crate::render_state::DocumentHost,
    primary: NodeSlotId,
    dom_offset: usize,
) -> FfiTextSourceRange {
    host.ask(node_read(), |state| {
        text_word_range(state.arena_mut(), primary, dom_offset)
    })
}

/// The DOM range of the word at `dom_offset` in the text node whose primary row is `primary`, where the word runs on
/// into the text of other DOM text nodes through inline elements. False, leaving `range` alone, where it doesn't.
#[unsafe(no_mangle)]
pub extern "C" fn layout_text_inline_word_range(
    host: &crate::render_state::DocumentHost,
    primary: NodeSlotId,
    dom_offset: usize,
    range: &mut FfiDomTextRange,
) -> bool {
    host.ask(node_read(), |state| {
        match inline_word_range(state.arena_mut(), primary, dom_offset) {
            Some(found) => {
                *range = found;
                true
            }
            None => false,
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
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
        *arena.searchable_text = Some(builder.blocks);
        arena.set_text_content(text, TextContent::for_test("world", 0, 5, Vec::new()));
        assert!(arena.searchable_text.is_none());
        *arena.searchable_text = Some(Vec::new());
        arena
            .free_subtree(text)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        assert!(arena.searchable_text.is_none());
    }

    struct SelectedMatches {
        taken: Vec<(usize, usize)>,
        searches: usize,
        position_lookups: usize,
        listed_candidates: usize,
    }

    fn selected_matches(
        blocks: &[&str],
        block_places: &[usize],
        query: &str,
        search_start: Option<usize>,
        is_selectable: impl Fn(usize) -> bool,
    ) -> SelectedMatches {
        let place = |text_node: u32, offset: usize| {
            let block = blocks[text_node as usize];
            let zero_width_line_feeds = block[..offset].matches('\n').count();
            block_places[text_node as usize] + offset - zero_width_line_feeds
        };
        let mut searches = 0;
        let mut listed_candidates = 0;
        let candidates_from = |position: TextSearchPosition, limit: usize| {
            searches += 1;
            let mut candidates = Vec::new();
            for (index, block) in blocks.iter().enumerate().skip(position.block) {
                let mut offset = if index == position.block { position.offset } else { 0 };
                while let Some(found) = block.get(offset..).and_then(|rest| rest.find(query)) {
                    let text = offset + found..offset + found + query.len();
                    offset = text.end;
                    candidates.push(TextMatchCandidate {
                        range: FfiDomTextRange {
                            start_text_node: index as u32,
                            start_offset: text.start,
                            end_text_node: index as u32,
                            end_offset: text.end,
                        },
                        block: index,
                        text,
                    });
                    if candidates.len() == limit {
                        listed_candidates += candidates.len();
                        let position = TextSearchPosition { block: index, offset };
                        return (candidates, Some(position));
                    }
                }
            }
            listed_candidates += candidates.len();
            (candidates, None)
        };
        let mut compare = |text_node, offset| place(text_node, offset).cmp(&search_start.unwrap());
        let mut position_lookups = 0;
        let mut start_position_at = |candidate: &TextMatchCandidate, offset| {
            position_lookups += 1;
            Some(DomTextPosition {
                text_node: candidate.block as u32,
                offset,
            })
        };
        let mut taken = Vec::new();
        select_matches(
            candidates_from,
            search_start.is_some().then_some(SearchStart {
                compare: &mut compare,
                start_position_at: &mut start_position_at,
            }),
            search_start.is_some(),
            |range| {
                let selectable = is_selectable(place(range.start_text_node, range.start_offset));
                if selectable {
                    taken.push((range.start_text_node as usize, range.start_offset));
                }
                selectable
            },
        );
        SelectedMatches {
            taken,
            searches,
            position_lookups,
            listed_candidates,
        }
    }

    #[test]
    fn matches_do_not_overlap_and_are_searched_for_once_where_all_are_taken() {
        let selected = selected_matches(&["eeeee", "xee"], &[0, 5], "ee", None, |_| true);
        assert_eq!(selected.taken, [(0, 0), (0, 2), (1, 1)]);
        assert_eq!(selected.searches, 1);
    }

    #[test]
    fn a_match_that_is_turned_down_does_not_hide_a_match_that_overlaps_it() {
        let selected = selected_matches(&["eeeee", "xee"], &[0, 5], "ee", None, |place| place != 0);
        assert_eq!(selected.taken, [(0, 1), (0, 3), (1, 1)]);
        let selected = selected_matches(&["eeeee", "xee"], &[0, 5], "ee", None, |_| false);
        assert_eq!(selected.taken, []);
    }

    #[test]
    fn a_turned_down_match_that_no_match_overlaps_costs_one_search() {
        let text = "ab ".repeat(100);
        let selected = selected_matches(&[&text], &[0], "ab", None, |place| place % 2 == 1);
        assert_eq!(selected.taken.len(), 50);
        assert_eq!(selected.taken[0], (0, 3));
        assert_eq!(selected.searches, 1 + 50);
    }

    #[test]
    fn the_first_match_from_the_search_start_can_overlap_a_match_before_it() {
        let first_from =
            |search_start| selected_matches(&["xx", "eeeeee"], &[0, 2], "ee", Some(search_start), |_| true).taken;
        assert_eq!(first_from(0), [(1, 0)]);
        assert_eq!(first_from(4), [(1, 2)]);
        assert_eq!(first_from(5), [(1, 3)]);
        assert_eq!(first_from(6), [(1, 4)]);
        assert_eq!(first_from(7), []);
    }

    #[test]
    fn matches_are_listed_in_growing_batches_until_the_first_is_taken() {
        let text = "x ".repeat(1000);
        let first_from = |search_start| {
            let selected = selected_matches(&[&text], &[0], "x", Some(search_start), |_| true);
            (selected.taken, selected.listed_candidates)
        };
        assert_eq!(first_from(0), (vec![(0, 0)], 1));
        assert_eq!(first_from(300), (vec![(0, 300)], 1 + 2 + 4 + 8 + 16 + 32 + 64 + 128));
        let selected = selected_matches(&[&text], &[0], "x", None, |_| true);
        assert_eq!(selected.taken.len(), 1000);
        assert_eq!(selected.searches, 1);
    }

    #[test]
    fn a_match_across_the_search_start_is_passed_in_one_step() {
        let text = "e".repeat(600);
        let query = "e".repeat(512);
        let selected = selected_matches(&[&text], &[0], &query, Some(77), |_| true);
        assert_eq!(selected.taken, [(0, 77)]);
        assert_eq!(selected.searches, 2);
        assert!(selected.position_lookups <= 10);
    }

    #[test]
    fn a_match_that_ends_at_the_search_start_does_not_hide_a_match_that_starts_there() {
        let selected = selected_matches(&["a\nx\nx\nb"], &[0], "\nx\n", Some(2), |_| true);
        assert_eq!(selected.taken, [(0, 3)]);
        let selected = selected_matches(&["a x\nx\nx b"], &[0], "x\nx", Some(3), |_| true);
        assert_eq!(selected.taken, [(0, 4)]);
    }

    #[test]
    fn the_search_start_is_found_among_matches_that_are_not_in_document_order() {
        let selected = selected_matches(&["x", "x x"], &[10, 0], "x", Some(10), |_| true);
        assert_eq!(selected.taken, [(0, 0)]);
        let selected = selected_matches(&["x", "x x"], &[10, 0], "x", Some(1), |_| true);
        assert_eq!(selected.taken, [(0, 0)]);
        let selected = selected_matches(&["x", "x x"], &[10, 0], "x", Some(11), |_| true);
        assert_eq!(selected.taken, []);
    }

    #[test]
    fn searchable_text_folds_quotation_marks_without_moving_offsets() {
        let mut arena = LayoutNodeArena::new();
        let source = "\u{201c}don\u{2019}t\u{201d}";
        let text = node(&mut arena, source, 0, 7, Vec::new());
        let mut builder = SearchTextBuilder::default();
        builder.append(text, &arena.text_content(text).unwrap().text, false);
        assert_eq!(String::from_utf16_lossy(&builder.current.text), "\"don't\"");
        let range = builder.current.dom_range(&arena, 0..7).unwrap();
        assert_eq!((range.start_offset, range.end_offset), (0, 7));
    }
}
