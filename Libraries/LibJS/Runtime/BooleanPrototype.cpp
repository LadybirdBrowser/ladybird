/*
 * Copyright (c) 2020, Jack Karamanian <karamanian.jack@gmail.com>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Function.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/BooleanPrototype.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(BooleanPrototype);

BooleanPrototype::BooleanPrototype(Realm& realm)
    : BooleanObject(false, realm.intrinsics().object_prototype())
{
}

void BooleanPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Writable | Attribute::Configurable;
    define_native_function(realm, vm.names.toString, to_string, 0, attr);
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
}

// thisBooleanValue ( value ), https://tc39.es/ecma262/#thisbooleanvalue
static ThrowCompletionOr<bool> this_boolean_value(VM& vm, Value value)
{
    // 1. If value is a Boolean, return value.
    if (value.is_boolean())
        return value.as_bool();

    // 2. If value is an Object and value has a [[BooleanData]] internal slot, then
    if (value.is_object() && is<BooleanObject>(value.as_object())) {
        // a. Let b be value.[[BooleanData]].
        // b. Assert: b is a Boolean.
        // c. Return b.
        return static_cast<BooleanObject&>(value.as_object()).boolean();
    }

    // 3. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Boolean");
}

// 20.3.3.2 Boolean.prototype.toString ( ), https://tc39.es/ecma262/#sec-boolean.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(BooleanPrototype::to_string)
{
    // 1. Let b be ? thisBooleanValue(this value).
    auto b = TRY(this_boolean_value(vm, vm.this_value()));

    // 2. If b is true, return "true"; else return "false".
    return PrimitiveString::create(vm, TRY_OR_THROW_OOM(vm, String::from_utf8(b ? "true"sv : "false"sv)));
}

// 20.3.3.3 Boolean.prototype.valueOf ( ), https://tc39.es/ecma262/#sec-boolean.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(BooleanPrototype::value_of)
{
    // 1. Return ? thisBooleanValue(this value).
    return TRY(this_boolean_value(vm, vm.this_value()));
}

}
