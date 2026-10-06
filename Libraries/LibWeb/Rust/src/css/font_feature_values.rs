/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_string::CssString;
use crate::css::ffi_support::FfiUtf16View;
use crate::css::parser::value_parser::equals_ascii_case_insensitive;
use crate::css::style_value::StyleValueData;
use std::cell::RefCell;
use std::collections::HashMap;
use std::ffi::c_void;
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

/// The OpenType features a style asks of a font, by tag, the later setting of a tag winning.
#[derive(Default)]
struct ShapeFeatures(Vec<(u32, u32)>);

impl ShapeFeatures {
    fn set(&mut self, tag: &[u8; 4], value: u32) {
        let tag = u32::from_be_bytes(*tag);
        match self.0.iter_mut().find(|(existing, _)| *existing == tag) {
            Some(feature) => feature.1 = value,
            None => self.0.push((tag, value)),
        }
    }
}

/// The keywords a computed `font-variant-*` tuple holds, in its slot order: absent ones are `None`.
fn tuple_keywords(value: Option<&StyleValueData>) -> impl Iterator<Item = Option<u16>> + '_ {
    let slots = match value {
        Some(StyleValueData::Tuple { values }) => values.as_slice(),
        _ => &[],
    };
    slots.iter().map(|slot| match slot.optional_data() {
        Some(StyleValueData::Keyword { keyword }) => Some(*keyword),
        _ => None,
    })
}

fn keyword_of(value: Option<&StyleValueData>) -> Option<u16> {
    match value {
        Some(StyleValueData::Keyword { keyword }) => Some(*keyword),
        _ => None,
    }
}

fn integer_of(value: &StyleValueData) -> Option<i32> {
    match value {
        StyleValueData::Integer { value } => Some(*value),
        calculated @ StyleValueData::Calculated { .. } => {
            crate::css::calc::resolve_calculated_integer_without_context(calculated)
        }
        _ => None,
    }
}

/// The OpenType features the computed values of a font resolution request ask for, by
/// `FontResolutionFeatureInput` and null for a property with its initial value. The names in
/// `font-variant-alternates` are looked up in a family's `@font-feature-values` through `lookup`,
/// when `family_values` is not null. Each feature goes to `append` as a big-endian tag and a value.
/// https://drafts.csswg.org/css-fonts-4/#feature-variation-precedence
///
/// # Safety
///
/// `values` must address `FONT_RESOLUTION_FEATURE_INPUT_COUNT` null or live style values, and the
/// callbacks must accept `family_values` and `features`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_shape_features(
    values: *const *const c_void,
    family_values: *const c_void,
    lookup: unsafe extern "C" fn(*const c_void, FontFeatureValuesRuleKind, FfiUtf16View, &mut usize) -> *const u32,
    features: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, u32, u32),
) {
    use crate::css::css_enums::keyword::*;
    use crate::css::style::bridge::{FONT_RESOLUTION_FEATURE_INPUT_COUNT, FontResolutionFeatureInput as Input};
    // SAFETY: Guaranteed by the caller.
    let values = unsafe { std::slice::from_raw_parts(values, FONT_RESOLUTION_FEATURE_INPUT_COUNT) };
    let value = |input: Input| unsafe { values[input as usize].cast::<StyleValueData>().as_ref() };
    let mut shape = ShapeFeatures::default();

    // 10. Font features implied by the value of the font-variant property, the related font-variant
    //     subproperties and any other CSS property that uses OpenType features (e.g. the
    //     font-kerning property) are applied.
    // https://drafts.csswg.org/css-fonts/#font-variant-ligatures-prop
    let optimize_speed = keyword_of(value(Input::TextRendering)) == Some(OPTIMIZESPEED);
    let ligatures = value(Input::FontVariantLigatures).filter(|ligatures| keyword_of(Some(ligatures)) != Some(NORMAL));
    if keyword_of(ligatures) == Some(NONE) || (ligatures.is_none() && optimize_speed) {
        // AD-HOC: text-rendering: optimizeSpeed disables the ligatures normal enables.
        for tag in [b"liga", b"clig", b"dlig", b"hlig", b"calt"] {
            shape.set(tag, 0);
        }
    } else if ligatures.is_none() {
        // A value of normal specifies that common default features are enabled.
        shape.set(b"liga", 1);
        shape.set(b"clig", 1);
    }
    for keyword in tuple_keywords(ligatures).flatten() {
        let (tags, value): (&[&[u8; 4]], u32) = match keyword {
            COMMON_LIGATURES => (&[b"liga", b"clig"], 1),
            NO_COMMON_LIGATURES => (&[b"liga", b"clig"], 0),
            DISCRETIONARY_LIGATURES => (&[b"dlig"], 1),
            NO_DISCRETIONARY_LIGATURES => (&[b"dlig"], 0),
            HISTORICAL_LIGATURES => (&[b"hlig"], 1),
            NO_HISTORICAL_LIGATURES => (&[b"hlig"], 0),
            CONTEXTUAL => (&[b"calt"], 1),
            NO_CONTEXTUAL => (&[b"calt"], 0),
            _ => continue,
        };
        tags.iter().for_each(|tag| shape.set(tag, value));
    }

    // https://drafts.csswg.org/css-fonts/#font-variant-position-prop
    // https://drafts.csswg.org/css-fonts/#font-variant-caps-prop
    // https://drafts.csswg.org/css-fonts/#font-variant-numeric-prop
    // https://drafts.csswg.org/css-fonts/#font-variant-east-asian-prop
    let keywords = [Input::FontVariantPosition, Input::FontVariantCaps]
        .into_iter()
        .map(|input| keyword_of(value(input)))
        .chain(tuple_keywords(value(Input::FontVariantNumeric)))
        .chain(tuple_keywords(value(Input::FontVariantEastAsian)));
    for keyword in keywords.flatten() {
        let tags: &[&[u8; 4]] = match keyword {
            SUB => &[b"subs"],
            SUPER => &[b"sups"],
            SMALL_CAPS => &[b"smcp"],
            ALL_SMALL_CAPS => &[b"c2sc", b"smcp"],
            PETITE_CAPS => &[b"pcap"],
            ALL_PETITE_CAPS => &[b"c2pc", b"pcap"],
            UNICASE => &[b"unic"],
            TITLING_CAPS => &[b"titl"],
            OLDSTYLE_NUMS => &[b"onum"],
            LINING_NUMS => &[b"lnum"],
            PROPORTIONAL_NUMS => &[b"pnum"],
            TABULAR_NUMS => &[b"tnum"],
            DIAGONAL_FRACTIONS => &[b"frac"],
            STACKED_FRACTIONS => &[b"afrc"],
            ORDINAL => &[b"ordn"],
            SLASHED_ZERO => &[b"zero"],
            JIS78 => &[b"jp78"],
            JIS83 => &[b"jp83"],
            JIS90 => &[b"jp90"],
            JIS04 => &[b"jp04"],
            SIMPLIFIED => &[b"smpl"],
            TRADITIONAL => &[b"trad"],
            FULL_WIDTH => &[b"fwid"],
            PROPORTIONAL_WIDTH => &[b"pwid"],
            RUBY => &[b"ruby"],
            _ => continue,
        };
        tags.iter().for_each(|tag| shape.set(tag, 1));
    }

    // https://drafts.csswg.org/css-fonts/#font-variant-alternates-prop
    // FIXME: These values never apply to generic font families
    let alternates = match value(Input::FontVariantAlternates) {
        Some(StyleValueData::ValueList { values, .. }) => values.as_slice(),
        _ => &[],
    };
    for alternate in alternates.iter().map(|alternate| alternate.data()) {
        let StyleValueData::Function { name, value } = alternate else {
            if keyword_of(Some(alternate)) == Some(HISTORICAL_FORMS) {
                shape.set(b"hist", 1);
            }
            continue;
        };
        let Some(kind) = [
            ("stylistic", FontFeatureValuesRuleKind::Stylistic),
            ("styleset", FontFeatureValuesRuleKind::Styleset),
            ("character-variant", FontFeatureValuesRuleKind::CharacterVariant),
            ("swash", FontFeatureValuesRuleKind::Swash),
            ("ornaments", FontFeatureValuesRuleKind::Ornaments),
            ("annotation", FontFeatureValuesRuleKind::Annotation),
        ]
        .into_iter()
        .find_map(|(function, kind)| equals_ascii_case_insensitive(name.units(), function.as_bytes()).then_some(kind)) else {
            continue;
        };
        let names = match value.data() {
            StyleValueData::ValueList { values, .. } => values.as_slice(),
            _ => &[],
        };
        for feature_name in names {
            let feature_name = match feature_name.data() {
                StyleValueData::CustomIdent { custom_ident } => custom_ident.units(),
                StyleValueData::String { string, .. } => string.units(),
                _ => continue,
            };
            if family_values.is_null() {
                continue;
            }
            let mut count = 0;
            // SAFETY: The callback answers with `count` values that live through this call.
            let feature_values = unsafe { lookup(family_values, kind, string_view(feature_name), &mut count) };
            if feature_values.is_null() || count == 0 {
                continue;
            }
            let feature_values = unsafe { std::slice::from_raw_parts(feature_values, count) };
            let first = feature_values[0];
            match kind {
                // https://drafts.csswg.org/css-fonts/#multi-value-features
                // Values between 1 and 99 enable OpenType features ss01 through ss99 and cv01
                // through cv99; greater ones and 0 enable none. A second character-variant value
                // is the value passed for the feature.
                FontFeatureValuesRuleKind::Styleset => {
                    for &index in feature_values.iter().filter(|&&index| (1..=99).contains(&index)) {
                        shape.set(&numbered_tag(b"ss", index), 1);
                    }
                }
                FontFeatureValuesRuleKind::CharacterVariant if (1..=99).contains(&first) => {
                    shape.set(&numbered_tag(b"cv", first), feature_values.get(1).copied().unwrap_or(1));
                }
                FontFeatureValuesRuleKind::CharacterVariant => {}
                FontFeatureValuesRuleKind::Swash => {
                    shape.set(b"swsh", first);
                    shape.set(b"cswh", first);
                }
                FontFeatureValuesRuleKind::Stylistic => shape.set(b"salt", first),
                FontFeatureValuesRuleKind::Ornaments => shape.set(b"ornm", first),
                _ => shape.set(b"nalt", first),
            }
        }
    }

    // FIXME: vkrn should be enabled for vertical text.
    // AD-HOC: Kerning is off with text-rendering: optimizeSpeed unless font-kerning asks for it.
    let kerning = match keyword_of(value(Input::FontKerning)) {
        Some(NONE) => 0,
        Some(NORMAL) => 1,
        _ => u32::from(!optimize_speed),
    };
    shape.set(b"kern", kerning);

    // 13. Font features implied by the value of font-feature-settings property are applied.
    if let Some(StyleValueData::ValueList { values, .. }) = value(Input::FontFeatureSettings) {
        for setting in values.as_slice() {
            if let StyleValueData::OpenTypeTagged {
                packed_tag,
                value: setting,
                ..
            } = setting.data()
                && let Some(setting) = integer_of(setting.data())
            {
                // NB: A feature setting is kept as one byte.
                shape.set(&packed_tag.to_be_bytes(), u32::from(setting as u8));
            }
        }
    }

    for (tag, value) in shape.0 {
        // SAFETY: Guaranteed by the caller.
        unsafe { append(features, tag, value) };
    }
}

/// `ss01` through `ss99` and `cv01` through `cv99`.
fn numbered_tag(prefix: &[u8; 2], index: u32) -> [u8; 4] {
    [
        prefix[0],
        prefix[1],
        b'0' + (index / 10) as u8,
        b'0' + (index % 10) as u8,
    ]
}

/// The font variation axes a computed `font-variation-settings` value sets, in order, each handed
/// to `append` as a big-endian tag and a value.
///
/// # Safety
///
/// `value` must be null or point to a live style value, and `append` must accept `axes`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_font_variation_axes(
    value: *const c_void,
    axes: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, u32, f64),
) {
    // SAFETY: Guaranteed by the caller.
    let Some(StyleValueData::ValueList { values, .. }) = (unsafe { value.cast::<StyleValueData>().as_ref() }) else {
        return;
    };
    for setting in values.as_slice() {
        let StyleValueData::OpenTypeTagged { packed_tag, value, .. } = setting.data() else {
            continue;
        };
        let value = match value.data() {
            StyleValueData::Number { value } => Some(*value),
            calculated => crate::css::calc::resolve_calculated_number_without_context(calculated),
        };
        if let Some(value) = value {
            // SAFETY: Guaranteed by the caller.
            unsafe { append(axes, *packed_tag, value) };
        }
    }
}
