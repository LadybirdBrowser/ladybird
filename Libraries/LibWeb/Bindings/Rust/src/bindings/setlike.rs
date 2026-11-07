/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The setlike declarations of the interfaces implemented in Rust, https://webidl.spec.whatwg.org/#idl-setlike

use core::cell::RefCell;

use libjs_rust::api::{
    Attribute, ErrorKind, ErrorType, Gc, GcWeak, MarkedVec, Object, PropertyAttributes, PropertyKey, PropertyKind,
    RawNativeFunctionPointer, Realm, Set, SetIterator, ThrowCompletionOr, Trace, Value, Vm, call,
};

use crate::bindings::CallContext;
use crate::bindings::interface::{
    Interface, SecurityCheckType, define_attribute, define_operation, this_implementation,
};
use crate::bindings::realm_data::{WrapperWorld, wrapper_world_of};

/// The JS collection of an object in each wrapper world that asked for it, as the entries of a collection hold the
/// wrappers of one world. Nothing keeps a collection alive here, so the next use creates it again if it is collected.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct WorldEntries<T: 'static> {
    #[gc(untraced)]
    entries: RefCell<Vec<(WrapperWorld, GcWeak<T>)>>,
}

impl<T> Default for WorldEntries<T> {
    fn default() -> Self {
        Self {
            entries: RefCell::new(Vec::new()),
        }
    }
}

impl<T> WorldEntries<T> {
    /// The collection of the wrapper world of the call, which `create` makes if there is none.
    pub(crate) fn get_or_create(&self, cx: &CallContext, create: impl FnOnce() -> Gc<T>) -> Gc<T> {
        let wrapper_world = wrapper_world_of(cx.vm(), cx.realm());
        self.entries.borrow_mut().retain(|(_, entries)| entries.get().is_some());
        let existing = self
            .entries
            .borrow()
            .iter()
            .find(|(world, _)| *world == wrapper_world)
            .and_then(|(_, entries)| entries.get());
        if let Some(entries) = existing {
            return entries;
        }
        let entries = create();
        self.entries
            .borrow_mut()
            .push((wrapper_world, GcWeak::new(cx.vm().heap(), entries)));
        entries
    }

    /// Calls back with each live collection, for the caller to update when the object changes. The collections stay
    /// alive through the walk, as updating one can allocate and nothing else holds the rest.
    pub(crate) fn for_each_live(&self, vm: &Vm, mut callback: impl FnMut(Gc<T>)) {
        let live = MarkedVec::new(vm);
        for (_, entries) in self.entries.borrow().iter() {
            if let Some(entries) = entries.get() {
                live.push(entries);
            }
        }
        for entries in live.values().iter() {
            callback(*entries);
        }
    }
}

/// What the implementation of an interface with a setlike declaration implements.
pub(crate) trait Setlike: Interface {
    // https://webidl.spec.whatwg.org/#dfn-set-entries
    fn set_entries(&self, cx: &CallContext) -> Gc<Set>;

    /// The size of the set entries, without creating them and the wrappers they hold.
    fn set_entries_size(&self) -> usize;

    /// Whether the set entries contain the value, which the bindings checked against the value type, without creating
    /// them and the wrappers they hold.
    fn set_entries_contain(&self, value: Value) -> bool;
}

/// What the implementation of an interface with a read-write setlike declaration implements, for the operations that
/// the interface does not declare itself. The value is one that the bindings converted to the value type.
pub(crate) trait WritableSetlike: Setlike {
    fn set_add(&self, cx: &CallContext, value: Value);
    fn set_remove(&self, cx: &CallContext, value: Value) -> bool;
    fn set_clear(&self, cx: &CallContext);
}

/// What the generated bindings of an interface with a setlike declaration define.
pub(crate) trait SetlikeDeclaration: Setlike {
    /// Throws unless the JS value converts to the value type of the declaration.
    fn check_value(vm: &Vm, value: Value) -> ThrowCompletionOr<()>;
}

/// Defines the properties that every setlike declaration has, https://webidl.spec.whatwg.org/#es-setlike
#[expect(clippy::too_many_arguments, reason = "these are the functions of the declaration")]
pub(crate) fn define_setlike_properties(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    size: RawNativeFunctionPointer,
    entries: RawNativeFunctionPointer,
    for_each: RawNativeFunctionPointer,
    has: RawNativeFunctionPointer,
    values: RawNativeFunctionPointer,
) {
    define_attribute(vm, realm, prototype, "size", size, None);
    define_operation(vm, realm, prototype, "entries", entries, 0);
    define_operation(vm, realm, prototype, "keys", values, 0);
    define_operation(vm, realm, prototype, "values", values, 0);
    prototype.define_direct_property(
        vm,
        &PropertyKey::from(vm.well_known_symbols().iterator),
        prototype.get_without_side_effects(vm, &PropertyKey::from_utf8("values")),
        PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE),
    );
    define_operation(vm, realm, prototype, "forEach", for_each, 1);
    define_operation(vm, realm, prototype, "has", has, 1);
}

/// The call context of the realm that a collection of a wrapper world lives in.
pub(crate) fn context_of_set<'vm>(cx: &CallContext<'vm>, set: Gc<Set>) -> CallContext<'vm> {
    CallContext::new(cx.vm(), set.upcast::<Object>().shape().realm())
}

// https://webidl.spec.whatwg.org/#js-set-size
pub(crate) fn set_size<T: Setlike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "size" and type "getter".
    let (_, implementation, _) = this_implementation::<T>(vm, "size", SecurityCheckType::Getter)?;

    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    // 3. Return set's size, converted to a JavaScript value.
    // OPTIMIZATION: The implementation knows the size without creating the set entries.
    Ok(Value::from_f64(implementation.set_entries_size() as f64))
}

fn create_set_iterator<T: Setlike>(vm: &Vm, identifier: &str, kind: PropertyKind) -> ThrowCompletionOr<Value> {
    let (_, implementation, cx) = this_implementation::<T>(vm, identifier, SecurityCheckType::Method)?;
    let set = implementation.set_entries(&cx);
    Ok(Value::from_object(
        SetIterator::create(vm, cx.realm(), set, kind).upcast::<Object>(),
    ))
}

// https://webidl.spec.whatwg.org/#js-set-entries
pub(crate) fn set_iterator_entries<T: Setlike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "entries" and type "method".
    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    // 3. Return the result of creating a set iterator from set with kind "key+value".
    create_set_iterator::<T>(vm, "entries", PropertyKind::KeyAndValue)
}

// https://webidl.spec.whatwg.org/#js-set-values
pub(crate) fn set_iterator_values<T: Setlike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "values" and type "method".
    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    // 3. Return the result of creating a set iterator from set with kind "value".
    create_set_iterator::<T>(vm, "values", PropertyKind::Value)
}

// https://webidl.spec.whatwg.org/#js-set-forEach
pub(crate) fn set_for_each<T: Setlike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "forEach" and type "method".
    let (wrapper, implementation, cx) = this_implementation::<T>(vm, "forEach", SecurityCheckType::Method)?;

    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    let set = implementation.set_entries(&cx);

    // 3. Let callbackFn be the first argument passed to the function, or undefined if not supplied.
    // 4. If IsCallable(callbackFn) is false, throw a TypeError.
    let callback = vm.argument(0);
    if !callback.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback]);
    }

    // 5. Let thisArg be the second argument passed to the function, or undefined if not supplied.
    let this_arg = vm.argument(1);

    // 6. For each value of set:
    let iterator = set.begin();
    while !iterator.is_end() {
        // 1. Let jsValue be value converted to a JavaScript value.
        let value = iterator.current();

        // 2. Perform ? Call(callbackFn, thisArg, « jsValue, jsValue, O»).
        call(vm, callback, this_arg, &[value, value, Value::from_object(wrapper)])?;
        iterator.advance();
    }

    // 7. Return undefined.
    Ok(Value::UNDEFINED)
}

// https://webidl.spec.whatwg.org/#js-set-has
pub(crate) fn set_has<T: SetlikeDeclaration>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "has" and type "method".
    let (_, implementation, _) = this_implementation::<T>(vm, "has", SecurityCheckType::Method)?;

    // 2. Let set be the set entries of the IDL value that represents a reference to O.
    // 3. Let valueType be the value type specified in the setlike declaration.
    // 4. Let valueArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let value be valueArg converted to an IDL value of type valueType.
    let value = vm.argument(0);
    T::check_value(vm, value)?;

    // 7. If set contains value, then return true, otherwise return false.
    // OPTIMIZATION: The implementation looks for the value without creating the set entries.
    Ok(Value::from_bool(implementation.set_entries_contain(value)))
}

// https://webidl.spec.whatwg.org/#js-set-add
pub(crate) fn set_add<T: SetlikeDeclaration + WritableSetlike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "add" and type "method".
    let (wrapper, implementation, cx) = this_implementation::<T>(vm, "add", SecurityCheckType::Method)?;

    // 3. Let valueType be the value type specified in the setlike declaration.
    // 4. Let valueArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let value be valueArg converted to an IDL value of type valueType.
    let value = vm.argument(0);
    T::check_value(vm, value)?;

    // 7. If set does not contain value, then append value to set.
    implementation.set_add(&cx, value);

    // 8. Return O.
    Ok(Value::from_object(wrapper))
}

// https://webidl.spec.whatwg.org/#js-set-delete
pub(crate) fn set_delete<T: SetlikeDeclaration + WritableSetlike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "delete" and type "method".
    let (_, implementation, cx) = this_implementation::<T>(vm, "delete", SecurityCheckType::Method)?;

    // 3. Let valueType be the value type specified in the setlike declaration.
    // 4. Let valueArg be the first argument passed to this function, or undefined if not supplied.
    // 5. Let value be valueArg converted to an IDL value of type valueType.
    let value = vm.argument(0);
    T::check_value(vm, value)?;

    // 7. Let retVal be true if set contains value, or else false.
    // 8. Remove value from set.
    // 9. Return retVal.
    Ok(Value::from_bool(implementation.set_remove(&cx, value)))
}

// https://webidl.spec.whatwg.org/#js-set-clear
pub(crate) fn set_clear<T: WritableSetlike>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let O be the this value, implementation-checked against A with identifier "clear" and type "method".
    let (_, implementation, cx) = this_implementation::<T>(vm, "clear", SecurityCheckType::Method)?;

    // 3. Empty set.
    implementation.set_clear(&cx);

    // 4. Return undefined.
    Ok(Value::UNDEFINED)
}
