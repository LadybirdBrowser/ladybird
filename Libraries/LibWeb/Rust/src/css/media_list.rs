/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16StringUnits;

use crate::css::css_tokenizer::TokenizerInput;
use crate::css::ffi_support::{FfiUtf16View, OwnedBox};
use crate::css::parser::query_parser::{
    FfiMediaEnvironment, FfiQueryHandle, MediaEnvironment, MediaQuery, VisitQuerySerialization, invalid_media_query,
    media_query_handle, parse_media_query_list, resolve_query_feature,
};
use crate::css::serialize::TextSink;
use crate::css::thin_slice::ThinSlice;
use std::cell::RefCell;
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;

/// A media list's queries, held by one pointer with their count before them.
#[derive(Clone, Default)]
pub struct MediaListData {
    pub(crate) queries: ThinSlice<Arc<FfiQueryHandle>>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<MediaListData>();
};

impl MediaListData {
    pub(crate) fn from_queries(queries: Vec<MediaQuery>) -> Self {
        Self {
            queries: queries.into_iter().map(media_query_handle).collect(),
        }
    }
}

struct MediaListState {
    data: Arc<MediaListData>,
    matches: Vec<bool>,
}

pub enum MediaListDeleteResult {
    Invalid,
    Removed,
    NotFound,
}

#[derive(Clone)]
pub struct MediaList {
    owner: Rc<RefCell<MediaListState>>,
}

impl MediaList {
    pub(crate) fn data(&self) -> Arc<MediaListData> {
        self.owner.borrow().data.clone()
    }

    /// A list of its own with the media queries of this one, as they are now.
    pub fn share(&self) -> Self {
        Self::new(self.owner.borrow().data.clone())
    }

    /// The number of media queries in the collection of media queries.
    pub fn length(&self) -> usize {
        self.owner.borrow().data.queries.len()
    }

    /// The serialization of the collection of media queries.
    pub fn media_text(&self) -> ak::Utf16String {
        let state = self.owner.borrow();
        let mut text = TextSink::new();
        for (index, query) in state.data.queries.iter().enumerate() {
            if index != 0 {
                text.push_ascii(", ");
            }
            query
                .media_text()
                .into_iter()
                .for_each(|unit| text.push_code_unit(unit));
        }
        text.into_utf16_string()
    }

    /// The serialization of the media query at the index of the collection of media queries, if it has one there.
    pub fn item(&self, index: usize) -> Option<ak::Utf16String> {
        self.owner
            .borrow()
            .data
            .queries
            .get(index)
            .map(|query| ak::Utf16String::from_utf16(&query.media_text()))
    }

    /// Replaces the collection of media queries with the result of parsing the text.
    pub fn set_media_text(&self, text: Utf16StringUnits<'_>) {
        self.set_text(text.into());
    }

    /// Whether the medium was added.
    pub fn append_medium(&self, medium: Utf16StringUnits<'_>) -> bool {
        // https://drafts.csswg.org/cssom/#dom-medialist-appendmedium
        // 1. Let m be the result of parsing the given value.
        // 2. If m is null, then return.
        let Some(query) = parse_medium(medium.into()) else {
            return false;
        };
        // 3. If comparing m with any of the media queries in the collection of media queries returns true, then return.
        let text = query.media_text();
        let mut state = self.owner.borrow_mut();
        if state.data.queries.iter().any(|existing| existing.media_text() == text) {
            return false;
        }
        // 4. Append m to the collection of media queries.
        Arc::make_mut(&mut state.data).queries.push(query);
        state.matches.push(false);
        true
    }

    pub fn delete_medium(&self, medium: Utf16StringUnits<'_>) -> MediaListDeleteResult {
        // https://drafts.csswg.org/cssom/#dom-medialist-deletemedium
        // 1. Let m be the result of parsing the given value.
        // 2. If m is null, then return.
        let Some(query) = parse_medium(medium.into()) else {
            return MediaListDeleteResult::Invalid;
        };
        // 3. Remove any media query from the collection of media queries for which comparing the media query with m
        //    returns true. If nothing was removed, then throw a NotFoundError exception.
        let text = query.media_text();
        let mut state = self.owner.borrow_mut();
        let mut removed = false;
        for index in (0..state.data.queries.len()).rev() {
            if state.data.queries[index].media_text() == text {
                Arc::make_mut(&mut state.data).queries.remove(index);
                state.matches.remove(index);
                removed = true;
            }
        }
        if removed {
            MediaListDeleteResult::Removed
        } else {
            MediaListDeleteResult::NotFound
        }
    }

    pub(crate) fn set_text(&self, source: TokenizerInput<'_>) {
        let data = Arc::new(parse(source));
        *self.owner.borrow_mut() = MediaListState {
            matches: vec![false; data.queries.len()],
            data,
        };
    }

    pub fn matches(&self) -> bool {
        let state = self.owner.borrow();
        state.matches.is_empty() || state.matches.iter().any(|matches| *matches)
    }

    pub(crate) fn evaluate(&self, environment: MediaEnvironment<'_>) -> bool {
        let mut state = self.owner.borrow_mut();
        for index in 0..state.data.queries.len() {
            state.matches[index] = state.data.queries[index].matches_media(environment);
        }
        state.matches.is_empty() || state.matches.iter().any(|matches| *matches)
    }

    pub(crate) fn set_matches(&self, matches: &[bool]) {
        self.owner.borrow_mut().matches.copy_from_slice(matches);
    }

    pub(crate) fn new(data: Arc<MediaListData>) -> Self {
        Self {
            owner: Rc::new(RefCell::new(MediaListState {
                matches: vec![false; data.queries.len()],
                data,
            })),
        }
    }
}

fn parse(source: TokenizerInput<'_>) -> MediaListData {
    MediaListData::from_queries(
        parse_media_query_list(source, &resolve_query_feature).unwrap_or_else(|| vec![invalid_media_query()]),
    )
}

fn parse_medium(source: TokenizerInput<'_>) -> Option<Arc<FfiQueryHandle>> {
    let mut data = parse(source);
    match data.queries.len() {
        0 => Some(media_query_handle(invalid_media_query())),
        1 => Some(data.queries.remove(0)),
        _ => None,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_media_list_create() -> *mut MediaList {
    Box::into_raw(Box::new(MediaList::new(Arc::default())))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_media_list_retain(list: &MediaList) -> *mut MediaList {
    Box::into_raw(Box::new(list.clone()))
}

#[cfg(test)]
pub(crate) fn rust_media_list_share(list: &MediaList) -> *mut MediaList {
    Box::into_raw(Box::new(list.share()))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_media_list_free(list: OwnedBox<MediaList>) {
    drop(list);
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_media_list_length(list: &MediaList) -> usize {
    list.length()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_media_list_set_text(list: &MediaList, source: FfiUtf16View) {
    list.set_text(source.units().unwrap());
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_serialize(
    list: &MediaList,
    context: *mut c_void,
    visit: VisitQuerySerialization,
) {
    let text = list.media_text();
    let units = text.to_utf16();
    unsafe { visit(context, units.as_ptr(), units.len()) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_item(
    list: &MediaList,
    index: usize,
    context: *mut c_void,
    visit: VisitQuerySerialization,
) -> bool {
    let Some(text) = list.item(index) else {
        return false;
    };
    let units = text.to_utf16();
    unsafe { visit(context, units.as_ptr(), units.len()) };
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_media_list_matches(list: &MediaList) -> bool {
    list.matches()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_media_list_item_matches(list: &MediaList, index: usize) -> bool {
    list.owner.borrow().matches[index]
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_media_list_evaluate(list: &MediaList, environment: FfiMediaEnvironment<'_>) -> bool {
    list.evaluate(environment.borrow())
}
