/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The pair iterators of the interfaces implemented in Rust, https://webidl.spec.whatwg.org/#idl-iterable

use core::cell::Cell;
use core::ops::Deref;
use core::ptr::NonNull;

use libjs_rust::api::{
    Array, Attribute, EmbedderCell, EmbedderClass, EmbedderValue, ErrorKind, ErrorType, Gc, HostObject, JSHostClass,
    Object, PropertyAttributes, PropertyKey, RawNativeFunctionPointer, Realm, ThrowCompletionOr, Trace, Value, Vm,
    call, create_iterator_result_object, host_data_of, is_host_instance_of,
};

use crate::bindings::CallContext;
use crate::bindings::conversions::ToJs;
use crate::bindings::interface::{Interface, Ref, define_operation, implementation_of, this_object};
use crate::bindings::services::web_bindings_ensure_web_iterator_prototype;

// https://webidl.spec.whatwg.org/#default-iterator-object-kind
#[derive(Clone, Copy)]
pub(crate) enum IterationKind {
    Key,
    Value,
    KeyAndValue,
}

/// A default iterator object, https://webidl.spec.whatwg.org/#dfn-default-iterator-object, which is the host data of
/// a host object of the iterator class of its interface.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct DefaultIterator<T: Interface> {
    target: Ref<T>,
    #[gc(untraced)]
    kind: IterationKind,
    #[gc(untraced)]
    index: Cell<usize>,
}

/// What the implementation of an interface with a pair iterator implements: its value pairs to iterate over, with
/// the Rust types of the key and value types of the iterable declaration.
pub(crate) trait PairIterable: Interface {
    type Key: ToJs + Clone;
    type Value: ToJs + Clone;

    // https://webidl.spec.whatwg.org/#dfn-value-pairs-to-iterate-over
    fn value_pairs_to_iterate_over(&self) -> impl Deref<Target = [(Self::Key, Self::Value)]> + '_;
}

/// What the generated bindings of an interface with a pair iterator define: the classes of its default iterator
/// objects.
pub(crate) trait DefaultIteratorClasses: PairIterable {
    /// The class of the cells of the default iterator objects of the interface.
    const ITERATOR_CELL_CLASS: &'static EmbedderClass<DefaultIterator<Self>>;

    /// The class of the default iterator objects of the interface.
    fn iterator_class() -> &'static JSHostClass;
}

impl<T: DefaultIteratorClasses> EmbedderValue for DefaultIterator<T> {
    const CLASS: &'static EmbedderClass<Self> = T::ITERATOR_CELL_CLASS;
}

/// Defines the iteration methods of an interface with a pair iterator on its interface prototype object,
/// https://webidl.spec.whatwg.org/#define-the-iteration-methods
pub(crate) fn define_pair_iterator_properties(
    vm: &Vm,
    realm: Gc<Realm>,
    prototype: Gc<Object>,
    entries: RawNativeFunctionPointer,
    for_each: RawNativeFunctionPointer,
    keys: RawNativeFunctionPointer,
    values: RawNativeFunctionPointer,
) {
    define_operation(vm, realm, prototype, "entries", entries, 0);
    define_operation(vm, realm, prototype, "forEach", for_each, 1);
    define_operation(vm, realm, prototype, "keys", keys, 0);
    define_operation(vm, realm, prototype, "values", values, 0);

    // https://webidl.spec.whatwg.org/#es-iterator
    prototype.define_direct_property(
        vm,
        &PropertyKey::from(vm.well_known_symbols().iterator),
        prototype.get_without_side_effects(vm, &vm.names.entries),
        PropertyAttributes::new(Attribute::CONFIGURABLE | Attribute::WRITABLE),
    );
}

/// A newly created default iterator object for the interface, with the object as its target, the kind, and index set
/// to 0, in the realm of the call context.
pub(crate) fn create_default_iterator<T: DefaultIteratorClasses>(
    cx: &CallContext,
    target: Ref<T>,
    kind: IterationKind,
) -> Value {
    let vm = cx.vm();
    let realm = cx.realm();
    let iterator = T::ITERATOR_CELL_CLASS.allocate(vm, |_| DefaultIterator {
        target,
        kind,
        index: Cell::new(0),
    });
    // SAFETY: The realm is live, and the interface has a pair iterator.
    let prototype = unsafe { web_bindings_ensure_web_iterator_prototype(realm.as_ptr().cast(), T::INTERFACE_NAME) };
    let prototype = NonNull::new(prototype.cast::<Object>()).expect("LibWeb returns the iterator prototype object");
    // SAFETY: LibWeb returns a live object, the iterator class has no hooks, and the host data is a live cell.
    let object = unsafe {
        HostObject::create(
            vm,
            realm,
            T::iterator_class(),
            Some(Gc::from_non_null(prototype)),
            None,
            Some(iterator.as_non_null().cast()),
        )
    };
    Value::from_object(object.upcast::<Object>())
}

/// The "next" method of the iterator prototype object of the interface,
/// https://webidl.spec.whatwg.org/#es-iterator-prototype-object
pub(crate) fn iterator_next<T: DefaultIteratorClasses>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let interface be the interface for which the iterator prototype object exists.
    // 2. Let thisValue be the this value.
    // 3. Let object be ? ToObject(thisValue).
    let object = vm.this_value().to_object(vm)?;

    // 4. If object is a platform object, then perform a security check, passing:
    //     * the platform object object,
    //     * the identifier "next", and
    //     * the type "method".
    // NB: A default iterator object is not a platform object.

    // 5. If object is not a default iterator object for interface, then throw a TypeError.
    let Some(iterator) = default_iterator_of::<T>(object) else {
        let name = format!("{}Iterator", T::NAME);
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAnObjectOfType, &[&name]);
    };
    let realm = object.shape().realm();

    // 6. Let index be object's index.
    let index = iterator.index.get();

    // 7. Let kind be object's kind.
    let kind = iterator.kind;

    let pair = {
        // 8. Let values be object's target's value pairs to iterate over.
        let values = iterator.target.value_pairs_to_iterate_over();

        // 9. Let len be the length of values.
        let len = values.len();

        // 10. If index is greater than or equal to len, then return CreateIteratorResultObject(undefined, true).
        if index >= len {
            return Ok(Value::from_object(create_iterator_result_object(
                vm,
                realm,
                Value::UNDEFINED,
                true,
            )));
        }

        // 11. Let pair be the entry in values at index index.
        values[index].clone()
    };

    // 12. Set object's index to index + 1.
    iterator.index.set(index + 1);

    // 13. Return the iterator result for pair and kind.
    let cx = CallContext::new(vm, realm);
    iterator_result(&cx, pair, kind)
}

fn default_iterator_of<T: DefaultIteratorClasses>(object: Gc<Object>) -> Option<Gc<EmbedderCell<DefaultIterator<T>>>> {
    if !is_host_instance_of(&object, T::iterator_class()) {
        return None;
    }
    let host_data = host_data_of(&object).expect("a default iterator object holds its cell");
    // SAFETY: The object is a host object of the iterator class, whose host data is the cell of a default iterator.
    unsafe { T::ITERATOR_CELL_CLASS.downcast(host_data) }
}

/// https://webidl.spec.whatwg.org/#iterator-result
fn iterator_result<K: ToJs, V: ToJs>(cx: &CallContext, pair: (K, V), kind: IterationKind) -> ThrowCompletionOr<Value> {
    let (key, value) = pair;
    let result = match kind {
        // 1. Let result be a value determined by the value of kind:
        // -> "key"
        //     1. Let idlKey be pair's key.
        //     2. Let key be the result of converting idlKey to a JavaScript value.
        //     3. result is key.
        IterationKind::Key => key.to_js(cx)?,
        // -> "value"
        //     1. Let idlValue be pair's value.
        //     2. Let value be the result of converting idlValue to a JavaScript value.
        //     3. result is value.
        IterationKind::Value => value.to_js(cx)?,
        // -> "key+value"
        //     1. Let idlKey be pair's key.
        //     2. Let idlValue be pair's value.
        //     3. Let key be the result of converting idlKey to a JavaScript value.
        //     4. Let value be the result of converting idlValue to a JavaScript value.
        //     5. Let array be ! ArrayCreate(2).
        //     6. Perform ! CreateDataPropertyOrThrow(array, "0", key).
        //     7. Perform ! CreateDataPropertyOrThrow(array, "1", value).
        //     8. result is array.
        IterationKind::KeyAndValue => {
            let key = key.to_js(cx)?;
            let value = value.to_js(cx)?;
            Value::from_object(Array::create_from(cx.vm(), cx.realm(), &[key, value]).upcast::<Object>())
        }
    };

    // 2. Return CreateIteratorResultObject(result, false).
    Ok(Value::from_object(create_iterator_result_object(
        cx.vm(),
        cx.realm(),
        result,
        false,
    )))
}

/// The steps of the %Symbol.iterator% and "entries" methods of the interface prototype object,
/// https://webidl.spec.whatwg.org/#define-the-iteration-methods
pub(crate) fn entries<T: DefaultIteratorClasses>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let jsValue be ? ToObject(this value).
    // 2. If jsValue is a platform object, then perform a security check, passing jsValue, "%Symbol.iterator%", and
    //    "method".
    // 3. If jsValue does not implement definition, then throw a TypeError.
    let (js_value, realm) = this_object(vm, T::wrapper_class(), T::NAME)?;

    // 4. Return a newly created default iterator object for definition, with jsValue as its target, "key+value" as its
    //    kind, and index set to 0.
    Ok(create_default_iterator(
        &CallContext::new(vm, realm),
        implementation_of::<T>(js_value),
        IterationKind::KeyAndValue,
    ))
}

/// The steps of the "keys" method of the interface prototype object,
/// https://webidl.spec.whatwg.org/#define-the-iteration-methods
pub(crate) fn keys<T: DefaultIteratorClasses>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let jsValue be ? ToObject(this value).
    // 2. If jsValue is a platform object, then perform a security check, passing jsValue, "keys", and "method".
    // 3. If jsValue does not implement definition, then throw a TypeError.
    let (js_value, realm) = this_object(vm, T::wrapper_class(), T::NAME)?;

    // 4. Return a newly created default iterator object for definition, with jsValue as its target, "key" as its kind,
    //    and index set to 0.
    Ok(create_default_iterator(
        &CallContext::new(vm, realm),
        implementation_of::<T>(js_value),
        IterationKind::Key,
    ))
}

/// The steps of the "values" method of the interface prototype object,
/// https://webidl.spec.whatwg.org/#define-the-iteration-methods
pub(crate) fn values<T: DefaultIteratorClasses>(vm: &Vm) -> ThrowCompletionOr<Value> {
    // 1. Let jsValue be ? ToObject(this value).
    // 2. If jsValue is a platform object, then perform a security check, passing jsValue, "values", and "method".
    // 3. If jsValue does not implement definition, then throw a TypeError.
    let (js_value, realm) = this_object(vm, T::wrapper_class(), T::NAME)?;

    // 4. Return a newly created default iterator object for definition, with jsValue as its target, "value" as its
    //    kind, and index set to 0.
    Ok(create_default_iterator(
        &CallContext::new(vm, realm),
        implementation_of::<T>(js_value),
        IterationKind::Value,
    ))
}

/// The steps of the "forEach" method of the interface prototype object,
/// https://webidl.spec.whatwg.org/#define-the-iteration-methods
pub(crate) fn for_each<T: DefaultIteratorClasses>(vm: &Vm) -> ThrowCompletionOr<Value> {
    let callback = vm.argument(0);
    let this_arg = vm.argument(1);

    // 1. Let jsValue be ? ToObject(this value).
    // 2. If jsValue is a platform object, then perform a security check, passing jsValue, "forEach", and "method".
    // 3. If jsValue does not implement definition, then throw a TypeError.
    let (js_value, realm) = this_object(vm, T::wrapper_class(), T::NAME)?;
    let cx = CallContext::new(vm, realm);

    // 4. Let idlCallback be callback, converted to a Function.
    if !callback.is_function() {
        return vm.throw_completion(ErrorKind::TypeError, ErrorType::NotAFunction, &[&callback]);
    }

    // 5. Let idlObject be the IDL interface type value that represents a reference to jsValue.
    let idl_object = implementation_of::<T>(js_value);

    // 6. Let pairs be idlObject's list of value pairs to iterate over.
    // 7. Let i be 0.
    let mut i = 0;

    // 8. While i < pairs's size:
    while i < idl_object.value_pairs_to_iterate_over().len() {
        // 1. Let pair be pairs[i].
        let (key, value) = idl_object.value_pairs_to_iterate_over()[i].clone();

        // 2. Invoke idlCallback with « pair's value, pair's key, idlObject » and with thisArg as the callback this
        //    value.
        let value = value.to_js(&cx)?;
        let key = key.to_js(&cx)?;
        call(vm, callback, this_arg, &[value, key, Value::from_object(js_value)])?;

        // 3. Set pairs to idlObject's current list of value pairs to iterate over. (It might have changed.)
        // 4. Set i to i + 1.
        i += 1;
    }

    Ok(Value::UNDEFINED)
}
