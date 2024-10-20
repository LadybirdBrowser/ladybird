/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Utf16View.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/StringObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(StringObject);

// 10.4.3.4 StringCreate ( value, prototype ), https://tc39.es/ecma262/#sec-stringcreate
NonnullGCPtr<StringObject> StringObject::create(Realm& realm, PrimitiveString& primitive_string, Object& prototype)
{
    // 1. Let S be MakeBasicObject(« [[Prototype]], [[Extensible]], [[StringData]] »).
    // 2. Set S.[[Prototype]] to prototype.
    // 3. Set S.[[StringData]] to value.
    // 4. Set S.[[GetOwnProperty]] as specified in 10.4.3.1.
    // 5. Set S.[[DefineOwnProperty]] as specified in 10.4.3.2.
    // 6. Set S.[[OwnPropertyKeys]] as specified in 10.4.3.3.
    // 7. Let length be the length of value.
    // 8. Perform ! DefinePropertyOrThrow(S, "length", PropertyDescriptor { [[Value]]: 𝔽(length), [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: false }).
    // 9. Return S.
    return realm.heap().allocate<StringObject>(realm, primitive_string, prototype);
}

StringObject::StringObject(PrimitiveString& string, Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype, MayInterfereWithIndexedPropertyAccess::Yes)
    , m_string(string)
{
}

void StringObject::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);

    define_direct_property(vm.names.length, Value(m_string->utf16_string_view().length_in_code_units()), 0);
}

void StringObject::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_string);
}

// 10.4.3.5 StringGetOwnProperty ( S, P ), https://tc39.es/ecma262/#sec-stringgetownproperty
static ThrowCompletionOr<Optional<PropertyDescriptor>> string_get_own_property(StringObject const& string, PropertyKey const& property_key)
{
    VERIFY(property_key.is_valid());

    auto& vm = string.vm();

    // 1. If Type(P) is not String, return undefined.
    // NOTE: The spec only uses string and symbol keys, and later coerces to numbers -
    // this is not the case for PropertyKey, so '!property_key.is_string()' would be wrong.
    if (property_key.is_symbol())
        return Optional<PropertyDescriptor> {};

    // 2. Let index be CanonicalNumericIndexString(P).
    auto index = canonical_numeric_index_string(property_key, CanonicalIndexMode::IgnoreNumericRoundtrip);

    // 3. If index is undefined, return undefined.
    // 4. If IsIntegralNumber(index) is false, return undefined.
    // 5. If index is -0𝔽, return undefined.
    if (!index.is_index())
        return Optional<PropertyDescriptor> {};

    // 6. Let str be S.[[StringData]].
    // 7. Assert: Type(str) is String.
    auto str = string.primitive_string().utf16_string_view();

    // 8. Let len be the length of str.
    auto length = str.length_in_code_units();

    // 9. If ℝ(index) < 0 or len ≤ ℝ(index), return undefined.
    if (length <= index.as_index())
        return Optional<PropertyDescriptor> {};

    // 10. Let resultStr be the String value of length 1, containing one code unit from str, specifically the code unit at index ℝ(index).
    auto result_str = PrimitiveString::create(vm, Utf16String::create(str.substring_view(index.as_index(), 1)));

    // 11. Return the PropertyDescriptor { [[Value]]: resultStr, [[Writable]]: false, [[Enumerable]]: true, [[Configurable]]: false }.
    return PropertyDescriptor {
        .value = result_str,
        .writable = false,
        .enumerable = true,
        .configurable = false,
    };
}

// 10.4.3.1 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-string-exotic-objects-getownproperty-p
ThrowCompletionOr<Optional<PropertyDescriptor>> StringObject::internal_get_own_property(PropertyKey const& property_key) const
{
    VERIFY(property_key.is_valid());

    // 1. Let desc be OrdinaryGetOwnProperty(S, P).
    auto descriptor = MUST(Object::internal_get_own_property(property_key));

    // 2. If desc is not undefined, return desc.
    if (descriptor.has_value())
        return descriptor;

    // 3. Return StringGetOwnProperty(S, P).
    return string_get_own_property(*this, property_key);
}

// 10.4.3.2 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-string-exotic-objects-defineownproperty-p-desc
ThrowCompletionOr<bool> StringObject::internal_define_own_property(PropertyKey const& property_key, PropertyDescriptor const& property_descriptor)
{
    VERIFY(property_key.is_valid());

    // 1. Let stringDesc be StringGetOwnProperty(S, P).
    auto string_descriptor = TRY(string_get_own_property(*this, property_key));

    // 2. If stringDesc is not undefined, then
    if (string_descriptor.has_value()) {
        // a. Let extensible be S.[[Extensible]].
        auto extensible = m_is_extensible;

        // b. Return IsCompatiblePropertyDescriptor(extensible, Desc, stringDesc).
        return is_compatible_property_descriptor(extensible, property_descriptor, string_descriptor);
    }

    // 3. Return ! OrdinaryDefineOwnProperty(S, P, Desc).
    return Object::internal_define_own_property(property_key, property_descriptor);
}

// 10.4.3.3 [[OwnPropertyKeys]] ( ), https://tc39.es/ecma262/#sec-string-exotic-objects-ownpropertykeys
ThrowCompletionOr<MarkedVector<Value>> StringObject::internal_own_property_keys() const
{
    auto& vm = this->vm();

    // 1. Let keys be a new empty List.
    auto keys = MarkedVector<Value> { heap() };

    // 2. Let str be O.[[StringData]].
    auto str = m_string->utf16_string_view();

    // 3. Assert: Type(str) is String.

    // 4. Let len be the length of str.
    auto length = str.length_in_code_units();

    // 5. For each integer i starting with 0 such that i < len, in ascending order, do
    for (size_t i = 0; i < length; ++i) {
        // a. Add ! ToString(𝔽(i)) as the last element of keys.
        keys.append(PrimitiveString::create(vm, String::number(i)));
    }

    // 6. For each own property key P of O such that P is an array index and ! ToIntegerOrInfinity(P) ≥ len, in ascending numeric index order, do
    for (auto& entry : indexed_properties()) {
        if (entry.index() >= length) {
            // a. Add P as the last element of keys.
            keys.append(PrimitiveString::create(vm, String::number(entry.index())));
        }
    }

    // 7. For each own property key P of O such that Type(P) is String and P is not an array index, in ascending chronological order of property creation, do
    for (auto& it : shape().property_table()) {
        if (it.key.is_string()) {
            // a. Add P as the last element of keys.
            keys.append(it.key.to_value(vm));
        }
    }

    // 8. For each own property key P of O such that Type(P) is Symbol, in ascending chronological order of property creation, do
    for (auto& it : shape().property_table()) {
        if (it.key.is_symbol()) {
            // a. Add P as the last element of keys.
            keys.append(it.key.to_value(vm));
        }
    }

    // 9. Return keys.
    return { move(keys) };
}

}
