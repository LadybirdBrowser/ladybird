/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the layout tree build puts inside a pseudo-element's box: for now, the counter styles its
//! generated content renders from, which a later style change compares against.

use super::counters::{CounterOwner, style_of};
use super::layout_node_arena::LayoutNodeArena;
use super::node_data::{GENERATED_FOR_MARKER, NodeSlotId};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::counter_representation::{CounterStyle, resolve_counter_style_value};
use crate::css::css_enums::keyword;
use crate::css::style::fast_hash::FastMap as HashMap;
use crate::css::style::tree::StyleNodeID;
use crate::css::style_value::StyleValueData;
use std::rc::Rc;

/// What a list marker whose `content` is `normal` shows, resolved from the published records of the
/// marker box and of the list item box it belongs to.
enum MarkerContent {
    Image,
    String,
    /// A counter style, or `None` for a name that resolves to none.
    CounterStyle(Option<Rc<CounterStyle>>),
}

/// What the tree build recorded for the generated content of each pseudo-element it built a box for.
#[derive(Default)]
pub(crate) struct GeneratedContent {
    /// The counter styles the box built for a pseudo-element was rendered from, one per
    /// `counter()` or `counters()` the content names and one for the counter style a normal
    /// marker shows. A later style change compares what the pseudo-element's record names now
    /// against this to decide whether the box has to be rebuilt.
    content_counter_styles_in_use: HashMap<CounterOwner, Vec<Option<Rc<CounterStyle>>>>,
}

impl GeneratedContent {
    /// Drops everything kept for an element's pseudo-elements, once its identity is retired.
    pub(crate) fn forget(&mut self, element: StyleNodeID) {
        // NB: One lookup per owner, not a walk over every pseudo-element of the document: a removed
        //     subtree retires its identities one at a time.
        for owner in CounterOwner::every_owner_of(element) {
            self.content_counter_styles_in_use.remove(&owner);
        }
    }
}

// https://drafts.csswg.org/css-lists-3/#text-markers
/// What a list marker whose `content` is `normal` shows, resolved from the marker's own
/// `list-style-image` and the list item's `list-style-type`. The build and the comparison after a
/// style change both answer through this, so they cannot disagree about what the marker shows.
fn marker_content(
    arena: &LayoutNodeArena,
    tree_scope: u32,
    marker_style: ComputedValuesView<'_>,
    list_item_style: ComputedValuesView<'_>,
) -> MarkerContent {
    if marker_style.list_style_image_is_set() {
        return MarkerContent::Image;
    }
    let list_style_type = list_item_style.inherited_list().list_style_type.data();
    if let Some(StyleValueData::String { .. }) = list_style_type {
        return MarkerContent::String;
    }
    MarkerContent::CounterStyle(
        arena
            .with_counter_style_registry(|registry| resolve_counter_style_value(registry, tree_scope, list_style_type)),
    )
}

/// What the list marker box `marker` shows, from its own record and that of the list item box
/// `list_box`.
fn resolve_marker_content(
    arena: &LayoutNodeArena,
    marker: NodeSlotId,
    list_box: NodeSlotId,
    tree_scope: u32,
) -> MarkerContent {
    let style_of_row = |row: NodeSlotId| {
        arena
            .style_payloads(row)
            .map(|payloads| ComputedValuesView::new(&payloads.groups))
            .expect("a list marker and its list item box both carry a style")
    };
    let list_item_style = style_of_row(list_box);
    let content = marker_content(arena, tree_scope, style_of_row(marker), list_item_style);
    // A list item whose list-style-type is `none` generates a marker box only to show its image.
    assert!(
        matches!(content, MarkerContent::Image)
            || !matches!(list_item_style.inherited_list().list_style_type.data(), Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::NONE),
        "a list marker box was built for list-style-type: none without a list-style-image"
    );
    content
}

/// The counter style a normal marker renders from, as the list its box records for a later style
/// change to compare against. A marker showing an image or a string renders from none.
fn marker_counter_styles_in_use(content: MarkerContent) -> Vec<Option<Rc<CounterStyle>>> {
    match content {
        MarkerContent::CounterStyle(counter_style) => vec![counter_style],
        MarkerContent::Image | MarkerContent::String => Vec::new(),
    }
}

/// The counter styles a `content` value names, in the order they appear in the content list and
/// then in its alt text; `None` for a name no scope in the chain registers, which reads as
/// `decimal`.
fn content_counter_styles(
    arena: &LayoutNodeArena,
    content: Option<&StyleValueData>,
    tree_scope: u32,
) -> Vec<Option<Rc<CounterStyle>>> {
    let Some(StyleValueData::Content {
        content: content_list,
        alt_text,
    }) = content
    else {
        return Vec::new();
    };
    let mut styles = Vec::new();
    arena.with_counter_style_registry(|registry| {
        for list in [content_list.optional_data(), alt_text.optional_data()] {
            for item in value_list(list) {
                if let Some(StyleValueData::Counter { counter_style, .. }) = item.optional_data() {
                    styles.push(resolve_counter_style_value(
                        registry,
                        tree_scope,
                        counter_style.optional_data(),
                    ));
                }
            }
        }
    });
    styles
}

fn value_list(value: Option<&StyleValueData>) -> &[crate::css::style_value::RetainedStyleValueData] {
    match value {
        Some(StyleValueData::ValueList { values, .. }) => values.as_slice(),
        _ => &[],
    }
}

fn content_is_normal(content: Option<&StyleValueData>) -> bool {
    matches!(content, Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::NORMAL)
}

/// Records the counter styles the box just built for the pseudo-element `owner` renders from. A list
/// marker box whose `content` is `normal` renders its marker string instead, from the list item box's
/// `list-style-type`.
pub(crate) fn note_content_counter_styles_in_use(
    arena: &LayoutNodeArena,
    owner: CounterOwner,
    marker_and_list_box: Option<(NodeSlotId, NodeSlotId)>,
) {
    let styles = arena.with_style_store(|engine| {
        let style = style_of(arena, engine, owner).expect("a pseudo-element with a box has a style");
        let content = style.content_value();
        let tree_scope = engine.counter_style_tree_scope(owner.element);
        match marker_and_list_box {
            Some((marker, list_box)) if content_is_normal(content) => {
                marker_counter_styles_in_use(resolve_marker_content(arena, marker, list_box, tree_scope))
            }
            _ => content_counter_styles(arena, content, tree_scope),
        }
    });
    arena
        .generated_content()
        .borrow_mut()
        .content_counter_styles_in_use
        .insert(owner, styles);
}

/// Whether the counter styles the pseudo-element's record names now differ from the ones its box
/// was built with. False while no box of the pseudo-element has recorded any.
pub(crate) fn content_counter_styles_changed(arena: &LayoutNodeArena, owner: CounterOwner) -> bool {
    let generated_content = arena.generated_content().borrow();
    let Some(in_use) = generated_content.content_counter_styles_in_use.get(&owner) else {
        return false;
    };
    let styles = arena.with_style_store(|engine| {
        let tree_scope = engine.counter_style_tree_scope(owner.element);
        let style = style_of(arena, engine, owner)?;
        let content = style.content_value();
        // A list marker whose `content` is `normal` shows its marker string, and the marker
        // string is what the build recorded for it. Re-deriving from `content` would answer
        // about a value that names no counter at all, so the two sets could never agree and
        // every recomputation of the marker's style would read as a counter-style change.
        // Answer the same question the build did instead.
        if owner.generated_for == GENERATED_FOR_MARKER && content_is_normal(content) {
            let list_item_style = style_of(arena, engine, CounterOwner::element(owner.element))?;
            return Some(marker_counter_styles_in_use(marker_content(
                arena,
                tree_scope,
                style,
                list_item_style,
            )));
        }
        Some(content_counter_styles(arena, content, tree_scope))
    });
    styles.is_some_and(|styles| *in_use != styles)
}
