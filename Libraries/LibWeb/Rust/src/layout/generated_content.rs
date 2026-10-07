/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the layout tree build puts inside a pseudo-element's box: its `content` resolved against the
//! counters sets and the quote depth, or the marker string of a list marker whose `content` is
//! `normal`.

use super::counters::{CounterName, CounterOwner, LIST_ITEM_COUNTER_NAME, style_of};
use super::layout_node_arena::LayoutNodeArena;
use super::node_data::{GENERATED_FOR_MARKER, NodeSlotId};
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::counter_representation::{
    CounterStyle, decimal, generate_a_counter_representation, resolve_counter_style_value,
};
use crate::css::css_enums::keyword;
use crate::css::css_string::CssString;
use crate::css::style::fast_hash::FastMap as HashMap;
use crate::css::style::tree::StyleNodeID;
use crate::css::style_value::{COUNTER_FUNCTION_COUNTER, StyleValueData};
use std::sync::Arc;

/// What a list marker whose `content` is `normal` shows, resolved from the published records of the
/// marker box and of the list item box it belongs to.
enum MarkerContent<'a> {
    Image,
    String(&'a [u16]),
    /// A counter style, or `None` for a name that resolves to none.
    CounterStyle(Option<Arc<CounterStyle>>),
}

/// What the tree build recorded for the generated content of each pseudo-element it built a box for.
#[derive(Clone, Default)]
pub(crate) struct GeneratedContent {
    /// The text each pseudo-element's content resolved to, the way accessibility reads it.
    accessible_texts: HashMap<CounterOwner, Vec<u16>>,
    /// The counter styles the box built for a pseudo-element was rendered from, one per
    /// `counter()` or `counters()` the content names and one for the counter style a normal
    /// marker shows. A later style change compares what the pseudo-element's record names now
    /// against this to decide whether the box has to be rebuilt.
    content_counter_styles_in_use: HashMap<CounterOwner, Vec<Option<Arc<CounterStyle>>>>,
}

impl GeneratedContent {
    /// The text the content of `owner` last resolved to, the way accessibility reads it: the alt text
    /// when there is one, otherwise every string in order.
    pub(crate) fn accessible_text(&self, owner: CounterOwner) -> &[u16] {
        self.accessible_texts.get(&owner).map_or(&[], Vec::as_slice)
    }

    /// Drops everything kept for an element's pseudo-elements, once its identity is retired.
    pub(crate) fn forget(&mut self, element: StyleNodeID) {
        // NB: One lookup per owner, not a walk over every pseudo-element of the document: a removed
        //     subtree retires its identities one at a time.
        for owner in CounterOwner::every_owner_of(element) {
            self.accessible_texts.remove(&owner);
            self.content_counter_styles_in_use.remove(&owner);
        }
    }
}

/// One thing to put in a pseudo-element's box, in order.
pub(crate) enum ContentItem {
    Text(Vec<u16>),
    /// The `<image>` at this index of the `content` list.
    Image(usize),
    /// The marker box's `list-style-image`.
    ListStyleImage,
}

pub(crate) struct ResolvedContent {
    pub(crate) items: Vec<ContentItem>,
    /// Whether the content is a list of items, as opposed to `normal` or `none`.
    pub(crate) is_list: bool,
    pub(crate) final_quote_nesting_level: u32,
    /// Whether the content shows the value of the `list-item` counter, which the document tracks to
    /// know when renumbering a list changed what is rendered.
    pub(crate) renders_list_item_counter_value: bool,
}

fn representation(
    arena: &LayoutNodeArena,
    tree_scope: u32,
    counter_style: Option<&CounterStyle>,
    value: i32,
) -> Vec<u16> {
    arena.with_counter_style_registry(|registry| {
        generate_a_counter_representation(registry, tree_scope, counter_style, value)
    })
}

// https://drafts.csswg.org/css-lists-3/#text-markers
/// What a list marker whose `content` is `normal` shows, resolved from the marker's own
/// `list-style-image` and the list item's `list-style-type`. The build and the comparison after a
/// style change both answer through this, so they cannot disagree about what the marker shows.
fn marker_content<'a>(
    arena: &LayoutNodeArena,
    tree_scope: u32,
    marker_style: ComputedValuesView<'a>,
    list_item_style: ComputedValuesView<'a>,
) -> MarkerContent<'a> {
    if marker_style.list_style_image_is_set() {
        return MarkerContent::Image;
    }
    let list_style_type = list_item_style.inherited_list().list_style_type.data();
    if let Some(StyleValueData::String { string, .. }) = list_style_type {
        return MarkerContent::String(string.units());
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
) -> MarkerContent<'_> {
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

/// The one item a list marker whose `content` is `normal` shows.
pub(crate) struct MarkerItem {
    pub(crate) item: ContentItem,
    /// Whether the item shows the value of the `list-item` counter.
    pub(crate) renders_list_item_counter_value: bool,
}

// https://drafts.csswg.org/css-lists-3/#text-markers
// "<counter-style>: Specifies the element's marker string as the value of the list-item counter
// represented using the specified <counter-style>. Specifically, the marker string is the result of
// generating a counter representation of the list-item counter value using the specified
// <counter-style>, prefixed by the prefix of the <counter-style>, and followed by the suffix of the
// <counter-style>. If the specified <counter-style> does not exist, decimal is assumed.
// <string>: The element's marker string is the specified <string>."
fn resolve_normal_marker_content(
    arena: &LayoutNodeArena,
    element: CounterOwner,
    tree_scope: u32,
    content: MarkerContent<'_>,
) -> MarkerItem {
    let list_item_counter_value = || {
        arena
            .counters_sets()
            .borrow_mut()
            .counter_value_for_use(element, &CounterName::Units(&LIST_ITEM_COUNTER_NAME))
    };
    let (item, renders_list_item_counter_value) = match content {
        MarkerContent::Image => (ContentItem::ListStyleImage, false),
        MarkerContent::String(string) => {
            // NB: The value is used even though the marker shows none of it, as using it instantiates
            //     the counter when it is missing.
            list_item_counter_value();
            // A string literal marker is the same for every item, regardless of the counter value.
            (ContentItem::Text(string.to_vec()), false)
        }
        MarkerContent::CounterStyle(counter_style) => {
            let counter_style = counter_style.as_deref().unwrap_or(decimal());
            let counter_value = list_item_counter_value();
            let counter_representation = representation(arena, tree_scope, Some(counter_style), counter_value);
            let text = [&*counter_style.prefix, &counter_representation, &*counter_style.suffix].concat();
            (ContentItem::Text(text), !counter_style.representation_is_constant())
        }
    };
    MarkerItem {
        item,
        renders_list_item_counter_value,
    }
}

/// The counter style a normal marker renders from, as the list its box records for a later style
/// change to compare against. A marker showing an image or a string renders from none.
fn marker_counter_styles_in_use(content: &MarkerContent<'_>) -> Vec<Option<Arc<CounterStyle>>> {
    match content {
        MarkerContent::CounterStyle(counter_style) => vec![counter_style.clone()],
        MarkerContent::Image | MarkerContent::String(_) => Vec::new(),
    }
}

/// The marker string of the list marker a list-item pseudo-element `element` nests.
pub(crate) fn resolve_nested_marker_content(
    arena: &LayoutNodeArena,
    element: CounterOwner,
    marker: NodeSlotId,
    list_box: NodeSlotId,
) -> MarkerItem {
    let tree_scope = arena.with_style_store(|engine| engine.counter_style_tree_scope(element.element));
    resolve_normal_marker_content(
        arena,
        element,
        tree_scope,
        resolve_marker_content(arena, marker, list_box, tree_scope),
    )
}

enum QuotesData<'a> {
    None,
    Auto,
    Specified(&'a [crate::css::style_value::RetainedStyleValueData]),
}

fn quotes_data(value: Option<&StyleValueData>) -> QuotesData<'_> {
    match value {
        Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::NONE => QuotesData::None,
        Some(StyleValueData::ValueList { values, .. }) => {
            let values = values.as_slice();
            assert!(values.len().is_multiple_of(2));
            QuotesData::Specified(values)
        }
        _ => QuotesData::Auto,
    }
}

fn quote_string<'a>(quotes: &QuotesData<'a>, open: bool, depth: u32) -> &'a [u16] {
    match quotes {
        QuotesData::None => &[],
        // FIXME: "A typographically appropriate used value for quotes is automatically chosen by the UA
        //        based on the content language of the element and/or its parent."
        QuotesData::Auto => match (open, depth) {
            (true, 0) => &[0x201C],
            (true, _) => &[0x2018],
            (false, 0) => &[0x201D],
            (false, _) => &[0x2019],
        },
        QuotesData::Specified(values) => {
            // If the depth is greater than the number of pairs, the last pair is repeated.
            let level = (depth as usize).min(values.len() / 2 - 1);
            let quote = &values[level * 2 + usize::from(!open)];
            match quote.optional_data() {
                Some(StyleValueData::String { string, .. }) => string.units(),
                _ => panic!("a quote is a string"),
            }
        }
    }
}

struct CounterItemResolver<'a> {
    arena: &'a LayoutNodeArena,
    element: CounterOwner,
    tree_scope: u32,
    styles: &'a [Option<Arc<CounterStyle>>],
    next_counter_style: usize,
    renders_list_item_counter_value: bool,
}

impl CounterItemResolver<'_> {
    // counter( <counter-name>, <counter-style>? )
    // counters( <counter-name>, <string>, <counter-style>? )
    fn resolve(&mut self, function: u8, counter_name: &CssString, join_string: &CssString) -> Vec<u16> {
        if counter_name.units() == LIST_ITEM_COUNTER_NAME {
            self.renders_list_item_counter_value = true;
        }
        let counter_style = self.styles[self.next_counter_style].as_deref();
        self.next_counter_style += 1;

        // "If no counter named <counter-name> exists on an element where counter() or counters() is used,
        // one is first instantiated with a starting value of 0."
        let name = CounterName::Css(counter_name);

        // "Represents the value of the innermost counter in the element’s CSS counters set named <counter-name>
        // using the counter style named <counter-style>."
        if function == COUNTER_FUNCTION_COUNTER {
            let value = self
                .arena
                .counters_sets()
                .borrow_mut()
                .counter_value_for_use(self.element, &name);
            return representation(self.arena, self.tree_scope, counter_style, value);
        }

        // "Represents the values of all the counters in the element’s CSS counters set named <counter-name>
        // using the counter style named <counter-style>, sorted in outermost-first to innermost-last order
        // and joined by the specified <string>."
        let values = self
            .arena
            .counters_sets()
            .borrow_mut()
            .counter_values_for_use(self.element, &name);
        let mut result = Vec::new();
        for value in values {
            let counter_string = representation(self.arena, self.tree_scope, counter_style, value);
            if !result.is_empty() {
                result.extend_from_slice(join_string.units());
            }
            result.extend_from_slice(&counter_string);
        }
        result
    }
}

/// The counter styles a `content` value names, in the order they appear in the content list and
/// then in its alt text; `None` for a name no scope in the chain registers, which reads as
/// `decimal`.
fn content_counter_styles(
    arena: &LayoutNodeArena,
    content: Option<&StyleValueData>,
    tree_scope: u32,
) -> Vec<Option<Arc<CounterStyle>>> {
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

/// Records the counter styles the box just built for `owner` renders from.
fn note_content_counter_styles_in_use(
    arena: &LayoutNodeArena,
    owner: CounterOwner,
    styles: Vec<Option<Arc<CounterStyle>>>,
) {
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
            return Some(marker_counter_styles_in_use(&marker_content(
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

/// Resolves the content of the pseudo-element `element` names, whose box the host has just built. A
/// list marker box whose `content` is `normal` shows its marker string instead.
pub(crate) fn resolve_content(
    arena: &LayoutNodeArena,
    element: CounterOwner,
    marker_and_list_box: Option<(NodeSlotId, NodeSlotId)>,
    initial_quote_nesting_level: u32,
) -> ResolvedContent {
    arena.with_style_store(|engine| {
        let style = style_of(arena, engine, element).expect("a pseudo-element with a box has a style");
        let content = style.content_value();
        let tree_scope = engine.counter_style_tree_scope(element.element);

        if let Some((marker, list_box)) = marker_and_list_box
            && content_is_normal(content)
        {
            let content = resolve_marker_content(arena, marker, list_box, tree_scope);
            note_content_counter_styles_in_use(arena, element, marker_counter_styles_in_use(&content));
            let marker = resolve_normal_marker_content(arena, element, tree_scope, content);
            return ResolvedContent {
                items: vec![marker.item],
                is_list: true,
                final_quote_nesting_level: initial_quote_nesting_level,
                renders_list_item_counter_value: marker.renders_list_item_counter_value,
            };
        }

        let styles = content_counter_styles(arena, content, tree_scope);
        let Some(StyleValueData::Content {
            content: content_list,
            alt_text,
        }) = content
        else {
            note_content_counter_styles_in_use(arena, element, styles);
            arena.generated_content().borrow_mut().accessible_texts.remove(&element);
            return ResolvedContent {
                items: Vec::new(),
                is_list: false,
                final_quote_nesting_level: initial_quote_nesting_level,
                renders_list_item_counter_value: false,
            };
        };

        let mut counters = CounterItemResolver {
            arena,
            element,
            tree_scope,
            styles: &styles,
            next_counter_style: 0,
            renders_list_item_counter_value: false,
        };
        let quotes = quotes_data(style.quotes_value());
        let mut quote_nesting_level = initial_quote_nesting_level;

        let mut items = Vec::new();
        let mut pending_text: Option<Vec<u16>> = None;
        let flush_pending_text = |items: &mut Vec<ContentItem>, pending_text: &mut Option<Vec<u16>>| {
            if let Some(text) = pending_text.take() {
                items.push(ContentItem::Text(text));
            }
        };

        for (index, item) in value_list(content_list.optional_data()).iter().enumerate() {
            match item.optional_data() {
                Some(StyleValueData::String { string, .. }) => {
                    pending_text.get_or_insert_default().extend_from_slice(string.units());
                }
                Some(StyleValueData::Keyword { keyword }) => match *keyword {
                    keyword::OPEN_QUOTE => {
                        let quote = quote_string(&quotes, true, quote_nesting_level);
                        quote_nesting_level += 1;
                        pending_text.get_or_insert_default().extend_from_slice(quote);
                    }
                    keyword::CLOSE_QUOTE => {
                        // A 'close-quote' or 'no-close-quote' that would make the depth negative is in error and is ignored
                        // (at rendering time): the depth stays at 0 and no quote mark is rendered (although the rest of the
                        // 'content' property's value is still inserted).
                        // - https://www.w3.org/TR/CSS21/generate.html#quotes-insert
                        // (This is missing from the CONTENT-3 spec.)
                        if quote_nesting_level > 0 {
                            quote_nesting_level -= 1;
                            let quote = quote_string(&quotes, false, quote_nesting_level);
                            pending_text.get_or_insert_default().extend_from_slice(quote);
                        }
                    }
                    keyword::NO_OPEN_QUOTE => quote_nesting_level += 1,
                    keyword::NO_CLOSE_QUOTE => {
                        // NOTE: See CloseQuote
                        quote_nesting_level = quote_nesting_level.saturating_sub(1);
                    }
                    _ => {}
                },
                Some(StyleValueData::Counter {
                    function,
                    counter_name,
                    join_string,
                    ..
                }) => {
                    flush_pending_text(&mut items, &mut pending_text);
                    items.push(ContentItem::Text(counters.resolve(
                        *function,
                        counter_name,
                        join_string,
                    )));
                }
                // https://drafts.csswg.org/css-content-3/#typedef-content-list
                // https://drafts.csswg.org/css-images-4/#typedef-image
                // <content-list> accepts <image>, and image-set() is an <image>.
                Some(StyleValueData::Image { .. } | StyleValueData::ImageSet { .. }) => {
                    flush_pending_text(&mut items, &mut pending_text);
                    items.push(ContentItem::Image(index));
                }
                // TODO: Implement images, and other things.
                _ => {}
            }
        }
        flush_pending_text(&mut items, &mut pending_text);

        let mut accessible_text = Vec::new();
        if let Some(alt_text) = alt_text.optional_data() {
            for item in value_list(Some(alt_text)) {
                match item.optional_data() {
                    Some(StyleValueData::String { string, .. }) => accessible_text.extend_from_slice(string.units()),
                    Some(StyleValueData::Counter {
                        function,
                        counter_name,
                        join_string,
                        ..
                    }) => accessible_text.extend(counters.resolve(*function, counter_name, join_string)),
                    _ => {}
                }
            }
        } else {
            for item in &items {
                if let ContentItem::Text(text) = item {
                    accessible_text.extend_from_slice(text);
                }
            }
        }
        arena
            .generated_content()
            .borrow_mut()
            .accessible_texts
            .insert(element, accessible_text);
        let renders_list_item_counter_value = counters.renders_list_item_counter_value;
        note_content_counter_styles_in_use(arena, element, styles);

        ResolvedContent {
            items,
            is_list: true,
            final_quote_nesting_level: quote_nesting_level,
            renders_list_item_counter_value,
        }
    })
}
