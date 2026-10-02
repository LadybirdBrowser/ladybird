/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::HashMap;

use libjs_runtime_macros::Trace;

use crate::gc::class::{GcCell, define_cell};
use crate::gc::root::MarkedVec;
use crate::interpreter::vm::Vm;
use crate::layout::cell::Gc;
use crate::layout::object::Object;
use crate::layout::value::Value;
use crate::runtime::abstract_operations::{
    KeyedGroups, call_function_object, group_by, ordinary_create_from_constructor_of,
};
use crate::runtime::array::Array;
use crate::runtime::completion::{Completion, ThrowCompletionOr};
use crate::runtime::error::ErrorKind;
use crate::runtime::error_types::ErrorType;
use crate::runtime::function_object::FunctionObject;
use crate::runtime::intrinsics::Intrinsics;
use crate::runtime::iterator::get_iterator_values;
use crate::runtime::keyed_collections::canonicalize_keyed_collection_key;
use crate::runtime::map::Map;
use crate::runtime::native_function::{NativeFunction, define_native_function_class, raw_native};
use crate::runtime::property_attributes::{Attribute, PropertyAttributes};
use crate::runtime::property_key::PropertyKey;
use crate::runtime::realm::Realm;
use crate::runtime::value::same_value;
use crate::runtime::value_traits::value_traits_hash;

#[repr(C)]
#[derive(Trace)]
pub struct MapConstructor {
    base: NativeFunction,
}

define_native_function_class!(
    MapConstructor,
    initialize: MapConstructor::initialize,
    call: MapConstructor::call,
    construct: MapConstructor::construct
);

/// The groups of GroupBy with the zero key coercion, the C++ OrderedHashMap<GC::Root<Value>, GC::RootVector<Value>,
/// KeyedGroupTraits>.
pub struct ValueKeyedGroups<'vm> {
    vm: &'vm Vm,
    keys: MarkedVec<'vm, Value>,
    elements: Vec<MarkedVec<'vm, Value>>,
    /// The groups by the ValueTraits hash of their key, which keeps the keys themselves in the marked list only.
    group_indices_by_key_hash: HashMap<u64, Vec<usize>>,
}

impl ValueKeyedGroups<'_> {
    fn find(&self, key: Value, key_hash: u64) -> Option<usize> {
        let candidates = self.group_indices_by_key_hash.get(&key_hash)?;
        candidates.iter().copied().find(|&index| {
            // AddValueToKeyedGroup uses SameValue on the keys on Step 1.a.
            same_value(self.keys.get(index).expect("the group exists"), key)
        })
    }
}

impl<'vm> KeyedGroups<'vm> for ValueKeyedGroups<'vm> {
    type Key = Value;

    fn new(vm: &'vm Vm) -> Self {
        Self {
            vm,
            keys: MarkedVec::new(vm),
            elements: Vec::new(),
            group_indices_by_key_hash: HashMap::new(),
        }
    }

    #[allow(clippy::unnecessary_wraps, reason = "the property key coercion can throw")]
    fn coerce_key(_: &Vm, key: Value) -> ThrowCompletionOr<Value> {
        // h. Else,
        //     i. Assert: keyCoercion is zero.
        //     ii. Set key to CanonicalizeKeyedCollectionKey(key).
        Ok(canonicalize_keyed_collection_key(key))
    }

    fn add_value_to_keyed_group(&mut self, key: Value, value: Value) {
        // 1. For each Record { [[Key]], [[Elements]] } g of groups, do
        //      a. If SameValue(g.[[Key]], key) is true, then
        //      NOTE: This is performed in KeyedGroupTraits::equals for groupToMap and Traits<JS::PropertyKey>::equals for group.
        let key_hash = value_traits_hash(key);
        if let Some(existing_group) = self.find(key, key_hash) {
            // i. Assert: exactly one element of groups meets this criteria.
            // NOTE: This is done on insertion into the hash map, as only `set` tells us if we overrode an entry.

            // ii. Append value as the last element of g.[[Elements]].
            self.elements[existing_group].push(value);

            // iii. Return unused.
            return;
        }

        // 2. Let group be the Record { [[Key]]: key, [[Elements]]: « value » }.
        let new_elements = MarkedVec::new(self.vm);
        new_elements.push(value);

        // 3. Append group as the last element of groups.
        self.group_indices_by_key_hash
            .entry(key_hash)
            .or_default()
            .push(self.keys.len());
        self.keys.push(key);
        self.elements.push(new_elements);

        // 4. Return unused.
    }
}

impl MapConstructor {
    pub fn create(vm: &Vm, realm: Gc<Realm>) -> Gc<MapConstructor> {
        realm.create_object(
            vm,
            MapConstructor {
                base: NativeFunction::new_with_name(
                    vm,
                    Self::CLASS,
                    vm.names.Map.as_string().clone(),
                    realm.function_prototype(),
                ),
            },
        )
    }

    fn initialize(object: &Object, vm: &Vm, realm: Gc<Realm>) {
        let names = &vm.names;

        // 24.1.2.2 Map.prototype, https://tc39.es/ecma262/#sec-map.prototype
        object.define_direct_property(
            vm,
            &names.prototype,
            Value::from_object(realm.intrinsics().map_prototype(vm)),
            PropertyAttributes::new(0),
        );

        let attr = PropertyAttributes::new(Attribute::WRITABLE | Attribute::CONFIGURABLE);
        object.define_native_function(
            vm,
            realm,
            &names.groupBy,
            raw_native!(MapConstructor::group_by),
            2,
            attr,
            None,
        );

        object.define_native_accessor(
            vm,
            realm,
            &PropertyKey::from(vm.well_known_symbols().species),
            raw_native!(MapConstructor::symbol_species_getter),
            None,
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );

        object.define_direct_property(
            vm,
            &names.length,
            Value::from_i32(0),
            PropertyAttributes::new(Attribute::CONFIGURABLE),
        );
    }

    // 24.1.1.1 Map ( [ iterable ] ), https://tc39.es/ecma262/#sec-map-iterable
    fn call(_: &NativeFunction, vm: &Vm) -> ThrowCompletionOr<Value> {
        vm.throw_completion(ErrorKind::TypeError, ErrorType::ConstructorWithoutNew, &[&"Map"])
    }

    // 24.1.1.1 Map ( [ iterable ] ), https://tc39.es/ecma262/#sec-map-iterable
    fn construct(_: &NativeFunction, vm: &Vm, new_target: Gc<FunctionObject>) -> ThrowCompletionOr<Gc<Object>> {
        let realm = vm.current_realm().expect("a constructor runs in a realm");

        let map = ordinary_create_from_constructor_of(vm, realm, new_target, Intrinsics::map_prototype, |prototype| {
            Map::new(vm, prototype)
        })?;

        if vm.argument(0).is_nullish() {
            return Ok(map.upcast());
        }

        let adder = map.get(vm, &vm.names.set)?;
        if !adder.is_function() {
            return vm.throw_completion(
                ErrorKind::TypeError,
                ErrorType::NotAFunction,
                &[&"'set' property of Map"],
            );
        }
        let adder = adder.as_function();

        let add_entry = |iterator_value: Value| -> ThrowCompletionOr<()> {
            if !iterator_value.is_object() {
                return vm.throw_completion(
                    ErrorKind::TypeError,
                    ErrorType::NotAnObject,
                    &[&format!("Iterator value {iterator_value}")],
                );
            }

            let key = iterator_value.as_object().get(vm, &PropertyKey::from(0))?;
            let value = iterator_value.as_object().get(vm, &PropertyKey::from(1))?;
            call_function_object(vm, adder, Value::from_object(map), &[key, value])?;

            Ok(())
        };
        get_iterator_values(vm, vm.argument(0), |iterator_value| {
            add_entry(iterator_value).err().map(Completion::from)
        })
        .into_throw_completion_or()?;

        Ok(map.upcast())
    }

    // 24.1.2.1 Map.groupBy ( items, callbackfn ), https://tc39.es/ecma262/#sec-map.groupby
    fn group_by(vm: &Vm) -> ThrowCompletionOr<Value> {
        let realm = vm.current_realm().expect("a builtin runs in a realm");

        let items = vm.argument(0);
        let callback_function = vm.argument(1);

        // 1. Let groups be ? GroupBy(items, callbackfn, zero).
        let groups: ValueKeyedGroups = group_by(vm, items, callback_function)?;

        // 2. Let map be ! Construct(%Map%).
        let map = Map::create(vm, realm);

        // 3. For each Record { [[Key]], [[Elements]] } g of groups, do
        for (index, group_elements) in groups.elements.iter().enumerate() {
            // a. Let elements be CreateArrayFromList(g.[[Elements]]).
            let elements = Array::create_from_list(vm, realm, group_elements);

            // b. Let entry be the Record { [[Key]]: g.[[Key]], [[Value]]: elements }.
            // c. Append entry to map.[[MapData]].
            map.map_set(
                groups.keys.get(index).expect("the group exists"),
                Value::from_object(elements),
            );
        }

        // 4. Return map.
        Ok(Value::from_object(map))
    }

    // 24.1.2.3 get Map [ @@species ], https://tc39.es/ecma262/#sec-get-map-@@species
    #[allow(clippy::unnecessary_wraps, reason = "native functions can throw")]
    fn symbol_species_getter(vm: &Vm) -> ThrowCompletionOr<Value> {
        Ok(vm.this_value())
    }
}
