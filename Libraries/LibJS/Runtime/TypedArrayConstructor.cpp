/*
 * Copyright (c) 2020-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/TypedArrayConstructor.h>

namespace JS {

GC_DEFINE_ALLOCATOR(TypedArrayConstructor);

TypedArrayConstructor::TypedArrayConstructor(FlyString const& name, Object& prototype)
    : NativeFunction(name, prototype)
{
}

TypedArrayConstructor::TypedArrayConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.TypedArray.as_string(), realm.intrinsics().function_prototype())
{
}

void TypedArrayConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 23.2.2.3 %TypedArray%.prototype, https://tc39.es/ecma262/#sec-%typedarray%.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().typed_array_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.from, from, 1, attr);
    define_native_function(realm, vm.names.of, of, 0, attr);

    define_native_accessor(realm, vm.well_known_symbol_species(), symbol_species_getter, {}, Attribute::Configurable);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 23.2.1.1 %TypedArray% ( ), https://tc39.es/ecma262/#sec-%typedarray%
ThrowCompletionOr<Value> TypedArrayConstructor::call()
{
    return TRY(construct(*this));
}

// 23.2.1.1 %TypedArray% ( ), https://tc39.es/ecma262/#sec-%typedarray%
ThrowCompletionOr<GC::Ref<Object>> TypedArrayConstructor::construct(FunctionObject&)
{
    // 1. Throw a TypeError exception.
    return vm().throw_completion<TypeError>(ErrorType::ClassIsAbstract, "TypedArray");
}

// 23.2.2.1 %TypedArray%.from ( source [ , mapfn [ , thisArg ] ] ), https://tc39.es/ecma262/#sec-%typedarray%.from
JS_DEFINE_NATIVE_FUNCTION(TypedArrayConstructor::from)
{
    auto source = vm.argument(0);
    auto map_fn_value = vm.argument(1);
    auto this_arg = vm.argument(2);

    // 1. Let C be the this value.
    auto constructor = vm.this_value();

    // 2. If IsConstructor(C) is false, throw a TypeError exception.
    if (!constructor.is_constructor())
        return vm.throw_completion<TypeError>(ErrorType::NotAConstructor, constructor.to_string_without_side_effects());

    // 3. If mapfn is undefined, let mapping be false.
    GC::Ptr<FunctionObject> map_fn;

    // 4. Else,
    if (!map_fn_value.is_undefined()) {
        // a. If IsCallable(mapfn) is false, throw a TypeError exception.
        if (!map_fn_value.is_function())
            return vm.throw_completion<TypeError>(ErrorType::NotAFunction, map_fn_value.to_string_without_side_effects());

        // b. Let mapping be true.
        map_fn = &map_fn_value.as_function();
    }

    // 5. Let usingIterator be ? GetMethod(source, @@iterator).
    auto using_iterator = TRY(source.get_method(vm, vm.well_known_symbol_iterator()));

    // 6. If usingIterator is not undefined, then
    if (using_iterator) {
        // a. Let values be ? IteratorToList(? GetIteratorFromMethod(source, usingIterator)).
        auto values = TRY(iterator_to_list(vm, TRY(get_iterator_from_method(vm, source, *using_iterator))));

        // b. Let len be the number of elements in values.
        auto length = values.size();

        // c. Let targetObj be ? TypedArrayCreate(C, « 𝔽(len) »).
        GC::RootVector<Value> arguments(vm.heap());
        arguments.empend(length);
        auto* target_object = TRY(typed_array_create(vm, constructor.as_function(), move(arguments)));

        // d. Let k be 0.
        // e. Repeat, while k < len,
        for (size_t k = 0; k < length; ++k) {
            // i. Let Pk be ! ToString(𝔽(k)).
            auto property_key = PropertyKey { k };

            // ii. Let kValue be the first element of values.
            // iii. Remove the first element from values.
            auto k_value = values[k];

            Value mapped_value;

            // iv. If mapping is true, then
            if (map_fn) {
                // 1. Let mappedValue be ? Call(mapfn, thisArg, « kValue, 𝔽(k) »).
                mapped_value = TRY(JS::call(vm, *map_fn, this_arg, k_value, Value(k)));
            }
            // v. Else, let mappedValue be kValue.
            else {
                mapped_value = k_value;
            }

            // vi. Perform ? Set(targetObj, Pk, mappedValue, true).
            TRY(target_object->set(property_key, mapped_value, Object::ShouldThrowExceptions::Yes));

            // vii. Set k to k + 1.
        }

        // f. Assert: values is now an empty List.
        // NOTE: We don't actually empty the list.

        // g. Return targetObj.
        return target_object;
    }

    // 7. NOTE: source is not an Iterable so assume it is already an array-like object.
    // 8. Let arrayLike be ! ToObject(source).
    auto array_like = MUST(source.to_object(vm));

    // 9. Let len be ? LengthOfArrayLike(arrayLike).
    auto length = TRY(length_of_array_like(vm, array_like));

    // 10. Let targetObj be ? TypedArrayCreate(C, « 𝔽(len) »).
    GC::RootVector<Value> arguments(vm.heap());
    arguments.empend(length);
    auto* target_object = TRY(typed_array_create(vm, constructor.as_function(), move(arguments)));

    // 11. Let k be 0.
    // 12. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // b. Let kValue be ? Get(arrayLike, Pk).
        auto k_value = TRY(array_like->get(property_key));

        Value mapped_value;

        // c. If mapping is true, then
        if (map_fn) {
            // i. Let mappedValue be ? Call(mapfn, thisArg, « kValue, 𝔽(k) »).
            mapped_value = TRY(JS::call(vm, *map_fn, this_arg, k_value, Value(k)));
        }
        // d. Else, let mappedValue be kValue.
        else {
            mapped_value = k_value;
        }

        // e. Perform ? Set(targetObj, Pk, mappedValue, true).
        TRY(target_object->set(property_key, mapped_value, Object::ShouldThrowExceptions::Yes));

        // f. Set k to k + 1.
    }

    // 13. Return targetObj.
    return target_object;
}

// 23.2.2.2 %TypedArray%.of ( ...items ), https://tc39.es/ecma262/#sec-%typedarray%.of
JS_DEFINE_NATIVE_FUNCTION(TypedArrayConstructor::of)
{
    // 1. Let len be the number of elements in items.
    auto length = vm.argument_count();

    // 2. Let C be the this value.
    auto constructor = vm.this_value();

    // 3. If IsConstructor(C) is false, throw a TypeError exception.
    if (!constructor.is_constructor())
        return vm.throw_completion<TypeError>(ErrorType::NotAConstructor, constructor.to_string_without_side_effects());

    // 4. Let newObj be ? TypedArrayCreate(C, « 𝔽(len) »).
    GC::RootVector<Value> arguments(vm.heap());
    arguments.append(Value(length));
    auto* new_object = TRY(typed_array_create(vm, constructor.as_function(), move(arguments)));

    // 5. Let k be 0.
    // 6. Repeat, while k < len,
    for (size_t k = 0; k < length; ++k) {
        // a. Let kValue be items[k].
        auto k_value = vm.argument(k);

        // b. Let Pk be ! ToString(𝔽(k)).
        auto property_key = PropertyKey { k };

        // c. Perform ? Set(newObj, Pk, kValue, true).
        TRY(new_object->set(property_key, k_value, Object::ShouldThrowExceptions::Yes));

        // d. Set k to k + 1.
    }

    // 7. Return newObj.
    return new_object;
}

// 23.2.2.4 get %TypedArray% [ @@species ], https://tc39.es/ecma262/#sec-get-%typedarray%-@@species
JS_DEFINE_NATIVE_FUNCTION(TypedArrayConstructor::symbol_species_getter)
{
    // 1. Return the this value.
    return vm.this_value();
}

}
