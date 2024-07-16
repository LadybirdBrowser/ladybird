/*
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/KeyedCollections.h>
#include <LibJS/Runtime/Set.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

JS_DEFINE_ALLOCATOR(Set);

NonnullGCPtr<Set> Set::create(Realm& realm)
{
    return realm.heap().allocate<Set>(realm, realm.intrinsics().set_prototype());
}

Set::Set(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

void Set::initialize(Realm& realm)
{
    m_values = Map::create(realm);
}

NonnullGCPtr<Set> Set::copy() const
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();
    // FIXME: This is very inefficient, but there's no better way to do this at the moment, as the underlying Map
    //  implementation of m_values uses a non-copyable RedBlackTree.
    auto result = Set::create(realm);
    for (auto const& entry : *this)
        result->set_add(entry.key);
    return *result;
}

void Set::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_values);
}

// 24.2.1.2 GetSetRecord ( obj ), https://tc39.es/ecma262/#sec-getsetrecord
ThrowCompletionOr<SetRecord> get_set_record(VM& vm, Value value)
{
    // 1. If obj is not an Object, throw a TypeError exception.
    if (!value.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, value.to_string_without_side_effects());
    auto const& object = value.as_object();

    // 2. Let rawSize be ? Get(obj, "size").
    auto raw_size = TRY(object.get(vm.names.size));

    // 3. Let numSize be ? ToNumber(rawSize).
    auto number_size = TRY(raw_size.to_number(vm));

    // 4. NOTE: If rawSize is undefined, then numSize will be NaN.
    // 5. If numSize is NaN, throw a TypeError exception.
    if (number_size.is_nan())
        return vm.throw_completion<TypeError>(ErrorType::NumberIsNaN, "size"sv);

    // 6. Let intSize be ! ToIntegerOrInfinity(numSize).
    auto integer_size = MUST(number_size.to_integer_or_infinity(vm));

    // 7. If intSize < 0, throw a RangeError exception.
    if (integer_size < 0)
        return vm.throw_completion<RangeError>(ErrorType::NumberIsNegative, "size"sv);

    // 8. Let has be ? Get(obj, "has").
    auto has = TRY(object.get(vm.names.has));

    // 9. If IsCallable(has) is false, throw a TypeError exception.
    if (!has.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, has.to_string_without_side_effects());

    // 10. Let keys be ? Get(obj, "keys").
    auto keys = TRY(object.get(vm.names.keys));

    // 11. If IsCallable(keys) is false, throw a TypeError exception.
    if (!keys.is_function())
        return vm.throw_completion<TypeError>(ErrorType::NotAFunction, keys.to_string_without_side_effects());

    // 12. Return a new Set Record { [[SetObject]]: obj, [[Size]]: intSize, [[Has]]: has, [[Keys]]: keys }.
    return SetRecord { .set_object = object, .size = integer_size, .has = has.as_function(), .keys = keys.as_function() };
}

// 24.2.1.3 SetDataHas ( setData, value ), https://tc39.es/ecma262/#sec-setdatahas
bool set_data_has(NonnullGCPtr<Set> set_data, Value value)
{
    // NOTE: We do not need to implement SetDataIndex, as we do not implement the use of empty slots in Set. But we do
    //       need to match its behavior of always canonicalizing the provided value.
    value = canonicalize_keyed_collection_key(value);

    // 1. If SetDataIndex(setData, value) is not-found, return false.
    // 2. Return true.
    return set_data->set_has(value);
}

}
