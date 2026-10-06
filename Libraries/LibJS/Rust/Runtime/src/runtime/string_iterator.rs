/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use ak::Utf16String;
use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::iterator::BuiltinIteratorNext;
use crate::runtime::object::{MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, define_object_class};
use crate::runtime::primitive_string::PrimitiveString;
use crate::runtime::realm::Realm;
use crate::runtime::string_prototype::code_point_at;
use crate::utf16::Utf16View;

/// The iterator String.prototype[@@iterator] returns, which steps through the code points of `string`. `position` is
/// the code unit offset of the next code point.
#[repr(C)]
#[derive(Trace)]
pub struct StringIterator {
    base: Object,
    string: Utf16String,
    position: Cell<usize>,
    done: Cell<bool>,
}

define_object_class!(StringIterator, extends: [Object], methods: {
    as_builtin_iterator_if_next_is_not_redefined: StringIterator::as_builtin_iterator_if_next_is_not_redefined,
    ..ORDINARY_OBJECT_METHODS
});

impl StringIterator {
    pub fn create(vm: &Vm, realm: Gc<Realm>, string: Utf16String) -> Gc<StringIterator> {
        realm.create_object(
            vm,
            StringIterator {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().string_iterator_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                string,
                position: Cell::new(0),
                done: Cell::new(false),
            },
        )
    }

    fn as_builtin_iterator_if_next_is_not_redefined(_: &Object, next_method: Value) -> Option<BuiltinIteratorNext> {
        if next_method.is_function()
            && let Some(native_function) = next_method.as_function().as_native_function()
            && native_function.is_string_prototype_next_builtin()
        {
            return Some(|object, vm, done, value| {
                object
                    .as_gc()
                    .downcast::<StringIterator>()
                    .expect("the builtin step of a string iterator is taken on a string iterator")
                    .next(vm, done, value)
            });
        }
        None
    }

    #[allow(
        clippy::unnecessary_wraps,
        reason = "the step of a builtin iterator can throw for other iterators"
    )]
    pub fn next(&self, vm: &Vm, done: &mut bool, value: &mut Value) -> ThrowCompletionOr<()> {
        if self.done.get() {
            *done = true;
            *value = Value::UNDEFINED;
            return Ok(());
        }

        let string = Utf16View::of_string(&self.string);
        let position = self.position.get();
        if position == string.length_in_code_units() {
            self.done.set(true);
            *done = true;
            *value = Value::UNDEFINED;
            return Ok(());
        }

        let code_unit_count = code_point_at(string, position).code_unit_count;
        self.position.set(position + code_unit_count);
        let code_point = string.substring_view(position, code_unit_count).to_utf16_string();

        *value = Value::from_string(PrimitiveString::create(vm, code_point));
        Ok(())
    }
}
