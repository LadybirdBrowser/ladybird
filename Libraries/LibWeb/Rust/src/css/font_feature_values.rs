/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::ffi_support::FfiUtf16View;
use std::cell::RefCell;
use std::collections::HashMap;
use std::sync::Arc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FontFeatureValuesRuleKind {
    Annotation,
    CharacterVariant,
    HistoricalForms,
    Ornaments,
    Styleset,
    Stylistic,
    Swash,
}

#[derive(Clone)]
pub(crate) struct FeatureValueEntry {
    name: Arc<[u16]>,
    values: Vec<u32>,
}

#[derive(Clone, Default)]
pub(crate) struct FeatureValueMap {
    entries: Vec<FeatureValueEntry>,
    indices: HashMap<Arc<[u16]>, usize>,
}

#[derive(Clone, Default)]
pub struct FontFeatureValuesData {
    pub(crate) families: Vec<CssString>,
    pub(crate) maps: [FeatureValueMap; 7],
}

impl FontFeatureValuesData {
    pub(crate) fn set(&mut self, kind: FontFeatureValuesRuleKind, name: &[u16], values: Vec<u32>) {
        let map = &mut self.maps[kind as usize];
        if let Some(&index) = map.indices.get(name) {
            map.entries[index].values = values;
        } else {
            let name: Arc<[u16]> = name.into();
            map.indices.insert(name.clone(), map.entries.len());
            map.entries.push(FeatureValueEntry { name, values });
        }
    }
}

const _: () = {
    const fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<FontFeatureValuesData>();
};

// Document-local consumers retain the same mutable owner. Independent sheets
// share only the immutable parsed data and copy it on mutation.
pub struct FontFeatureValuesRule {
    data: RefCell<Arc<FontFeatureValuesData>>,
}

impl FontFeatureValuesRule {
    pub(crate) fn snapshot(&self) -> Arc<FontFeatureValuesData> {
        self.data.borrow().clone()
    }
    pub(crate) fn new(data: Arc<FontFeatureValuesData>) -> Self {
        Self {
            data: RefCell::new(data),
        }
    }
}

/// # Safety
/// Data must be null or own an Arc reference returned by a rule view.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_feature_values_data_release(data: *const FontFeatureValuesData) {
    if !data.is_null() {
        drop(unsafe { Arc::from_raw(data) });
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_font_feature_values_data_family_count(data: &FontFeatureValuesData) -> usize {
    data.families.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_font_feature_values_data_family_at(data: &FontFeatureValuesData, index: usize) -> FfiUtf16View {
    string_view(data.families[index].units())
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_font_feature_values_data_count(
    data: &FontFeatureValuesData,
    kind: FontFeatureValuesRuleKind,
) -> usize {
    data.maps[kind as usize].entries.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_font_feature_values_data_at(
    data: &FontFeatureValuesData,
    kind: FontFeatureValuesRuleKind,
    index: usize,
) -> FfiFeatureValueEntry {
    let entry = &data.maps[kind as usize].entries[index];
    FfiFeatureValueEntry {
        name: string_view(&entry.name),
        values: entry.values.as_ptr(),
        count: entry.values.len(),
    }
}

#[repr(C)]
pub struct FfiFeatureValueEntry {
    pub name: FfiUtf16View,
    pub values: *const u32,
    pub count: usize,
}

fn string_view(name: &[u16]) -> FfiUtf16View {
    FfiUtf16View {
        ascii: std::ptr::null(),
        utf16: name.as_ptr(),
        length: name.len(),
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_font_feature_values_family_count(owner: &FontFeatureValuesRule) -> usize {
    owner.data.borrow().families.len()
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_font_feature_values_family_at(owner: &FontFeatureValuesRule, index: usize) -> FfiUtf16View {
    string_view(owner.data.borrow().families[index].units())
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_feature_values_set_families(
    owner: &FontFeatureValuesRule,
    families: *const FfiUtf16View,
    count: usize,
) {
    let families = (0..count)
        .map(|index| CssString::from_utf16(&unsafe { (*families.add(index)).to_utf16() }.unwrap()))
        .collect();
    Arc::make_mut(&mut owner.data.borrow_mut()).families = families;
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_font_feature_values_count(
    owner: &FontFeatureValuesRule,
    kind: FontFeatureValuesRuleKind,
) -> usize {
    owner.data.borrow().maps[kind as usize].entries.len()
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_feature_values_find(
    owner: &FontFeatureValuesRule,
    kind: FontFeatureValuesRuleKind,
    name: FfiUtf16View,
) -> usize {
    let name = unsafe { name.to_utf16() }.unwrap();
    owner.data.borrow().maps[kind as usize]
        .indices
        .get(name.as_slice())
        .copied()
        .unwrap_or(usize::MAX)
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_font_feature_values_at(
    owner: &FontFeatureValuesRule,
    kind: FontFeatureValuesRuleKind,
    index: usize,
) -> FfiFeatureValueEntry {
    let data = owner.data.borrow();
    let entry = &data.maps[kind as usize].entries[index];
    FfiFeatureValueEntry {
        name: string_view(&entry.name),
        values: entry.values.as_ptr(),
        count: entry.values.len(),
    }
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_feature_values_set(
    owner: &FontFeatureValuesRule,
    kind: FontFeatureValuesRuleKind,
    name: FfiUtf16View,
    values: *const u32,
    count: usize,
) {
    let name = unsafe { name.to_utf16() }.unwrap();
    // Copy borrowed inputs before COW can release or replace their storage.
    let values = (0..count)
        .map(|index| unsafe { *values.add(index) })
        .collect::<Vec<_>>();
    Arc::make_mut(&mut owner.data.borrow_mut()).set(kind, &name, values);
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_feature_values_remove(
    owner: &FontFeatureValuesRule,
    kind: FontFeatureValuesRuleKind,
    name: FfiUtf16View,
) -> bool {
    let name = unsafe { name.to_utf16() }.unwrap();
    let mut data = owner.data.borrow_mut();
    let Some(&index) = data.maps[kind as usize].indices.get(name.as_slice()) else {
        return false;
    };
    let map = &mut Arc::make_mut(&mut data).maps[kind as usize];
    map.indices.remove(name.as_slice());
    map.entries.remove(index);
    for (index, entry) in map.entries.iter().enumerate().skip(index) {
        *map.indices.get_mut(&entry.name).unwrap() = index;
    }
    true
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_font_feature_values_clear(owner: &FontFeatureValuesRule, kind: FontFeatureValuesRuleKind) {
    let mut data = owner.data.borrow_mut();
    if !data.maps[kind as usize].entries.is_empty() {
        let map = &mut Arc::make_mut(&mut data).maps[kind as usize];
        map.entries.clear();
        map.indices.clear();
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_font_feature_values_external_memory_size(owner: &FontFeatureValuesRule) -> usize {
    let data = owner.data.borrow();
    let mut size = size_of::<FontFeatureValuesRule>().saturating_add(size_of::<FontFeatureValuesData>());
    size = size.saturating_add(data.families.capacity().saturating_mul(size_of::<CssString>()));
    for family in &data.families {
        size = size.saturating_add(size_of_val(family.units()));
    }
    for map in &data.maps {
        size = size.saturating_add(map.entries.capacity().saturating_mul(size_of::<FeatureValueEntry>()));
        size = size.saturating_add(map.indices.capacity().saturating_mul(size_of::<(Arc<[u16]>, usize)>()));
        for entry in &map.entries {
            size = size
                .saturating_add(size_of_val(&*entry.name))
                .saturating_add(entry.values.capacity().saturating_mul(size_of::<u32>()));
        }
    }
    size
}
