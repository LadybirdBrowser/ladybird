/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use ak::Utf16FlyString;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::iterator::create_iterator_result_object;
use crate::runtime::native_function::raw_native;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, ShouldThrowExceptions, define_object_class,
};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::prototype_object::typed_this_value;
use crate::runtime::realm::Realm;
use crate::runtime::regexp_prototype::{advance_string_index, regexp_exec};
use crate::runtime::regexp_string_iterator::RegExpStringIterator;
use crate::utf16::Utf16View;

/// %RegExpStringIteratorPrototype%.
#[repr(C)]
#[derive(Trace)]
pub struct RegExpStringIteratorPrototype {
    base: Object,
}

define_object_class!(RegExpStringIteratorPrototype, extends: [Object], methods: {
    initialize: RegExpStringIteratorPrototype::initialize,
    ..ORDINARY_OBJECT_METHODS
});

impl RegExpStringIteratorPrototype {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<RegExpStringIteratorPrototype> {
        realm.create_object(
            vm,
            RegExpStringIteratorPrototype {
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
        let attributes = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &vm.names.next,
            raw_native!(RegExpStringIteratorPrototype::next),
            0,
            attributes,
            None,
        );

        // 22.2.9.2.2 %RegExpStringIteratorPrototype% [ @@toStringTag ], https://tc39.es/ecma262/#sec-%regexpstringiteratorprototype%-@@tostringtag
        object.define_direct_property(
            vm,
            &PropertyKey::from(vm.well_known_symbols().to_string_tag),
            Value::from_string(PrimitiveString::create_from_fly_string(
                vm,
                &Utf16FlyString::from_utf8("RegExp String Iterator"),
            )),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 22.2.9.2.1 %RegExpStringIteratorPrototype%.next ( ), https://tc39.es/ecma262/#sec-%regexpstringiteratorprototype%.next
    fn next(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        // 1. Let O be the this value.
        // 2. If O is not an Object, throw a TypeError exception.
        // 3. If O does not have all of the internal slots of a RegExp String Iterator Object Instance (see 22.2.9.3), throw a TypeError exception.
        let iterator = typed_this_value::<RegExpStringIterator>(vm, "RegExpStringIterator")?;

        // 4. If O.[[Done]] is true, then
        if iterator.done() {
            // a. Return CreateIteratorResultObject(undefined, true).
            return Ok(Value::from_object(create_iterator_result_object(
                vm,
                realm,
                Value::UNDEFINED,
                true,
            )));
        }

        // 5. Let R be O.[[IteratingRegExp]].
        let regexp = iterator.regexp_object();

        // 6. Let S be O.[[IteratedString]].
        let string = iterator.string();

        // 7. Let global be O.[[Global]].
        let global = iterator.global();

        // 8. Let fullUnicode be O.[[Unicode]].
        let full_unicode = iterator.unicode();

        // 9. Let match be ? RegExpExec(R, S).
        let match_ = regexp_exec(vm, &regexp, string)?;

        // 10. If match is null, then
        if match_.is_null() {
            // a. Set O.[[Done]] to true.
            iterator.set_done();

            // b. Return CreateIteratorResultObject(undefined, true).
            return Ok(Value::from_object(create_iterator_result_object(
                vm,
                realm,
                Value::UNDEFINED,
                true,
            )));
        }

        // 11. If global is false, then
        if !global {
            // a. Set O.[[Done]] to true.
            iterator.set_done();

            // b. Return CreateIteratorResultObject(match, false).
            return Ok(Value::from_object(create_iterator_result_object(
                vm, realm, match_, false,
            )));
        }

        // 12. Let matchStr be ? ToString(? Get(match, "0")).
        let match_string = match_.get(vm, &PropertyKey::from(0u32))?.to_utf16_string(vm)?;

        // 13. If matchStr is the empty String, then
        if Utf16View::of_string(&match_string).is_empty() {
            // a. Let thisIndex be ℝ(? ToLength(? Get(R, "lastIndex"))).
            let this_index = regexp.get(vm, &vm.names.lastIndex)?.to_length(vm)?;

            // b. Let nextIndex be AdvanceStringIndex(S, thisIndex, fullUnicode).
            let string_data = string.utf16_string();
            let next_index = advance_string_index(Utf16View::of_string(&string_data), this_index, full_unicode);

            // c. Perform ? Set(R, "lastIndex", 𝔽(nextIndex), true).
            regexp.set(
                vm,
                &vm.names.lastIndex,
                Value::from_f64(next_index as f64),
                ShouldThrowExceptions::Yes,
            )?;
        }

        // 14. Return CreateIteratorResultObject(match, false).
        Ok(Value::from_object(create_iterator_result_object(
            vm, realm, match_, false,
        )))
    }
}
