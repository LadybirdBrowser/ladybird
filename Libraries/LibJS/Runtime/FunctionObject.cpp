/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

FunctionObject::FunctionObject(Realm& realm, Object* prototype, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
    : Object(realm, prototype, may_interfere_with_indexed_property_access)
{
}

FunctionObject::FunctionObject(Object& prototype, MayInterfereWithIndexedPropertyAccess may_interfere_with_indexed_property_access)
    : Object(ConstructWithPrototypeTag::Tag, prototype, may_interfere_with_indexed_property_access)
{
}

// 10.2.9 SetFunctionName ( F, name [ , prefix ] ), https://tc39.es/ecma262/#sec-setfunctionname
void FunctionObject::set_function_name(Variant<PropertyKey, PrivateName> const& name_arg, Optional<StringView> const& prefix)
{
    auto& vm = this->vm();

    // 1. Assert: F is an extensible object that does not have a "name" own property.
    VERIFY(m_is_extensible);
    VERIFY(!storage_has(vm.names.name));

    String name;

    // 2. If Type(name) is Symbol, then
    if (auto const* property_key = name_arg.get_pointer<PropertyKey>(); property_key && property_key->is_symbol()) {
        // a. Let description be name's [[Description]] value.
        auto const& description = property_key->as_symbol()->description();

        // b. If description is undefined, set name to the empty String.
        if (!description.has_value())
            name = ""_string;
        // c. Else, set name to the string-concatenation of "[", description, and "]".
        else
            name = MUST(String::formatted("[{}]", *description));
    }
    // 3. Else if name is a Private Name, then
    else if (auto const* private_name = name_arg.get_pointer<PrivateName>()) {
        // a. Set name to name.[[Description]].
        name = private_name->description.to_string();
    }
    // NOTE: This is necessary as we use a different parameter name.
    else {
        name = name_arg.get<PropertyKey>().to_string();
    }

    // 4. If F has an [[InitialName]] internal slot, then
    if (is<NativeFunction>(this)) {
        // a. Set F.[[InitialName]] to name.
        static_cast<NativeFunction&>(*this).set_initial_name({}, name);
    }

    // 5. If prefix is present, then
    if (prefix.has_value()) {
        // a. Set name to the string-concatenation of prefix, the code unit 0x0020 (SPACE), and name.
        name = MUST(String::formatted("{} {}", *prefix, name));

        // b. If F has an [[InitialName]] internal slot, then
        if (is<NativeFunction>(this)) {
            // i. Optionally, set F.[[InitialName]] to name.
            static_cast<NativeFunction&>(*this).set_initial_name({}, name);
        }
    }

    // 6. Perform ! DefinePropertyOrThrow(F, "name", PropertyDescriptor { [[Value]]: name, [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: true }).
    MUST(define_property_or_throw(vm.names.name, PropertyDescriptor { .value = PrimitiveString::create(vm, move(name)), .writable = false, .enumerable = false, .configurable = true }));

    // 7. Return unused.
}

// 10.2.10 SetFunctionLength ( F, length ), https://tc39.es/ecma262/#sec-setfunctionlength
void FunctionObject::set_function_length(double length)
{
    auto& vm = this->vm();

    // "length (a non-negative integer or +∞)"
    VERIFY(trunc(length) == length || __builtin_isinf_sign(length) == 1);

    // 1. Assert: F is an extensible object that does not have a "length" own property.
    VERIFY(m_is_extensible);
    VERIFY(!storage_has(vm.names.length));

    // 2. Perform ! DefinePropertyOrThrow(F, "length", PropertyDescriptor { [[Value]]: 𝔽(length), [[Writable]]: false, [[Enumerable]]: false, [[Configurable]]: true }).
    MUST(define_property_or_throw(vm.names.length, PropertyDescriptor { .value = Value { length }, .writable = false, .enumerable = false, .configurable = true }));

    // 3. Return unused.
}

}
