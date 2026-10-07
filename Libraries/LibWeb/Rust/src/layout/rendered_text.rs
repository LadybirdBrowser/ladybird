/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::node_data::NodeSlotId;
use super::text_transform::{TextRenderingOptions, may_require_bidi_processing, render_text};
use super::{ComputedValuesView, LayoutNodeArena};
use crate::css::css_enums::text_transform;
use std::cell::{OnceCell, RefCell};
use std::sync::Arc;

/// Selects the beginning or end of a transformed span for offsets inside it.
#[derive(Clone, Copy)]
#[repr(C)]
pub enum RenderedTextBoundary {
    Start,
    End,
}

/// Length-preserving regions have an implicit one-to-one mapping. Edits name
/// absolute DOM offsets and offsets relative to this layout node's rendered text.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct RenderedTextEdit {
    pub dom_start_offset: usize,
    pub dom_length_in_code_units: usize,
    pub rendered_start_offset: usize,
    pub rendered_length_in_code_units: usize,
}

/// Rendered text and its DOM offset mapping are published and invalidated together.
/// Rust builds this snapshot from source text and rendering options; layout and painting read it.
/// It is immutable once built, so a publication shares it.
#[derive(Default)]
pub(crate) struct RenderedText {
    pub(crate) text: Vec<u16>,
    pub(crate) untransformed_text_is_ascii_whitespace: bool,
    pub(crate) may_require_bidi_processing: bool,
    dom_start_offset: usize,
    dom_length_in_code_units: usize,
    edits: Vec<RenderedTextEdit>,
    /// The DOM text as it was written, kept only under an SVG box: SVG text shapes the element's
    /// raw character data, not the white-space-collapsed rendering every other box uses. Text on a
    /// path also reads text whose parent is not a text box, such as an <a> inside a <textPath>.
    pub(crate) svg_source_text: Option<Box<[u16]>>,
}

/// What a text row publishes for the paint side: its rendered text, and the first-letter row that
/// renders the start of its source.
#[derive(Clone)]
pub(crate) struct PublishedTextSlot {
    pub(crate) generation: u8,
    pub(crate) first_letter: NodeSlotId,
    pub(crate) rendered: Option<Arc<RenderedText>>,
}

impl Default for PublishedTextSlot {
    fn default() -> Self {
        Self {
            generation: 0,
            first_letter: NodeSlotId::INVALID,
            rendered: None,
        }
    }
}

/// A slot is the same as another when it publishes the same rendered text: rendered text is
/// never written once it is shared.
impl PartialEq for PublishedTextSlot {
    fn eq(&self, other: &Self) -> bool {
        self.generation == other.generation
            && self.first_letter == other.first_letter
            && crate::cow_column::same_payload(self.rendered.as_ref(), other.rendered.as_ref(), |_, _| false)
    }
}

/// A text row's rendered text, with what layout caches beside it.
#[derive(Clone, Default)]
pub(crate) struct TextContent {
    rendered: Arc<RenderedText>,
    grapheme_segmenter: crate::fork::ForkReset<OnceCell<super::text_chunker::GraphemeSegmenter>>,
    chunks: RefCell<Option<Arc<CachedTextChunks>>>,
    pub(super) rendering_key: Option<TextRenderingKey>,
}

impl std::ops::Deref for TextContent {
    type Target = RenderedText;

    fn deref(&self) -> &RenderedText {
        &self.rendered
    }
}

// DOM mutations explicitly invalidate this key. Style changes enroll the node
// for comparison, so unrelated style updates do not rebuild or copy its text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct TextRenderingKey {
    options: TextRenderingOptions,
    source_length: usize,
    locale: Option<Vec<u16>>,
}

fn transform_uses_locale(transform: u8) -> bool {
    matches!(
        transform,
        text_transform::LOWERCASE | text_transform::UPPERCASE | text_transform::CAPITALIZE
    )
}

/// Enrolls the text under `root` whose casing depends on the language it renders with, and answers whether there was
/// any. This only enrolls text; source views are requested after DOM language invalidation completes.
pub(crate) fn enroll_text_after_language_change(arena: &LayoutNodeArena, root: NodeSlotId) -> bool {
    let mut changed = false;
    let mut enroll = |node| {
        if !super::node_facts::kind_is_text(arena.data(node).kind.get()) {
            return;
        }
        let parent = arena.data(node).parent.get();
        if !parent.is_invalid()
            && arena.style_payloads(parent).is_some_and(|style| {
                transform_uses_locale(ComputedValuesView::new(&style.groups).inherited_text().text_transform)
            })
        {
            arena.enroll_text_node_for_content_sync(node);
            changed = true;
        }
    };
    if super::node_facts::kind_is_text(arena.data(root).kind.get()) {
        for &node in arena.text_fragments(root).as_slice() {
            enroll(node);
        }
    } else {
        arena.for_each_node_in_layout_subtree_in_pre_order(root, enroll);
    }
    changed
}

#[derive(Clone)]
pub(crate) struct TextChunkCacheKey {
    pub(crate) should_wrap_lines: bool,
    pub(crate) should_respect_linebreaks: bool,
    pub(crate) unidirectional_ltr: bool,
    pub(crate) white_space_collapse: u8,
    pub(crate) word_break: u8,
    pub(crate) font_variant_emoji: u8,
    pub(crate) frozen_font_list: std::sync::Arc<libgfx_rust::font::FrozenFontList>,
}

impl PartialEq for TextChunkCacheKey {
    fn eq(&self, other: &Self) -> bool {
        self.should_wrap_lines == other.should_wrap_lines
            && self.should_respect_linebreaks == other.should_respect_linebreaks
            && self.unidirectional_ltr == other.unidirectional_ltr
            && self.white_space_collapse == other.white_space_collapse
            && self.word_break == other.word_break
            && self.font_variant_emoji == other.font_variant_emoji
            // A frozen cascade is immutable, so naming the same one is the whole of equality.
            && std::sync::Arc::ptr_eq(&self.frozen_font_list, &other.frozen_font_list)
    }
}

pub(crate) struct CachedTextChunks {
    key: TextChunkCacheKey,
    chunks: Vec<super::text_chunker::TextChunk>,
}

impl std::ops::Deref for CachedTextChunks {
    type Target = [super::text_chunker::TextChunk];

    fn deref(&self) -> &Self::Target {
        &self.chunks
    }
}

impl TextContent {
    #[cfg(test)]
    pub(crate) fn for_test(text: &str, dom_start: usize, dom_length: usize, edits: Vec<RenderedTextEdit>) -> Self {
        Self {
            rendered: Arc::new(RenderedText {
                text: text.encode_utf16().collect(),
                dom_start_offset: dom_start,
                dom_length_in_code_units: dom_length,
                edits,
                ..RenderedText::default()
            }),
            ..Self::default()
        }
    }

    /// The rendered text, as a text row publishes it.
    pub(crate) fn rendered(&self) -> &Arc<RenderedText> {
        &self.rendered
    }

    pub(super) fn is_password_input(&self) -> bool {
        self.rendering_key
            .as_ref()
            .is_some_and(|key| key.options.is_password_input)
    }

    pub(crate) fn text_chunks(
        &self,
        key: &TextChunkCacheKey,
        compute: impl FnOnce() -> Vec<super::text_chunker::TextChunk>,
    ) -> Arc<CachedTextChunks> {
        if let Some(entry) = self.chunks.borrow().as_ref()
            && entry.key == *key
        {
            return entry.clone();
        }

        // A nested measurement can request a different key while an iterator
        // still uses the previous chunks. Keep the chunks and their fonts alive
        // until that iterator finishes, even if this snapshot is replaced.
        let entry = Arc::new(CachedTextChunks {
            key: key.clone(),
            chunks: compute(),
        });
        *self.chunks.borrow_mut() = Some(entry.clone());
        entry
    }

    pub(crate) fn grapheme_segmenter(&self) -> &super::text_chunker::GraphemeSegmenter {
        self.grapheme_segmenter
            .get_or_init(|| super::text_chunker::GraphemeSegmenter::new(&self.text))
    }
}

impl RenderedText {
    pub(super) fn dom_range(&self) -> std::ops::Range<usize> {
        self.dom_start_offset..self.dom_start_offset + self.dom_length_in_code_units
    }

    pub(crate) fn has_same_content_as(&self, other: &Self) -> bool {
        self.text == other.text
            && self.untransformed_text_is_ascii_whitespace == other.untransformed_text_is_ascii_whitespace
            && self.may_require_bidi_processing == other.may_require_bidi_processing
            && self.dom_start_offset == other.dom_start_offset
            && self.dom_length_in_code_units == other.dom_length_in_code_units
            && self.edits == other.edits
            && self.svg_source_text == other.svg_source_text
    }

    pub(crate) fn dom_offset_for_rendered_text_offset(&self, offset: usize, boundary: RenderedTextBoundary) -> usize {
        let offset = offset.min(self.text.len());
        let mut previous_dom_end = self.dom_start_offset;
        let mut previous_rendered_end = 0;
        for edit in &self.edits {
            if offset < edit.rendered_start_offset {
                return previous_dom_end + offset - previous_rendered_end;
            }

            let dom_end = edit.dom_start_offset + edit.dom_length_in_code_units;
            let rendered_end = edit.rendered_start_offset + edit.rendered_length_in_code_units;
            if offset == edit.rendered_start_offset {
                if edit.rendered_length_in_code_units > 0 || matches!(boundary, RenderedTextBoundary::End) {
                    return edit.dom_start_offset;
                }
            } else if offset < rendered_end {
                return match boundary {
                    RenderedTextBoundary::Start => edit.dom_start_offset,
                    RenderedTextBoundary::End => dom_end,
                };
            } else if offset == rendered_end && matches!(boundary, RenderedTextBoundary::End) {
                return dom_end;
            }

            previous_dom_end = dom_end;
            previous_rendered_end = rendered_end;
        }
        previous_dom_end + offset - previous_rendered_end
    }

    pub(crate) fn rendered_text_offset_for_dom_offset(&self, offset: usize, boundary: RenderedTextBoundary) -> usize {
        let offset = offset.clamp(
            self.dom_start_offset,
            self.dom_start_offset + self.dom_length_in_code_units,
        );
        rendered_text_offset_for_dom_offset(&self.edits, self.dom_start_offset, offset, boundary).min(self.text.len())
    }
}

pub(super) fn rendered_text_offset_for_dom_offset(
    edits: &[RenderedTextEdit],
    dom_base_offset: usize,
    offset: usize,
    boundary: RenderedTextBoundary,
) -> usize {
    let mut previous_dom_end = dom_base_offset;
    let mut previous_rendered_end = 0;
    for edit in edits {
        if offset < edit.dom_start_offset {
            return previous_rendered_end + offset - previous_dom_end;
        }

        let dom_end = edit.dom_start_offset + edit.dom_length_in_code_units;
        let rendered_end = edit.rendered_start_offset + edit.rendered_length_in_code_units;
        if offset <= dom_end {
            if offset == edit.dom_start_offset {
                return edit.rendered_start_offset;
            }
            if offset == dom_end {
                return rendered_end;
            }
            return match boundary {
                RenderedTextBoundary::Start => edit.rendered_start_offset,
                RenderedTextBoundary::End => rendered_end,
            };
        }

        previous_dom_end = dom_end;
        previous_rendered_end = rendered_end;
    }
    previous_rendered_end + offset - previous_dom_end
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct FfiTextSourceRange {
    pub start: usize,
    pub length: usize,
}

/// Layout fragments of one DOM text node, in source order with the primary last.
pub(crate) struct TextFragments {
    pub nodes: [NodeSlotId; 2],
    pub length: usize,
}

impl TextFragments {
    pub(crate) fn as_slice(&self) -> &[NodeSlotId] {
        &self.nodes[..self.length]
    }
}

/// The code units a published string spells, without widening ASCII storage to count them.
pub(super) fn length_in_code_units(text: &ak::Utf16String) -> usize {
    match text.as_units() {
        ak::Utf16StringUnits::Ascii(units) => units.len(),
        ak::Utf16StringUnits::Utf16(units) => units.len(),
    }
}

/// Publishes the rendered text of `id`, a live text node with a styled parent, where what it renders changed.
pub(super) fn ensure_text_content(arena: &mut LayoutNodeArena, id: NodeSlotId) {
    if arena.text_content_needs_sync(id) {
        sync_text_content(arena, id);
    }
}

fn sync_text_content(arena: &mut LayoutNodeArena, id: NodeSlotId) {
    let parent = arena.data(id).parent.get();
    let inherited = ComputedValuesView::new(
        &arena
            .style_payloads(parent)
            .expect("text parent must have style")
            .groups,
    )
    .inherited_text();
    let uses_locale = transform_uses_locale(inherited.text_transform);
    let input = arena.published_text_source(id, uses_locale);
    let source_length = length_in_code_units(&input.data);
    let source_range = arena.text_source_range(id, source_length);
    let options = TextRenderingOptions {
        text_transform: inherited.text_transform,
        white_space_collapse: inherited.white_space_collapse,
        is_password_input: input.is_password_input,
        dom_start_offset: source_range.start,
        dom_length_in_code_units: source_range.length,
    };
    let key = TextRenderingKey {
        options,
        source_length,
        locale: input.locale,
    };
    if !arena
        .text_content(id)
        .is_some_and(|content| content.rendering_key.as_ref() == Some(&key))
    {
        let source = input.data.to_utf16().into_owned();
        let untransformed_text_is_ascii_whitespace = source.iter().all(|unit| matches!(unit, 0x09..=0x0d | 0x20));
        let svg_source_text =
            super::node_facts::kind_is_svg_box(arena.data(parent).kind.get()).then(|| Box::from(&source[..]));
        let rendered = render_text(source, key.locale.as_deref(), key.options);
        let content = TextContent {
            rendered: Arc::new(RenderedText {
                svg_source_text,
                may_require_bidi_processing: may_require_bidi_processing(&rendered.text),
                text: rendered.text,
                untransformed_text_is_ascii_whitespace,
                dom_start_offset: source_range.start,
                dom_length_in_code_units: source_range.length,
                edits: rendered.edits,
            }),
            grapheme_segmenter: crate::fork::ForkReset::default(),
            chunks: RefCell::default(),
            rendering_key: Some(key),
        };
        arena.set_text_content(id, content);
    }
    arena.finish_text_content_sync(id);
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::SLOTS_PER_CHUNK;
    use crate::layout::node_data::NodeKind;
    use crate::painting::paint_read::PaintRead;
    use RenderedTextBoundary::{End, Start};

    fn content(text: &str, dom_start: usize, dom_length: usize, edits: Vec<RenderedTextEdit>) -> TextContent {
        TextContent::for_test(text, dom_start, dom_length, edits)
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
    fn text_chunk_users_survive_cache_and_snapshot_replacement() {
        use super::TextChunkCacheKey;
        use crate::layout::text_chunker::TextChunk;
        use std::sync::Arc;

        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.set_text_content(node, content("hello", 0, 5, Vec::new()));
        let key = TextChunkCacheKey {
            should_wrap_lines: true,
            should_respect_linebreaks: false,
            unidirectional_ltr: true,
            white_space_collapse: 0,
            word_break: 0,
            font_variant_emoji: 0,
            frozen_font_list: std::sync::Arc::new(libgfx_rust::font::FrozenFontList::empty()),
        };
        let chunk = TextChunk {
            start: 0,
            length: 5,
            font: unsafe { libgfx_rust::font::FontHandle::intern(std::ptr::dangling()) },
            has_breaking_newline: false,
            has_breaking_tab: false,
            is_all_whitespace: false,
            can_break_after: true,
            text_type: 0,
        };
        let original = arena
            .text_content(node)
            .unwrap()
            .text_chunks(&key, || vec![chunk.clone()]);
        let hit = arena
            .text_content(node)
            .unwrap()
            .text_chunks(&key, || panic!("matching chunks should be cached"));
        assert!(Arc::ptr_eq(&original, &hit));
        drop(hit);
        let original_weak = Arc::downgrade(&original);
        let replacement = arena.text_content(node).unwrap().text_chunks(
            &TextChunkCacheKey {
                should_wrap_lines: false,
                ..key.clone()
            },
            Vec::new,
        );
        assert!(replacement.is_empty());
        assert_eq!(&**original, std::slice::from_ref(&chunk));
        assert_eq!(Arc::strong_count(&original), 1);
        drop(original);
        assert!(original_weak.upgrade().is_none());

        let replacement_weak = Arc::downgrade(&replacement);
        let published = arena.publish_paint_facts().text;
        let published_text = |published: &crate::cow_column::ColumnSnapshot<PublishedTextSlot, SLOTS_PER_CHUNK>| {
            published
                .get(node.slot_index() as usize)
                .unwrap()
                .rendered
                .clone()
                .unwrap()
        };
        let published_hello = published_text(&published);
        arena.set_text_content(node, content("hello", 0, 5, Vec::new()));
        assert_eq!(Arc::strong_count(&replacement), 2);
        // The same content keeps the rendered text the publication shares.
        assert!(std::sync::Arc::ptr_eq(
            &published_hello,
            &published_text(&arena.publish_paint_facts().text)
        ));
        arena.set_text_content(node, content("goodbye", 0, 7, Vec::new()));
        assert_eq!(Arc::strong_count(&replacement), 1);
        // A publication keeps the rendered text it was made with.
        assert_eq!(
            published_text(&published).text,
            "hello".encode_utf16().collect::<Vec<_>>()
        );
        assert_eq!(
            published_text(&arena.publish_paint_facts().text).text,
            "goodbye".encode_utf16().collect::<Vec<_>>()
        );
        let new_chunks = arena
            .text_content(node)
            .unwrap()
            .text_chunks(&key, || vec![chunk.clone()]);
        assert_eq!(&**new_chunks, std::slice::from_ref(&chunk));
        drop(replacement);
        assert!(replacement_weak.upgrade().is_none());
        let _ = arena.free_subtree(node);
        assert_eq!(Arc::strong_count(&new_chunks), 1);
    }

    #[test]
    fn identity_mapping_clamps_offsets_to_the_dom_slice() {
        let text = content("hello", 3, 5, Vec::new());
        for boundary in [Start, End] {
            assert_eq!(text.rendered_text_offset_for_dom_offset(0, boundary), 0);
            assert_eq!(text.rendered_text_offset_for_dom_offset(5, boundary), 2);
            assert_eq!(text.rendered_text_offset_for_dom_offset(usize::MAX, boundary), 5);
            assert_eq!(text.dom_offset_for_rendered_text_offset(0, boundary), 3);
            assert_eq!(text.dom_offset_for_rendered_text_offset(2, boundary), 5);
            assert_eq!(text.dom_offset_for_rendered_text_offset(usize::MAX, boundary), 8);
        }
    }

    #[test]
    fn expansions_contractions_and_deletions_preserve_boundary_choices() {
        // Combine the edits used for uppercasing sharp s, masking a surrogate
        // pair, and deleting a combining dot in contextual lowercasing.
        let text = content(
            "SSa●iX",
            0,
            7,
            vec![edit(0, 1, 0, 2), edit(2, 2, 3, 1), edit(5, 1, 5, 0)],
        );
        let dom_boundaries = [(0, 0), (0, 1), (1, 1), (2, 2), (4, 4), (6, 5), (7, 7)];
        for (offset, (start, end)) in dom_boundaries.into_iter().enumerate() {
            assert_eq!(text.dom_offset_for_rendered_text_offset(offset, Start), start);
            assert_eq!(text.dom_offset_for_rendered_text_offset(offset, End), end);
        }
        let rendered_boundaries = [(0, 0), (2, 2), (3, 3), (3, 4), (4, 4), (5, 5), (5, 5), (6, 6)];
        for (offset, (start, end)) in rendered_boundaries.into_iter().enumerate() {
            assert_eq!(text.rendered_text_offset_for_dom_offset(offset, Start), start);
            assert_eq!(text.rendered_text_offset_for_dom_offset(offset, End), end);
        }
    }

    #[test]
    fn first_letter_mapping_uses_absolute_dom_and_slice_relative_rendered_offsets() {
        let complete_edits = [edit(2, 1, 2, 2)];
        assert_eq!(rendered_text_offset_for_dom_offset(&complete_edits, 0, 2, Start), 2);
        assert_eq!(rendered_text_offset_for_dom_offset(&complete_edits, 0, 3, End), 4);

        let first_letter = content("SS", 2, 1, vec![edit(2, 1, 0, 2)]);
        assert_eq!(first_letter.dom_offset_for_rendered_text_offset(1, Start), 2);
        assert_eq!(first_letter.dom_offset_for_rendered_text_offset(1, End), 3);
        assert_eq!(first_letter.rendered_text_offset_for_dom_offset(0, Start), 0);
        assert_eq!(first_letter.rendered_text_offset_for_dom_offset(usize::MAX, End), 2);

        let remainder = content("ello", 3, 4, Vec::new());
        assert_eq!(remainder.dom_offset_for_rendered_text_offset(0, Start), 3);
        assert_eq!(remainder.rendered_text_offset_for_dom_offset(4, End), 1);
    }

    #[test]
    fn mapping_only_publication_invalidates_layout_without_a_cpp_shell() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate_for_test().slot;
        let node = arena.allocate_for_test().slot;
        arena.write_shape(parent).set_kind(NodeKind::BlockContainer);
        arena.write_shape(node).set_kind(NodeKind::TextNode);
        arena.write_shape(node).set_parent(parent);
        let sharp_s_first = || content("SSS", 0, 2, vec![edit(0, 1, 0, 2)]);
        arena.set_text_content(node, sharp_s_first());
        let epoch = arena.data(parent).fragment_cache_epoch.get();
        assert_eq!(arena.rendered_text_offset_for_dom_offset(node, 1, Start), 2);
        arena.set_text_content(node, sharp_s_first());
        assert_eq!(arena.data(parent).fragment_cache_epoch.get(), epoch);

        arena.set_text_content(node, content("SSS", 0, 2, vec![edit(1, 1, 1, 2)]));
        assert_ne!(arena.data(parent).fragment_cache_epoch.get(), epoch);
        assert_eq!(arena.rendered_text_offset_for_dom_offset(node, 1, Start), 1);
        assert_eq!(arena.dom_offset_for_rendered_text_offset(node, 2, End), 2);
    }

    #[test]
    fn refreshing_the_key_preserves_identical_text_storage_and_layout() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.write_shape(node).set_kind(NodeKind::TextNode);
        let key = TextRenderingKey {
            options: TextRenderingOptions {
                text_transform: text_transform::UPPERCASE,
                white_space_collapse: crate::css::css_enums::white_space_collapse::PRESERVE,
                is_password_input: false,
                dom_start_offset: 0,
                dom_length_in_code_units: 3,
            },
            source_length: 3,
            locale: None,
        };
        let mut original = content("123", 0, 3, Vec::new());
        original.rendering_key = Some(key.clone());
        arena.set_text_content(node, original);
        let storage = arena.text_content(node).unwrap().text.as_ptr();
        let epoch = arena.data(node).fragment_cache_epoch.get();

        arena.invalidate_text_content(node);
        assert!(arena.text_content(node).unwrap().rendering_key.is_none());
        let mut new_key = key;
        new_key.options.text_transform = text_transform::LOWERCASE;
        let mut rebuilt = content("123", 0, 3, Vec::new());
        rebuilt.rendering_key = Some(new_key.clone());
        arena.set_text_content(node, rebuilt);
        arena.finish_text_content_sync(node);

        assert_eq!(arena.text_content(node).unwrap().rendering_key.as_ref(), Some(&new_key));
        assert_eq!(arena.text_content(node).unwrap().text.as_ptr(), storage);
        assert_eq!(arena.data(node).fragment_cache_epoch.get(), epoch);
        assert!(arena.pending_text_nodes_for_content_sync().is_empty());
    }

    #[test]
    fn native_style_and_dom_notifications_share_one_pending_enrollment() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.write_shape(node).set_kind(NodeKind::TextNode);
        arena.enroll_text_node_for_content_sync(node);
        arena.invalidate_text_content(node);
        arena.enroll_text_node_for_content_sync(node);
        let pending = arena.pending_text_nodes_for_content_sync();
        assert_eq!(pending.len(), 1);
        assert!(pending.contains(&node));

        arena.enroll_text_node_for_content_sync(node);
        arena.finish_text_content_sync(node);
        assert!(arena.pending_text_nodes_for_content_sync().is_empty());
    }

    #[test]
    fn clean_reads_skip_source_reads_and_pending_style_reads_do_not() {
        let mut arena = LayoutNodeArena::new();
        let parent = arena.allocate_for_test().slot;
        let node = arena.allocate_for_test().slot;
        arena.write_shape(node).set_kind(NodeKind::GeneratedTextNode);
        arena.write_shape(node).set_parent(parent);
        arena.write_shape(parent).set_first_child(node);
        let hello = ak::Utf16FlyString::from_utf16(&"hello".encode_utf16().collect::<Vec<_>>());
        arena.set_generated_text(node, ak::Utf16String::from(hello));
        let mut text = content("hello", 0, 5, Vec::new());
        text.rendering_key = Some(TextRenderingKey {
            options: TextRenderingOptions {
                text_transform: text_transform::NONE,
                white_space_collapse: crate::css::css_enums::white_space_collapse::COLLAPSE,
                is_password_input: false,
                dom_start_offset: 0,
                dom_length_in_code_units: 5,
            },
            source_length: 5,
            locale: None,
        });
        arena.set_text_content(node, text);
        assert!(!arena.text_content_needs_sync(node));

        arena.set_node_style(parent, 1, crate::layout::node_data::StylePayloadsRef::null());
        let pending = arena.pending_text_nodes_for_content_sync();
        assert_eq!(pending, [node]);
        assert!(arena.text_content_needs_sync(node));
        assert_eq!(length_in_code_units(&arena.published_text_source(node, false).data), 5);
        arena.finish_text_content_sync(node);
        assert!(!arena.text_content_needs_sync(node));

        arena.invalidate_text_content(node);
        assert!(arena.text_content_needs_sync(node));
    }

    fn first_letter_slices(arena: &mut LayoutNodeArena, end: usize, length: usize) -> (NodeSlotId, NodeSlotId) {
        let first = arena.allocate_for_test().slot;
        let remainder = arena.allocate_for_test().slot;
        arena.write_shape(first).set_kind(NodeKind::TextNode);
        arena.write_shape(remainder).set_kind(NodeKind::TextNode);
        arena.set_first_letter_slices(first, remainder, end, length);
        (first, remainder)
    }

    #[test]
    fn slice_ranges_and_relationships_survive_content_publication() {
        let mut arena = LayoutNodeArena::new();
        let (first, remainder) = first_letter_slices(&mut arena, 2, 5);
        assert!(arena.text_has_source_range(first));
        assert!(arena.text_has_source_range(remainder));
        assert_eq!(arena.text_fragments(remainder).as_slice(), &[first, remainder]);

        arena.set_text_content(first, content("«SS", 0, 2, vec![edit(1, 1, 1, 2)]));
        arena.set_text_content(remainder, content("abc", 2, 3, Vec::new()));
        arena.invalidate_text_content(first);
        arena.set_text_content(first, content("«ß", 0, 2, Vec::new()));
        assert_eq!(
            arena.text_source_range(first, 5),
            FfiTextSourceRange { start: 0, length: 2 }
        );
        assert_eq!(
            arena.text_source_range(remainder, 5),
            FfiTextSourceRange { start: 2, length: 3 }
        );
        assert_eq!(arena.text_fragments(remainder).as_slice(), &[first, remainder]);
        assert_eq!(arena.text_fragments(first).as_slice(), &[first]);
    }

    #[test]
    fn freed_slices_do_not_resolve_to_reused_slots() {
        let mut arena = LayoutNodeArena::new();
        let (first, remainder) = first_letter_slices(&mut arena, 1, 1);
        assert_eq!(
            arena.text_source_range(remainder, 1),
            FfiTextSourceRange { start: 1, length: 0 }
        );
        let _ = arena.free_subtree(first);
        let replacement = arena.allocate_for_test().slot;
        arena.write_shape(replacement).set_kind(NodeKind::TextNode);
        assert_eq!(replacement.slot_index(), first.slot_index());
        assert_ne!(replacement, first);
        assert!(!arena.text_has_source_range(first));
        assert!(!arena.text_has_source_range(replacement));
        assert_eq!(arena.text_fragments(remainder).as_slice(), &[remainder]);
        assert_eq!(
            arena.text_source_range(replacement, 4),
            FfiTextSourceRange { start: 0, length: 4 }
        );

        let _ = arena.free_subtree(remainder);
        let replacement = arena.allocate_for_test().slot;
        arena.write_shape(replacement).set_kind(NodeKind::TextNode);
        assert_eq!(replacement.slot_index(), remainder.slot_index());
        assert!(arena.text_fragments(remainder).as_slice().is_empty());
        assert_eq!(arena.text_fragments(replacement).as_slice(), &[replacement]);
        assert!(arena.text_fragments(NodeSlotId::INVALID).as_slice().is_empty());
    }

    #[test]
    fn selection_entries_expand_the_primary_text_node_to_both_slices() {
        use crate::painting::paintable_data::{FfiSelectionEntry, SELECTION_STATE_START_AND_END};

        let mut arena = LayoutNodeArena::new();
        let viewport = arena.allocate_for_test().slot;
        arena.populate_paintable_row(viewport);
        let (first, remainder) = first_letter_slices(&mut arena, 1, 4);
        let entries = [FfiSelectionEntry {
            is_text_node_entry: true,
            layout_node: remainder,
            state: SELECTION_STATE_START_AND_END,
        }];
        let states = crate::painting::selection::apply(&mut arena.paintable_rows_mut(), viewport, &entries);
        assert_eq!(states.len(), 2);
        assert_eq!(states.get(&first), Some(&SELECTION_STATE_START_AND_END));
        assert_eq!(states.get(&remainder), Some(&SELECTION_STATE_START_AND_END));
    }
}
