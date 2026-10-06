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
use crate::runtime::map::{ConstIterator, Map};
use crate::runtime::map_prototype::MapPrototype;
use crate::runtime::object::{
    MayInterfereWithIndexedPropertyAccess, ORDINARY_OBJECT_METHODS, PropertyKind, define_object_class,
};
use crate::runtime::realm::Realm;

/// Returns true if iterating a Map with iterator_method cannot be observed: iterator_method is this realm's original
/// Map.prototype.entries, and %MapIteratorPrototype%.next is still the original data property. Walking the Map's
/// storage with a live map::ConstIterator then produces the same entries as the iterator protocol.
pub fn map_iteration_is_unobservable(vm: &Vm, realm: Gc<Realm>, iterator_method: Gc<FunctionObject>) -> bool {
    let map_prototype = realm
        .intrinsics()
        .map_prototype(vm)
        .downcast::<MapPrototype>()
        .expect("%Map.prototype% is a MapPrototype");
    if iterator_method != map_prototype.entries_function() {
        return false;
    }

    // NB: Inspect the intrinsic prototype's own property without invoking it. An accessor or a replacement method
    //     is observable, so it makes the caller take the generic path.
    let next_method = get_own_property_without_side_effects(
        &realm.intrinsics().map_iterator_prototype(),
        &vm.names.next,
        vm.static_property_lookup_cache(StaticPropertyLookupCacheSite::MapIterationIsUnobservableNextMethod),
    );
    next_method.is_function() && {
        let next_function = next_method.as_function();
        next_function.as_native_function().is_some()
            && next_function.realm() == Some(realm)
            && next_function.is_map_prototype_next_builtin()
    }
}

#[repr(C)]
#[derive(Trace)]
pub struct MapIterator {
    base: Object,
    map: Gc<Map>,
    done: Cell<bool>,
    #[gc(untraced)]
    iteration_kind: PropertyKind,
    iterator: ConstIterator,
}

define_object_class!(MapIterator, extends: [Object], methods: {
    as_builtin_iterator_if_next_is_not_redefined: MapIterator::as_builtin_iterator_if_next_is_not_redefined,
    ..ORDINARY_OBJECT_METHODS
});

impl MapIterator {
    pub fn create(vm: &Vm, realm: Gc<Realm>, map: Gc<Map>, iteration_kind: PropertyKind) -> Gc<MapIterator> {
        realm.create_object(
            vm,
            MapIterator {
                base: Object::new_with_prototype(
                    vm,
                    Self::CLASS,
                    realm.intrinsics().map_iterator_prototype(),
                    MayInterfereWithIndexedPropertyAccess::No,
                ),
                map,
                done: Cell::new(false),
                iteration_kind,
                iterator: map.begin(),
            },
        )
    }

    fn as_builtin_iterator_if_next_is_not_redefined(_: &Object, next_method: Value) -> Option<BuiltinIteratorNext> {
        // NB: Only functions are native functions, so this only needs to look at functions.
        if next_method.is_function() {
            let next_function = next_method.as_function();
            if next_function.as_native_function().is_some() && next_function.is_map_prototype_next_builtin() {
                return Some(MapIterator::builtin_next);
            }
        }
        None
    }

    fn builtin_next(object: &Object, vm: &Vm, done: &mut bool, value: &mut Value) -> ThrowCompletionOr<()> {
        object
            .as_gc()
            .downcast::<MapIterator>()
            .expect("the builtin step of a MapIterator steps a MapIterator")
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

        let entry = self.iterator.current();
        self.iterator.advance();
        if self.iteration_kind == PropertyKind::Key {
            *value = entry.key;
            return Ok(());
        }
        if self.iteration_kind == PropertyKind::Value {
            *value = entry.value;
            return Ok(());
        }

        let realm = vm.current_realm().expect("an iterator steps in a realm");
        *value = Value::from_object(Array::create_from(vm, realm, &[entry.key, entry.value]));
        Ok(())
    }
}
