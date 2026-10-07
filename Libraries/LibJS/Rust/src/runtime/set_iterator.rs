/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use core::cell::Cell;

use libjs_runtime_macros::Trace;

use crate::bytecode::executable::StaticPropertyLookupCacheSite;
use crate::bytecode::property_access::get_own_property_without_side_effects;
use crate::gc::class::{GcCell, define_cell};
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::array::Array;
use crate::runtime::completion::ThrowCompletionOr;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::iterator::BuiltinIteratorNext;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, PropertyKind, define_object_class,
};
use crate::runtime::realm::Realm;
use crate::runtime::set::{ConstIterator, Set};
use crate::runtime::set_prototype::SetPrototype;

/// Returns true if iterating a Set with iterator_method cannot be observed: iterator_method is this realm's original
/// Set.prototype.values, and %SetIteratorPrototype%.next is still the original data property. Walking the Set's
/// storage with a live set::ConstIterator then produces the same values as the iterator protocol.
pub fn set_iteration_is_unobservable(vm: &Vm, realm: Gc<Realm>, iterator_method: Gc<FunctionObject>) -> bool {
    let set_prototype = realm
        .intrinsics()
        .set_prototype(vm)
        .downcast::<SetPrototype>()
        .expect("%Set.prototype% is a SetPrototype");
    if iterator_method != set_prototype.values_function() {
        return false;
    }

    // NB: Inspect the intrinsic prototype's own property without invoking it. An accessor or a replacement method
    //     is observable, so it makes the caller take the generic path.
    let next_method = get_own_property_without_side_effects(
        &realm.intrinsics().set_iterator_prototype(),
        &vm.names.next,
        vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::SetIterationIsUnobservableNextMethod),
    );
    next_method.is_function() && {
        let next_function = next_method.as_function();
        next_function.as_native_function().is_some() && next_function.is_set_prototype_next_builtin()
    }
}

#[repr(C)]
#[derive(Trace)]
pub struct SetIterator {
    base: Object,
    set: Gc<Set>,
    done: Cell<bool>,
    #[gc(untraced)]
    iteration_kind: PropertyKind,
    iterator: ConstIterator,
}

define_object_class!(SetIterator, extends: [Object], methods: {
    as_builtin_iterator_if_next_is_not_redefined: SetIterator::as_builtin_iterator_if_next_is_not_redefined,
    ..ORDINARY_OBJECT_METHODS
});

impl SetIterator {
    pub fn create(vm: &Vm, realm: Gc<Realm>, set: Gc<Set>, iteration_kind: PropertyKind) -> Gc<SetIterator> {
        realm.create_object(
            vm,
            SetIterator {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().set_iterator_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                set,
                done: Cell::new(false),
                iteration_kind,
                iterator: set.begin(),
            },
        )
    }

    fn as_builtin_iterator_if_next_is_not_redefined(_: &Object, next_method: Value) -> Option<BuiltinIteratorNext> {
        if next_method.is_function() {
            let next_function = next_method.as_function();
            if next_function.as_native_function().is_some() && next_function.is_set_prototype_next_builtin() {
                return Some(SetIterator::builtin_next);
            }
        }
        None
    }

    fn builtin_next(object: &Object, vm: &Vm, done: &mut bool, value: &mut Value) -> ThrowCompletionOr<()> {
        object
            .as_gc()
            .downcast::<SetIterator>()
            .expect("the builtin step of a SetIterator steps a SetIterator")
            .next(vm, done, value)
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

        if self.iterator.is_end() {
            self.done.set(true);
            *done = true;
            *value = Value::UNDEFINED;
            return Ok(());
        }

        assert!(self.iteration_kind != PropertyKind::Key);

        *value = self.iterator.current();
        self.iterator.advance();
        if self.iteration_kind == PropertyKind::Value {
            return Ok(());
        }

        let realm = vm.current_realm().expect("an iterator steps in a realm");
        *value = Value::from_object(Array::create_from(vm, realm, &[*value, *value]));
        Ok(())
    }
}
