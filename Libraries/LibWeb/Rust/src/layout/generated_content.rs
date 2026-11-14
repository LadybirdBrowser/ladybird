/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! What the layout tree build puts inside a pseudo-element's box: its `content` resolved against the
//! counters sets and the quote depth, or the marker string of a list marker whose `content` is
//! `normal`.

use super::counters::{CounterName, CounterOwner, CountersSets, LIST_ITEM_COUNTER_NAME};
use super::layout_node_arena::LayoutNodeArena;
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::counter_representation::{
    CounterStyle, decimal, generate_a_counter_representation, resolve_counter_style_value,
};
use crate::css::css_enums::keyword;
use crate::css::style::tree::StyleNodeID;
use crate::css::style_value::{COUNTER_FUNCTION_COUNTER, StyleValueData};
use crate::css::style_value::{ContentPayload, CounterPayload};
use std::sync::Arc;

/// What a list marker whose `content` is `normal` shows, resolved from the published records of the
/// marker box and of the list item box it belongs to.
pub(crate) enum MarkerContent<'a> {
    Image,
    String(&'a [u16]),
    /// A counter style, or `None` for a name that resolves to none.
    CounterStyle(Option<Arc<CounterStyle>>),
}

/// What the tree build recorded for the generated content of each pseudo-element it built a box for.
#[derive(Clone, Default)]
pub(crate) struct GeneratedContent {
    /// The text each pseudo-element's content resolved to, the way accessibility reads it.
    accessible_texts: crate::cow_map::CowMap<CounterOwner, Vec<u16>>,
    /// The counter styles the box built for a pseudo-element was rendered from, one per
    /// `counter()` or `counters()` the content names and one for the counter style a normal
    /// marker shows. A later style change compares what the pseudo-element's record names now
    /// against this to decide whether the box has to be rebuilt.
    content_counter_styles_in_use: crate::cow_map::CowMap<CounterOwner, Vec<Option<Arc<CounterStyle>>>>,
}

impl GeneratedContent {
    /// The text the content of `owner` last resolved to, the way accessibility reads it: the alt text
    /// when there is one, otherwise every string in order.
    pub(crate) fn accessible_text(&self, owner: CounterOwner) -> &[u16] {
        self.accessible_texts.get(&owner).map_or(&[], Vec::as_slice)
    }

    /// Whether the content `owner` last resolved to shows the value of a counter, which a counter changing before it
    /// changes: a `counter()` or `counters()` of its content, or the counter style its marker shows, that represents
    /// some values differently.
    pub(crate) fn shows_counter_value(&self, owner: CounterOwner) -> bool {
        self.content_counter_styles_in_use.get(&owner).is_some_and(|styles| {
            styles
                .iter()
                .any(|style| style.as_deref().is_none_or(|style| !style.representation_is_constant()))
        })
    }

    /// Records what the content of `owner`, which a build just made the box of, resolved to: the counter styles its box
    /// renders from, and its accessible text.
    pub(crate) fn note_content(&mut self, owner: CounterOwner, record: ContentRecord) {
        let ContentRecord {
            counter_styles,
            accessible_text,
        } = record;
        match accessible_text {
            AccessibleText::Unchanged => {}
            AccessibleText::Remove => {
                self.accessible_texts.remove(&owner);
            }
            AccessibleText::Set(text) => {
                self.accessible_texts.insert(owner, text);
            }
            AccessibleText::Pending => unreachable!("a content is recorded once its accessible text is known"),
        }
        self.content_counter_styles_in_use.insert(owner, counter_styles);
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

/// What the generated content of a pseudo-element records for later builds and for accessibility, see
/// [`GeneratedContent`].
#[derive(Clone)]
pub(crate) struct ContentRecord {
    pub(crate) counter_styles: Vec<Option<Arc<CounterStyle>>>,
    pub(crate) accessible_text: AccessibleText,
}

/// The text of a pseudo-element's content the way accessibility reads it.
#[derive(Clone)]
pub(crate) enum AccessibleText {
    /// A list marker whose `content` is `normal` leaves what is recorded as it was.
    Unchanged,
    /// A content that is no list has none.
    Remove,
    Set(Vec<u16>),
    /// The text shows counter values or quotes not known yet.
    Pending,
}

/// One thing to put in a pseudo-element's box, in order.
pub(crate) enum ContentItem {
    /// A text, `None` until the counter values or the quote depth it shows are known. A text of `close-quote`s alone
    /// that closes no quote is not `made`: it puts nothing in the box.
    Text { text: Option<Vec<u16>>, made: bool },
    /// The `<image>` at this index of the `content` list.
    Image(usize),
}

/// What a pseudo-element's `content` resolved to, see [`resolve_content`].
pub(crate) struct ResolvedContent {
    pub(crate) items: Vec<ContentItem>,
    /// Whether the content is a list of items, as opposed to `normal` or `none`.
    pub(crate) is_list: bool,
    pub(crate) counter_styles: Vec<Option<Arc<CounterStyle>>>,
    pub(crate) accessible_text: AccessibleText,
    /// Whether the content shows the value of the `list-item` counter, which the document tracks to
    /// know when renumbering a list changed what is rendered.
    pub(crate) renders_list_item_counter_value: bool,
}

/// Where the values of the counters a pseudo-element's content shows come from: the counters set of the
/// pseudo-element.
pub(crate) trait CounterValues {
    /// The value of the innermost counter named `name` in the set, which instantiates one at 0 where the set holds
    /// none.
    fn innermost(&mut self, name: &[u16]) -> i32;
    /// The values of every counter named `name` in the set, outermost first, which instantiates one at 0 where the set
    /// holds none.
    fn every(&mut self, name: &[u16]) -> Vec<i32>;
}

/// The counters sets the arena stores, which a pseudo-element whose counters a build resolves after its commit reads.
pub(crate) struct StoredCounterValues<'a> {
    pub(crate) arena: &'a LayoutNodeArena,
    pub(crate) sets: &'a mut CountersSets,
    pub(crate) owner: CounterOwner,
}

impl CounterValues for StoredCounterValues<'_> {
    fn innermost(&mut self, name: &[u16]) -> i32 {
        self.sets
            .counter_value_for_use(self.arena, self.owner, &CounterName::Units(name))
    }

    fn every(&mut self, name: &[u16]) -> Vec<i32> {
        self.sets
            .counter_values_for_use(self.arena, self.owner, &CounterName::Units(name))
    }
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
pub(crate) fn marker_content<'a>(
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

// https://drafts.csswg.org/css-lists-3/#text-markers
// "<counter-style>: Specifies the element's marker string as the value of the list-item counter
// represented using the specified <counter-style>. Specifically, the marker string is the result of
// generating a counter representation of the list-item counter value using the specified
// <counter-style>, prefixed by the prefix of the <counter-style>, and followed by the suffix of the
// <counter-style>. If the specified <counter-style> does not exist, decimal is assumed.
// <string>: The element's marker string is the specified <string>."
/// The marker string of a list marker whose `content` is `normal` and which shows the value `value` of the `list-item`
/// counter in `counter_style`, or in decimal for none.
pub(crate) fn marker_counter_text(
    arena: &LayoutNodeArena,
    tree_scope: u32,
    counter_style: Option<&CounterStyle>,
    value: i32,
) -> Vec<u16> {
    let counter_style = counter_style.unwrap_or(decimal());
    let counter_representation = representation(arena, tree_scope, Some(counter_style), value);
    [&*counter_style.prefix, &counter_representation, &*counter_style.suffix].concat()
}

/// The counter style a normal marker renders from, as the list its box records for a later style
/// change to compare against. A marker showing an image or a string renders from none.
pub(crate) fn marker_counter_styles_in_use(content: &MarkerContent<'_>) -> Vec<Option<Arc<CounterStyle>>> {
    match content {
        MarkerContent::CounterStyle(counter_style) => vec![counter_style.clone()],
        MarkerContent::Image | MarkerContent::String(_) => Vec::new(),
    }
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

// counter( <counter-name>, <counter-style>? )
// counters( <counter-name>, <string>, <counter-style>? )
fn counter_text(
    arena: &LayoutNodeArena,
    tree_scope: u32,
    values: &mut dyn CounterValues,
    counter_style: Option<&CounterStyle>,
    counter_payload: &CounterPayload,
) -> Vec<u16> {
    let CounterPayload {
        function,
        counter_name,
        join_string,
        ..
    } = counter_payload;
    // "If no counter named <counter-name> exists on an element where counter() or counters() is used,
    // one is first instantiated with a starting value of 0."
    // "Represents the value of the innermost counter in the element’s CSS counters set named <counter-name>
    // using the counter style named <counter-style>."
    if *function == COUNTER_FUNCTION_COUNTER {
        let value = values.innermost(counter_name.units());
        return representation(arena, tree_scope, counter_style, value);
    }

    // "Represents the values of all the counters in the element’s CSS counters set named <counter-name>
    // using the counter style named <counter-style>, sorted in outermost-first to innermost-last order
    // and joined by the specified <string>."
    let mut result = Vec::new();
    for value in values.every(counter_name.units()) {
        let counter_string = representation(arena, tree_scope, counter_style, value);
        if !result.is_empty() {
            result.extend_from_slice(join_string.units());
        }
        result.extend_from_slice(&counter_string);
    }
    result
}

/// The counter styles a `content` value names, in the order they appear in the content list and
/// then in its alt text; `None` for a name no scope in the chain registers, which reads as
/// `decimal`.
fn content_counter_styles(
    arena: &LayoutNodeArena,
    content: Option<&StyleValueData>,
    tree_scope: u32,
) -> Vec<Option<Arc<CounterStyle>>> {
    let Some(StyleValueData::Content(content_payload)) = content else {
        return Vec::new();
    };
    let ContentPayload {
        content: content_list,
        alt_text,
    } = &**content_payload;
    let mut styles = Vec::new();
    arena.with_counter_style_registry(|registry| {
        for list in [content_list.optional_data(), alt_text.optional_data()] {
            for item in value_list(list) {
                if let Some(StyleValueData::Counter(counter_payload)) = item.optional_data() {
                    let CounterPayload { counter_style, .. } = &**counter_payload;
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

/// Calls `visit` with the name of every counter a `counter()` or `counters()` of the content `style` gives a
/// pseudo-element uses, in its content list and then in its alt text, as resolving the content uses them. A list marker
/// whose content is `normal` uses the `list-item` counter instead, unless it shows an image.
pub(crate) fn for_each_counter_name_content_uses(
    style: ComputedValuesView<'_>,
    is_list_item_marker: bool,
    mut visit: impl FnMut(&[u16]),
) {
    let content = style.content_value();
    if is_list_item_marker && content_is_normal(content) {
        if !style.list_style_image_is_set() {
            visit(&LIST_ITEM_COUNTER_NAME);
        }
        return;
    }
    let Some(StyleValueData::Content(content_payload)) = content else {
        return;
    };
    let ContentPayload {
        content: content_list,
        alt_text,
    } = &**content_payload;
    for list in [content_list.optional_data(), alt_text.optional_data()] {
        for item in value_list(list) {
            if let Some(StyleValueData::Counter(counter_payload)) = item.optional_data() {
                visit(counter_payload.counter_name.units());
            }
        }
    }
}

/// Whether the content of a pseudo-element whose style is `style` makes no text but white space, as far as can be told
/// before counters and quotes are resolved: its items are strings of white space, and keywords and items that make no
/// text. A counter, a quote and an image make content that is not white space.
pub(crate) fn content_is_white_space(style: ComputedValuesView<'_>) -> bool {
    let Some(StyleValueData::Content(content_payload)) = style.content_value() else {
        return false;
    };
    value_list(content_payload.content.optional_data())
        .iter()
        .all(|item| match item.optional_data() {
            Some(StyleValueData::String { string, .. }) => {
                string.units().iter().all(|unit| matches!(unit, 0x09..=0x0d | 0x20))
            }
            Some(StyleValueData::Keyword { keyword }) => {
                !matches!(*keyword, keyword::OPEN_QUOTE | keyword::CLOSE_QUOTE)
            }
            Some(StyleValueData::Counter(_) | StyleValueData::Image { .. } | StyleValueData::ImageSet { .. }) => false,
            _ => true,
        })
}

fn value_list(value: Option<&StyleValueData>) -> &[crate::css::style_value::RetainedStyleValueData] {
    match value {
        Some(StyleValueData::ValueList { values, .. }) => values.as_slice(),
        _ => &[],
    }
}

pub(crate) fn content_is_normal(content: Option<&StyleValueData>) -> bool {
    matches!(content, Some(StyleValueData::Keyword { keyword }) if *keyword == keyword::NORMAL)
}

/// The text of a run of strings and quotes of a `content` list, one item of the content, as far as the walk over the list
/// got: the quotes need the depth to be spelled.
struct TextRun {
    text: Vec<u16>,
    made: bool,
    shows_quote_depth: bool,
}

/// Resolves the `content` of a pseudo-element of `element`, whose style is `style`: the items it puts in the
/// pseudo-element's box, and what is recorded of it. With the quote depth before the content and the values of the
/// counters of the pseudo-element, every text is known; without them, the items are those it makes either way, but the
/// texts that show the depth or a value, which are `None`. A list marker whose `content` is `normal` shows its marker
/// string instead, which the caller resolves (see [`marker_content`]).
pub(crate) fn resolve_content(
    arena: &LayoutNodeArena,
    tree_scope: u32,
    style: ComputedValuesView<'_>,
    quote_depth: Option<u32>,
    mut values: Option<&mut dyn CounterValues>,
) -> ResolvedContent {
    let content = style.content_value();
    let counter_styles = content_counter_styles(arena, content, tree_scope);
    let Some(StyleValueData::Content(content_payload)) = content else {
        return ResolvedContent {
            items: Vec::new(),
            is_list: false,
            counter_styles,
            accessible_text: AccessibleText::Remove,
            renders_list_item_counter_value: false,
        };
    };
    let ContentPayload {
        content: content_list,
        alt_text,
    } = &**content_payload;
    let quotes = quotes_data(style.quotes_value());
    let mut quote_nesting_level = quote_depth;
    let mut next_counter_style = 0;
    let mut renders_list_item_counter_value = false;

    let mut items = Vec::new();
    let mut pending_text: Option<TextRun> = None;
    let flush_pending_text = |items: &mut Vec<ContentItem>, pending_text: &mut Option<TextRun>| {
        if let Some(run) = pending_text.take() {
            items.push(ContentItem::Text {
                made: run.made || run.shows_quote_depth && quote_depth.is_none(),
                text: (!run.shows_quote_depth || quote_depth.is_some()).then_some(run.text),
            });
        }
    };
    let new_run = || TextRun {
        text: Vec::new(),
        made: false,
        shows_quote_depth: false,
    };
    let mut resolve_counter = |counter_payload: &CounterPayload, values: &mut Option<&mut dyn CounterValues>| {
        if counter_payload.counter_name.units() == LIST_ITEM_COUNTER_NAME {
            renders_list_item_counter_value = true;
        }
        let counter_style = counter_styles[next_counter_style].as_deref();
        next_counter_style += 1;
        values
            .as_deref_mut()
            .map(|values| counter_text(arena, tree_scope, values, counter_style, counter_payload))
    };

    for (index, item) in value_list(content_list.optional_data()).iter().enumerate() {
        match item.optional_data() {
            Some(StyleValueData::String { string, .. }) => {
                let run = pending_text.get_or_insert_with(new_run);
                run.made = true;
                run.text.extend_from_slice(string.units());
            }
            Some(StyleValueData::Keyword { keyword }) => match *keyword {
                keyword::OPEN_QUOTE => {
                    let run = pending_text.get_or_insert_with(new_run);
                    run.made = true;
                    run.shows_quote_depth = true;
                    if let Some(level) = &mut quote_nesting_level {
                        run.text.extend_from_slice(quote_string(&quotes, true, *level));
                        *level += 1;
                    }
                }
                keyword::CLOSE_QUOTE => {
                    // A 'close-quote' or 'no-close-quote' that would make the depth negative is in error and is ignored
                    // (at rendering time): the depth stays at 0 and no quote mark is rendered (although the rest of the
                    // 'content' property's value is still inserted).
                    // - https://www.w3.org/TR/CSS21/generate.html#quotes-insert
                    // (This is missing from the CONTENT-3 spec.)
                    // NB: Until the depth is known, the close-quote may make the item.
                    match &mut quote_nesting_level {
                        Some(level) if *level > 0 => {
                            *level -= 1;
                            let run = pending_text.get_or_insert_with(new_run);
                            run.made = true;
                            run.shows_quote_depth = true;
                            run.text.extend_from_slice(quote_string(&quotes, false, *level));
                        }
                        // NB: A text of such close-quotes alone makes no item, but is one whatever the depth.
                        Some(_) | None => pending_text.get_or_insert_with(new_run).shows_quote_depth = true,
                    }
                }
                keyword::NO_OPEN_QUOTE => {
                    if let Some(level) = &mut quote_nesting_level {
                        *level += 1;
                    }
                }
                keyword::NO_CLOSE_QUOTE => {
                    // NOTE: See CloseQuote
                    if let Some(level) = &mut quote_nesting_level {
                        *level = level.saturating_sub(1);
                    }
                }
                _ => {}
            },
            Some(StyleValueData::Counter(counter_payload)) => {
                flush_pending_text(&mut items, &mut pending_text);
                let text = resolve_counter(counter_payload, &mut values);
                items.push(ContentItem::Text { text, made: true });
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
    let mut accessible_text_is_known = true;
    if let Some(alt_text) = alt_text.optional_data() {
        for item in value_list(Some(alt_text)) {
            match item.optional_data() {
                Some(StyleValueData::String { string, .. }) => accessible_text.extend_from_slice(string.units()),
                Some(StyleValueData::Counter(counter_payload)) => match resolve_counter(counter_payload, &mut values) {
                    Some(text) => accessible_text.extend(text),
                    None => accessible_text_is_known = false,
                },
                _ => {}
            }
        }
    } else {
        for item in &items {
            match item {
                ContentItem::Text {
                    text: Some(text),
                    made: true,
                } => accessible_text.extend_from_slice(text),
                ContentItem::Text { text: None, .. } => accessible_text_is_known = false,
                _ => {}
            }
        }
    }

    ResolvedContent {
        items,
        is_list: true,
        counter_styles,
        accessible_text: if accessible_text_is_known {
            AccessibleText::Set(accessible_text)
        } else {
            AccessibleText::Pending
        },
        renders_list_item_counter_value,
    }
}
