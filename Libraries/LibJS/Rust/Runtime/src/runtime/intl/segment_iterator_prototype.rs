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
use crate::runtime::iterator::create_iterator_result_object;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_object;
use crate::runtime::realm::Realm;
use crate::utf16::Utf16View;

/// 19.6.2 The %IntlSegmentIteratorPrototype% Object, https://tc39.es/ecma402/#sec-%intlsegmentiteratorprototype%-object
#[repr(C)]
#[derive(Trace)]
pub struct SegmentIteratorPrototype {
    base: Object,
}

define_object_class!(SegmentIteratorPrototype, extends: [Object], methods: {
    initialize: SegmentIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl SegmentIteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<SegmentIteratorPrototype> {
        realm.create_object(
            vm,
            SegmentIteratorPrototype {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().iterator_prototype(vm),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        // 19.6.2.2 %IntlSegmentIteratorPrototype% [ %Symbol.toStringTag% ], https://tc39.es/ecma402/#sec-%intlsegmentiteratorprototype%.%symbol.tostringtag%
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_utf8(vm, "Segmenter String Iterator")),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_native_function(
            vm,
            realm,
            &vm.names.next,
            raw_native!(SegmentIteratorPrototype::next),
            0,
            PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE),
            None,
        );
    }

    // 19.6.2.1 %IntlSegmentIteratorPrototype%.next ( ), https://tc39.es/ecma402/#sec-%intlsegmentiteratorprototype%.next
    fn next(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a built-in function runs in a realm");

        // 1. Let iterator be the this value.
        // 2. Perform ? RequireInternalSlot(iterator, [[IteratingSegmenter]]).
        let iterator = typed_this_object::<SegmentIterator>(vm, "SegmentIterator")?;

        // 3. Let segmenter be iterator.[[IteratingSegmenter]].
        let segmenter = iterator.iterating_segmenter();

        // 4. Let string be iterator.[[IteratedString]].
        let string = iterator.iterated_string();
        let string_view = Utf16View::of_string(&string);

        // 5. Let startIndex be iterator.[[IteratedStringNextSegmentCodeUnitIndex]].
        let start_index = iterator.iterated_string_next_segment_code_unit_index();

        // 6. Let len be the length of string.
        let length = string_view.length_in_code_units();

        // 7. If startIndex ≥ len, then
        if start_index >= length {
            // a. Return CreateIterResultObject(undefined, true).
            return Ok(Value::from_object(create_iterator_result_object(
                vm,
                realm,
                Value::UNDEFINED,
                true,
            )));
        }

        // 8. Let endIndex be FindBoundary(segmenter, string, startIndex, after).
        let end_index = find_boundary(
            segmenter
                .borrow_mut()
                .as_mut()
                .expect("a Segment Iterator has an ICU segmenter"),
            string_view,
            start_index,
            Direction::After,
        );

        // 9. Set iterator.[[IteratedStringNextSegmentCodeUnitIndex]] to endIndex.
        // NOTE: This is already handled by LibUnicode.

        // 10. Let segmentData be CreateSegmentDataObject(segmenter, string, startIndex, endIndex).
        let segment_data = create_segment_data_object(
            vm,
            segmenter,
            iterator.segments().segments_primitive_string(),
            string_view,
            start_index,
            end_index,
        )?;

        // 11. Return CreateIterResultObject(segmentData, false).
        Ok(Value::from_object(create_iterator_result_object(
            vm,
            realm,
            Value::from_object(segment_data),
            false,
        )))
    }
}
