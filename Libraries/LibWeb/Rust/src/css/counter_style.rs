/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::{keyword_from_ascii_case_insensitive, keyword_to_counter_style_name_keyword};
use crate::css::css_string::CssString;
use crate::css::descriptor_block::{DescriptorBlockData, FfiDescriptorBlock, rust_descriptor_block_retain};
use crate::css::ffi_support::{FfiUtf16View, ascii_lowercase};
use std::cell::RefCell;
use std::sync::Arc;

#[derive(Clone)]
pub struct CounterStyleData {
    pub(crate) name: CssString,
    pub(crate) descriptors: Arc<DescriptorBlockData>,
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<CounterStyleData>();
};

pub struct FfiCounterStyle {
    name: RefCell<CssString>,
    descriptors: FfiDescriptorBlock,
}

impl FfiCounterStyle {
    pub(crate) fn name(&self) -> CssString {
        self.name.borrow().clone()
    }

    pub(crate) fn descriptors(&self) -> Arc<DescriptorBlockData> {
        self.descriptors.data()
    }

    pub(crate) fn new(data: Arc<CounterStyleData>) -> Self {
        Self {
            name: RefCell::new(data.name.clone()),
            descriptors: FfiDescriptorBlock::new(data.descriptors.clone()),
        }
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_style_descriptors(rule: &FfiCounterStyle) -> *mut FfiDescriptorBlock {
    rust_descriptor_block_retain(&rule.descriptors)
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_style_name(rule: &FfiCounterStyle) -> FfiUtf16View {
    let name = rule.name.borrow();
    let name = name.units();
    FfiUtf16View {
        ascii: std::ptr::null(),
        utf16: name.as_ptr(),
        length: name.len(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_counter_style_set_name(rule: &FfiCounterStyle, name: FfiUtf16View) -> bool {
    let Some(mut name) = (unsafe { name.to_utf16() }) else {
        return false;
    };
    // https://drafts.csswg.org/css-counter-styles-3/#dom-csscounterstylerule-name
    // 1. If the value is an ASCII case-insensitive match for "none" or one of the non-overridable counter-style names, do nothing and return.
    let keyword = keyword_from_ascii_case_insensitive(&name);
    if keyword == Some(crate::css::css_enums::keyword::NONE)
        || [
            b"decimal".as_slice(),
            b"disc",
            b"square",
            b"circle",
            b"disclosure-open",
            b"disclosure-closed",
        ]
        .iter()
        .any(|reserved| {
            name.iter()
                .copied()
                .map(ascii_lowercase)
                .eq(reserved.iter().copied().map(u16::from))
        })
    {
        return false;
    }
    // 2. If the value is an ASCII case-insensitive match for any of the predefined counter styles, lowercase it.
    if keyword.and_then(keyword_to_counter_style_name_keyword).is_some() {
        name.iter_mut().for_each(|unit| *unit = ascii_lowercase(*unit));
    }
    // 3. Replace the associated rule’s name with an identifier equal to the value.
    *rule.name.borrow_mut() = CssString::from_utf16(&name);
    true
}
