/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_tokenizer::TokenizerInput;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::query_parser::{
    FfiMediaEnvironment, FfiQueryHandle, MediaEnvironment, VisitQuerySerialization, invalid_media_query,
    media_query_handle, parse_media_query_list, resolve_query_feature,
};
use std::cell::RefCell;
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;

#[derive(Clone, Default)]
pub struct MediaListData {
    pub(crate) queries: Vec<Arc<FfiQueryHandle>>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<MediaListData>();
};

struct MediaListState {
    data: Arc<MediaListData>,
    matches: Vec<bool>,
}

#[derive(Clone)]
pub struct MediaList {
    owner: Rc<RefCell<MediaListState>>,
}

impl MediaList {
    pub(crate) fn share(&self) -> Self {
        Self::new(self.owner.borrow().data.clone())
    }

    pub(crate) fn set_text(&self, source: TokenizerInput<'_>) {
        let data = Arc::new(parse(source));
        *self.owner.borrow_mut() = MediaListState {
            matches: vec![false; data.queries.len()],
            data,
        };
    }

    pub(crate) fn matches(&self) -> bool {
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
    MediaListData {
        queries: parse_media_query_list(source, &resolve_query_feature)
            .unwrap_or_else(|| vec![invalid_media_query()])
            .into_iter()
            .map(media_query_handle)
            .collect(),
    }
}

fn parse_medium(source: TokenizerInput<'_>) -> Option<Arc<FfiQueryHandle>> {
    let mut data = parse(source);
    match data.queries.len() {
        0 => Some(media_query_handle(invalid_media_query())),
        1 => data.queries.pop(),
        _ => None,
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_media_list_create() -> *mut MediaList {
    Box::into_raw(Box::new(MediaList::new(Arc::default())))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_retain(list: &MediaList) -> *mut MediaList {
    Box::into_raw(Box::new(list.clone()))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_share(list: &MediaList) -> *mut MediaList {
    Box::into_raw(Box::new(list.share()))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_free(list: *mut MediaList) {
    if !list.is_null() {
        drop(unsafe { Box::from_raw(list) });
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_length(list: &MediaList) -> usize {
    list.owner.borrow().data.queries.len()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_set_text(list: &MediaList, source: FfiUtf16View) {
    list.set_text(unsafe { source.units() }.unwrap());
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_append(list: &MediaList, source: FfiUtf16View) -> bool {
    // https://drafts.csswg.org/cssom/#dom-medialist-appendmedium
    // 1. Let m be the result of parsing the given value.
    // 2. If m is null, then return.
    let Some(query) = parse_medium(unsafe { source.units() }.unwrap()) else {
        return false;
    };
    // 3. If comparing m with any of the media queries in the collection of media queries returns true, then return.
    let text = query.media_text();
    let mut state = list.owner.borrow_mut();
    if state.data.queries.iter().any(|existing| existing.media_text() == text) {
        return false;
    }
    // 4. Append m to the collection of media queries.
    Arc::make_mut(&mut state.data).queries.push(query);
    state.matches.push(false);
    true
}

#[repr(u8)]
pub enum MediaListDeleteResult {
    Invalid,
    Removed,
    NotFound,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_delete(list: &MediaList, source: FfiUtf16View) -> MediaListDeleteResult {
    // https://drafts.csswg.org/cssom/#dom-medialist-deletemedium
    // 1. Let m be the result of parsing the given value.
    // 2. If m is null, then return.
    let Some(query) = parse_medium(unsafe { source.units() }.unwrap()) else {
        return MediaListDeleteResult::Invalid;
    };
    // 3. Remove any media query from the collection of media queries for which comparing the media query with m
    //    returns true. If nothing was removed, then throw a NotFoundError exception.
    let text = query.media_text();
    let mut state = list.owner.borrow_mut();
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

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_serialize(
    list: &MediaList,
    context: *mut c_void,
    visit: VisitQuerySerialization,
) {
    let state = list.owner.borrow();
    let mut text = Vec::new();
    for (index, query) in state.data.queries.iter().enumerate() {
        if index != 0 {
            text.extend([u16::from(b','), u16::from(b' ')]);
        }
        text.extend(query.media_text());
    }
    unsafe { visit(context, text.as_ptr(), text.len()) };
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_item(
    list: &MediaList,
    index: usize,
    context: *mut c_void,
    visit: VisitQuerySerialization,
) -> bool {
    let state = list.owner.borrow();
    let Some(query) = state.data.queries.get(index) else {
        return false;
    };
    let text = query.media_text();
    unsafe { visit(context, text.as_ptr(), text.len()) };
    true
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_matches(list: &MediaList) -> bool {
    list.matches()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_item_matches(list: &MediaList, index: usize) -> bool {
    list.owner.borrow().matches[index]
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_media_list_evaluate(list: &MediaList, environment: FfiMediaEnvironment) -> bool {
    list.evaluate(unsafe { environment.borrow() })
}
