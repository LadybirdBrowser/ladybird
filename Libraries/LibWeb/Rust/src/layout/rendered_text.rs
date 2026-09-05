/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::node_data::NodeSlotId;
use super::text_transform::{TextRenderingOptions, may_require_bidi_processing, render_text};
use super::{ComputedValuesView, LayoutNodeArena};
use crate::css::css_enums::text_transform;
use crate::css::ffi_support::FfiUtf16View;
use std::cell::OnceCell;
use std::ffi::c_void;

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
#[derive(Default)]
pub(crate) struct TextContent {
    pub(crate) text: Vec<u16>,
    pub(crate) untransformed_text_is_ascii_whitespace: bool,
    pub(crate) may_require_bidi_processing: bool,
    dom_start_offset: usize,
    dom_length_in_code_units: usize,
    edits: Vec<RenderedTextEdit>,
    grapheme_segmenter: OnceCell<super::text_chunker::GraphemeSegmenter>,
    pub(super) rendering_key: Option<TextRenderingKey>,
}

// DOM mutations explicitly invalidate this key. Style changes enroll the node
// for comparison, so unrelated style updates do not rebuild or copy its text.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(super) struct TextRenderingKey {
    options: TextRenderingOptions,
    source_length: usize,
    locale: Option<Vec<u16>>,
}

impl TextContent {
    pub(crate) fn has_same_content_as(&self, other: &Self) -> bool {
        self.text == other.text
            && self.untransformed_text_is_ascii_whitespace == other.untransformed_text_is_ascii_whitespace
            && self.may_require_bidi_processing == other.may_require_bidi_processing
            && self.dom_start_offset == other.dom_start_offset
            && self.dom_length_in_code_units == other.dom_length_in_code_units
            && self.edits == other.edits
    }

    pub(crate) fn grapheme_segmenter(&self) -> &super::text_chunker::GraphemeSegmenter {
        self.grapheme_segmenter
            .get_or_init(|| super::text_chunker::GraphemeSegmenter::new(&self.text))
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

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiTextSource {
    pub text: FfiUtf16View,
    pub locale: FfiUtf16View,
    pub has_locale: bool,
    pub is_password_input: bool,
    pub dom_start_offset: usize,
    pub dom_length_in_code_units: usize,
}

#[repr(C)]
pub struct FfiRenderedTextView {
    pub text: *const u16,
    pub length_in_code_units: usize,
}

/// # Safety
///
/// The arena must be exclusively available and `id` must name a live node.
/// Source and optional locale views must remain readable throughout this call.
/// The DOM range must be contained in the source text.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_sync_text_content(arena: *mut c_void, id: NodeSlotId, input: FfiTextSource) {
    // SAFETY: The host lends the arena exclusively; Unicode callbacks only
    // access their input views and output buffers, never the arena.
    let arena = unsafe { LayoutNodeArena::from_handle_mut(arena) };
    let parent = arena.data(id).parent.get();
    let inherited = ComputedValuesView::new(
        &arena
            .style_payloads(parent)
            .expect("text parent must have style")
            .groups,
    )
    .inherited_text();
    let options = TextRenderingOptions {
        text_transform: inherited.text_transform,
        white_space_collapse: inherited.white_space_collapse,
        is_password_input: input.is_password_input,
        dom_start_offset: input.dom_start_offset,
        dom_length_in_code_units: input.dom_length_in_code_units,
    };
    let uses_locale = matches!(
        options.text_transform,
        text_transform::LOWERCASE | text_transform::UPPERCASE | text_transform::CAPITALIZE
    );
    // SAFETY: The host lends the locale view for this call.
    let locale = (uses_locale && input.has_locale)
        .then(|| unsafe { input.locale.to_utf16() }.expect("text locale carries no storage"));
    let key = TextRenderingKey {
        options,
        source_length: input.text.length,
        locale,
    };
    if !arena
        .text_content(id)
        .is_some_and(|content| content.rendering_key.as_ref() == Some(&key))
    {
        // SAFETY: The host lends the source view for this synchronous build.
        let source = unsafe { input.text.to_utf16() }.expect("text source carries no storage");
        let untransformed_text_is_ascii_whitespace = source.iter().all(|unit| matches!(unit, 0x09..=0x0d | 0x20));
        let rendered = render_text(source, key.locale.as_deref(), key.options);
        let content = TextContent {
            may_require_bidi_processing: may_require_bidi_processing(&rendered.text),
            text: rendered.text,
            untransformed_text_is_ascii_whitespace,
            dom_start_offset: input.dom_start_offset,
            dom_length_in_code_units: input.dom_length_in_code_units,
            edits: rendered.edits,
            grapheme_segmenter: OnceCell::new(),
            rendering_key: Some(key),
        };
        arena.set_text_content(id, content);
    }
    arena.finish_text_content_sync(id);
}

/// # Safety
///
/// The arena must be exclusively available and `id` must name a live text node.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_invalidate_text_content(arena: *mut c_void, id: NodeSlotId) {
    // SAFETY: DOM mutation publishes invalidation outside layout and painting.
    unsafe { LayoutNodeArena::from_handle_mut(arena) }.invalidate_text_content(id);
}

/// # Safety
///
/// `id` must name a live node with published text. The returned view is borrowed
/// until that node's text is republished or the node is freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn layout_arena_text_for_rendering(arena: *mut c_void, id: NodeSlotId) -> FfiRenderedTextView {
    // SAFETY: The host keeps the arena and its published text live during the read.
    let content = unsafe { LayoutNodeArena::from_handle(arena) }
        .text_content(id)
        .expect("text must be published before borrowing its rendered view");
    FfiRenderedTextView {
        text: content.text.as_ptr(),
        length_in_code_units: content.text.len(),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::layout::node_data::NodeKind;
    use RenderedTextBoundary::{End, Start};

    fn content(text: &str, dom_start: usize, dom_length: usize, edits: Vec<RenderedTextEdit>) -> TextContent {
        TextContent {
            text: text.encode_utf16().collect(),
            dom_start_offset: dom_start,
            dom_length_in_code_units: dom_length,
            edits,
            ..TextContent::default()
        }
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
        arena.data(parent).kind.set(NodeKind::BlockContainer);
        arena.data(node).kind.set(NodeKind::TextNode);
        arena.data(node).parent.set(parent);
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
        arena.data(node).kind.set(NodeKind::TextNode);
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
        assert!(arena.take_text_nodes_for_content_sync().is_empty());
    }

    #[test]
    fn native_style_and_dom_notifications_share_one_pending_enrollment() {
        let mut arena = LayoutNodeArena::new();
        let node = arena.allocate_for_test().slot;
        arena.data(node).kind.set(NodeKind::TextNode);
        arena.enroll_text_node_for_content_sync(node);
        arena.invalidate_text_content(node);
        arena.enroll_text_node_for_content_sync(node);
        let pending = arena.take_text_nodes_for_content_sync();
        assert_eq!(pending.len(), 1);
        assert!(pending.contains(&node));

        arena.enroll_text_node_for_content_sync(node);
        arena.finish_text_content_sync(node);
        assert!(arena.take_text_nodes_for_content_sync().is_empty());
    }
}
