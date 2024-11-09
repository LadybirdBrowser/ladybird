/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/Iterator.h>
#include <LibJS/Runtime/Map.h>
#include <LibJS/Runtime/MapConstructor.h>

namespace JS {

JS_DEFINE_ALLOCATOR(MapConstructor);

MapConstructor::MapConstructor(Realm& realm)
    : NativeFunction(realm.vm().names.Map.as_string(), realm.intrinsics().function_prototype())
{
}

void MapConstructor::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    // 24.1.2.2 Map.prototype, https://tc39.es/ecma262/#sec-map.prototype
    define_direct_property(vm.names.prototype, realm.intrinsics().map_prototype(), 0);

    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.groupBy, group_by, 2, attr);

    define_native_accessor(realm, vm.well_known_symbol_species(), symbol_species_getter, {}, Attribute::Configurable);

    define_direct_property(vm.names.length, Value(0), Attribute::Configurable);
}

// 24.1.1.1 Map ( [ iterable ] ), https://tc39.es/ecma262/#sec-map-iterable
ThrowCompletionOr<Value> MapConstructor::call()
{
    auto& vm = this->vm();
    return vm.throw_completion<TypeError>(ErrorType::ConstructorWithoutNew, vm.names.Map);
}

// 24.1.1.1 Map ( [ iterable ] ), https://tc39.es/ecma262/#sec-map-iterable
ThrowCompletionOr<NonnullGCPtr<Object>> MapConstructor::construct(FunctionObject& new_target)
{
    auto& vm = this->vm();

    auto map = TRY(ordinary_create_from_constructor<Map>(vm, new_target, &Intrinsics::map_prototype));

    if (vm.argument(0).is_nullish())
        return map;

    auto adder = TRY(map->get(vm.names.set));
    if (!adder.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, "'set' property of Map");

    (void)TRY(get_iterator_values(vm, vm.argument(0), [&](Value iterator_value) -> Optional<Completion> {
        if (!iterator_value.is_object())
            return vm.throw_completion<TypeError>(ErrorType::NotAnObject, ByteString::formatted("Iterator value {}", iterator_value.to_string_without_side_effects()));

        auto key = TRY(iterator_value.as_object().get(0));
        auto value = TRY(iterator_value.as_object().get(1));
        TRY(JS::call(vm, adder.as_function(), map, key, value));

        return {};
    }));

    return map;
}

// 24.1.2.1 Map.groupBy ( items, callbackfn ), https://tc39.es/ecma262/#sec-map.groupby
JS_DEFINE_NATIVE_FUNCTION(MapConstructor::group_by)
{
    auto& realm = *vm.current_realm();

    auto items = vm.argument(0);
    auto callback_function = vm.argument(1);

    struct KeyedGroupTraits : public Traits<Handle<Value>> {
        static unsigned hash(Handle<Value> const& value_handle)
        {
            return ValueTraits::hash(value_handle.value());
        }

        static bool equals(Handle<Value> const& a, Handle<Value> const& b)
        {
            // AddValueToKeyedGroup uses SameValue on the keys on Step 1.a.
            return same_value(a.value(), b.value());
        }
    };

    // 1. Let groups be ? GroupBy(items, callbackfn, zero).
    auto groups = TRY((JS::group_by<OrderedHashMap<Handle<Value>, MarkedVector<Value>, KeyedGroupTraits>, void>(vm, items, callback_function)));

    // 2. Let map be ! Construct(%Map%).
    auto map = Map::create(realm);

    // 3. For each Record { [[Key]], [[Elements]] } g of groups, do
    for (auto& group : groups) {
        // a. Let elements be CreateArrayFromList(g.[[Elements]]).
        auto elements = Array::create_from(realm, group.value);

        // b. Let entry be the Record { [[Key]]: g.[[Key]], [[Value]]: elements }.
        // c. Append entry to map.[[MapData]].
        map->map_set(group.key.value(), elements);
    }

    // 4. Return map.
    return map;
}

// 24.1.2.3 get Map [ @@species ], https://tc39.es/ecma262/#sec-get-map-@@species
JS_DEFINE_NATIVE_FUNCTION(MapConstructor::symbol_species_getter)
{
    return vm.this_value();
}

}
