/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::intl::segment_iterator::SegmentIterator;
use crate::runtime::intl::segmenter::{Direction, create_segment_data_object, find_boundary};
use crate::runtime::intl::segments::Segments;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::utf16::Utf16View;

/// 19.5.2 The %IntlSegmentsPrototype% Object, https://tc39.es/ecma402/#sec-%intlsegmentsprototype%-object
#[repr(C)]
#[derive(Trace)]
pub struct SegmentsPrototype {
    base: Object,
}

define_object_class!(SegmentsPrototype, extends: [Object], methods: {
    initialize: SegmentsPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

fn typed_this_segments(vm: &Vm) -> ThrowCompletionOr<Gc<Segments>> {
    typed_this_object::<Segments>(vm, "Segments")
}

impl SegmentsPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SegmentsPrototype> {
        realm.create_object(
            vm,
            SegmentsPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.object_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().iterator),
            raw_native!(SegmentsPrototype::symbol_iterator),
            0,
            attr,
            None,
        );
        object.define_native_function(
            vm,
            realm,
            &vm.names.containing,
            raw_native!(SegmentsPrototype::containing),
            1,
            attr,
            None,
        );
    }

    // 19.5.2.1 %IntlSegmentsPrototype%.containing ( index ), https://tc39.es/ecma402/#sec-%intlsegmentsprototype%.containing
    fn containing(vm: &Vm) -> ThrowCompletionOr<Value> {
        // 1. Let segments be the this value.
        // 2. Perform ? RequireInternalSlot(segments, [[SegmentsSegmenter]]).
        let segments = typed_this_segments(vm)?;

        // 3. Let segmenter be segments.[[SegmentsSegmenter]].
        let segmenter = segments.segments_segmenter();

        // 4. Let string be segments.[[SegmentsString]].
        let string = segments.segments_string();
        let string_view = Utf16View::of_string(&string);

        // 5. Let len be the length of string.
        let length = string_view.length_in_code_units();

        // 6. Let n be ? ToIntegerOrInfinity(index).
        let n = vm.argument(0).to_integer_or_infinity(vm)?;

        // 7. If n < 0 or n ≥ len, return undefined.
        if n < 0.0 || n >= length as f64 {
            return Ok(Value::UNDEFINED);
        }

        let (start_index, end_index) = {
            let mut segmenter = segmenter.borrow_mut();
            let segmenter = segmenter.as_mut().expect("a Segments object has an ICU segmenter");

            // 8. Let startIndex be FindBoundary(segmenter, string, n, before).
            let start_index = find_boundary(segmenter, string_view, n as usize, Direction::Before);

            // 9. Let endIndex be FindBoundary(segmenter, string, n, after).
            let end_index = find_boundary(segmenter, string_view, n as usize, Direction::After);

            (start_index, end_index)
        };

        // 10. Return CreateSegmentDataObject(segmenter, string, startIndex, endIndex).
        Ok(Value::from_object(create_segment_data_object(
            vm,
            segmenter,
            segments.segments_primitive_string(),
            string_view,
            start_index,
            end_index,
        )?))
    }

    // 19.5.2.2 %IntlSegmentsPrototype% [ %Symbol.iterator% ] ( ), https://tc39.es/ecma402/#sec-%intlsegmentsprototype%-%symbol.iterator%
    fn symbol_iterator(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let segments be the this value.
        // 2. Perform ? RequireInternalSlot(segments, [[SegmentsSegmenter]]).
        let segments = typed_this_segments(vm)?;

        // 3. Let segmenter be segments.[[SegmentsSegmenter]].
        let segmenter = segments.clone_segments_segmenter();

        // 4. Let string be segments.[[SegmentsString]].
        let string = segments.segments_string();

        // 5. Return ! CreateSegmentIterator(segmenter, string).
        Ok(Value::from_object(SegmentIterator::create(
            vm, realm, segmenter, string, segments,
        )))
    }
}
