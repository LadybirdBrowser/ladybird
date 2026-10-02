/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;
use core::ops::Deref;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{Finalize, GcCell, define_cell};
use crate::gc::gc_ref_cell::GcRefCell;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::{Must, ThrowCompletionOr};
use crate::runtime::intl::intl_object::{IntlObject, ResolutionOptionDescriptor};
use crate::runtime::object::MayInterfereWithIndexedPropertyAccess;
use crate::runtime::primitive_string::PrimitiveString;
use crate::unicode::intl::{self as unicode, Inclusive, SegmenterGranularity};
use crate::utf16::Utf16View;

/// 19 Segmenter Objects, https://tc39.es/ecma402/#segmenter-objects
#[repr(C)]
#[derive(Trace)]
pub struct Segmenter {
    base: Object,
    #[gc(untraced)]
    locale: GcRefCell<Utf16String>, // [[Locale]]
    #[gc(untraced)]
    segmenter_granularity: Cell<SegmenterGranularity>, // [[SegmenterGranularity]]

    // Non-standard. Stores the ICU segmenter for the Intl object's segmentation options.
    #[gc(untraced)]
    segmenter: GcRefCell<Option<unicode::Segmenter>>,
}

define_cell!(Segmenter, Object, extends: [Object], finalize: finalize);

impl Deref for Segmenter {
    type Target = Object;

    fn deref(&self) -> &Object {
        &self.base
    }
}

impl Finalize for Segmenter {
    fn finalize(&self) {
        drop(self.segmenter.replace(None));
    }
}

impl Segmenter {
    pub fn new(vm: &Vm, prototype: Gc<Object>) -> Segmenter {
        Segmenter {
            base: Object::new_with_prototype(vm, Self::CLASS, prototype, MayInterfereWithIndexedPropertyAccess::No),
            locale: GcRefCell::default(),
            segmenter_granularity: Cell::new(SegmenterGranularity::Grapheme),
            segmenter: GcRefCell::new(None),
        }
    }

    pub fn locale(&self) -> Utf16String {
        self.locale.borrow().clone()
    }

    pub fn set_locale(&self, locale: Utf16String) {
        self.locale.replace(locale);
    }

    pub fn segmenter_granularity(&self) -> SegmenterGranularity {
        self.segmenter_granularity.get()
    }

    pub fn set_segmenter_granularity(&self, segmenter_granularity: Utf16View<'_>) {
        self.segmenter_granularity
            .set(unicode::segmenter_granularity_from_string(segmenter_granularity));
    }

    pub fn segmenter_granularity_string(&self) -> &'static str {
        unicode::segmenter_granularity_to_string(self.segmenter_granularity.get())
    }

    /// A copy of the ICU segmenter, as Unicode::Segmenter::clone() makes it.
    pub fn clone_segmenter(&self) -> unicode::Segmenter {
        self.segmenter
            .borrow()
            .as_ref()
            .expect("the Intl.Segmenter constructor creates the ICU segmenter")
            .clone_segmenter()
    }

    pub fn set_segmenter(&self, segmenter: unicode::Segmenter) {
        self.segmenter.replace(Some(segmenter));
    }
}

impl IntlObject for Segmenter {
    // 19.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.segmenter-internal-slots
    fn relevant_extension_keys(&self) -> &'static [&'static str] {
        // The value of the [[RelevantExtensionKeys]] internal slot is « ».
        &[]
    }

    // 19.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.segmenter-internal-slots
    fn resolution_option_descriptors<'vm>(&self, _: &'vm Vm) -> Vec<ResolutionOptionDescriptor<'vm>> {
        // The value of the [[ResolutionOptionDescriptors]] internal slot is « ».
        Vec::new()
    }
}

// 19.7.1 CreateSegmentDataObject ( segmenter, string, startIndex, endIndex ), https://tc39.es/ecma402/#sec-createsegmentdataobject
/// `segmenter` is the ICU segmenter of the Segments object or Segment Iterator, which is only borrowed between the
/// allocations this makes.
pub fn create_segment_data_object(
    vm: &Vm,
    segmenter: &GcRefCell<Option<unicode::Segmenter>>,
    string_value: Gc<PrimitiveString>,
    string: Utf16View<'_>,
    start_index: usize,
    end_index: usize,
) -> ThrowCompletionOr<Gc<Object>> {
    let realm = vm.current_realm().expect("CreateSegmentDataObject runs in a realm");
    let names = &vm.names;

    // 1. Let len be the length of string.
    let length = string.length_in_code_units();

    // 2. Assert: startIndex ≥ 0.
    // NOTE: This is always true because the type is size_t.

    // 3. Assert: endIndex ≤ len.
    assert!(end_index <= length);

    // 4. Assert: startIndex < endIndex.
    assert!(start_index < end_index);

    // 5. Let result be OrdinaryObjectCreate(%Object.prototype%).
    let result = Object::create(vm, realm, Some(realm.object_prototype()));

    // 6. Let segment be the substring of string from startIndex to endIndex.

    // 7. Perform ! CreateDataPropertyOrThrow(result, "segment", segment).
    let segment = PrimitiveString::create_from_substring(vm, string_value, start_index, end_index - start_index);
    result
        .create_data_property_or_throw(vm, &names.segment, Value::from_string(segment))
        .must();

    // 8. Perform ! CreateDataPropertyOrThrow(result, "index", 𝔽(startIndex)).
    result
        .create_data_property_or_throw(vm, &names.index, Value::from_f64(start_index as f64))
        .must();

    // 9. Perform ! CreateDataPropertyOrThrow(result, "input", string).
    result
        .create_data_property_or_throw(vm, &names.input, Value::from_string(string_value))
        .must();

    // 10. Let granularity be segmenter.[[SegmenterGranularity]].
    let (granularity, is_current_boundary_word_like) = {
        let segmenter = segmenter.borrow();
        let segmenter = segmenter
            .as_ref()
            .expect("a Segments object and a Segment Iterator have an ICU segmenter");
        (
            segmenter.segmenter_granularity(),
            segmenter.is_current_boundary_word_like(),
        )
    };

    // 11. If granularity is "word", then
    if granularity == SegmenterGranularity::Word {
        // a. Let isWordLike be a Boolean value indicating whether the segment in string is "word-like" according to locale segmenter.[[Locale]].
        let is_word_like = is_current_boundary_word_like;

        // b. Perform ! CreateDataPropertyOrThrow(result, "isWordLike", isWordLike).
        result
            .create_data_property_or_throw(vm, &names.isWordLike, Value::from_bool(is_word_like))
            .must();
    }

    // 12. Return result.
    Ok(result)
}

/// The direction FindBoundary searches in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Direction {
    Before,
    After,
}

// 19.8.1 FindBoundary ( segmenter, string, startIndex, direction ), https://tc39.es/ecma402/#sec-findboundary
pub fn find_boundary(
    segmenter: &mut unicode::Segmenter,
    string: Utf16View<'_>,
    start_index: usize,
    direction: Direction,
) -> usize {
    // 1. Let len be the length of string.
    let length = string.length_in_code_units();

    // 2. Assert: startIndex < len.
    assert!(start_index < length);

    // 3. Let locale be segmenter.[[Locale]].
    // 4. Let granularity be segmenter.[[SegmenterGranularity]].

    // 5. If direction is before, then
    if direction == Direction::Before {
        // a. Search string for the last segmentation boundary that is preceded by at most startIndex code units from
        //    the beginning, using locale locale and text element granularity granularity.
        let boundary = segmenter.previous_boundary(start_index, Inclusive::Yes);

        // b. If a boundary is found, return the count of code units in string preceding it.
        // c. Return 0.
        return boundary.unwrap_or(0);
    }

    // 6. Assert: direction is after.
    // 7. Search string for the first segmentation boundary that follows the code unit at index startIndex, using locale
    //    locale and text element granularity granularity.
    let boundary = segmenter.next_boundary(start_index, Inclusive::No);

    // 8. If a boundary is found, return the count of code units in string preceding it.
    // 9. Return len.
    boundary.unwrap_or(length)
}
