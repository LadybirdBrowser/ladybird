/*
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Object.h>
#include <LibJS/Runtime/PropertyDescriptor.h>
#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

// 6.2.5.1 IsAccessorDescriptor ( Desc ), https://tc39.es/ecma262/#sec-isaccessordescriptor
bool PropertyDescriptor::is_accessor_descriptor() const
{
    // 1. If Desc is undefined, return false.

    // 2. If Desc has a [[Get]] field, return true.
    if (get.has_value())
        return true;

    // 3. If Desc has a [[Set]] field, return true.
    if (set.has_value())
        return true;

    // 4. Return false.
    return false;
}

// 6.2.5.2 IsDataDescriptor ( Desc ), https://tc39.es/ecma262/#sec-isdatadescriptor
bool PropertyDescriptor::is_data_descriptor() const
{
    // 1. If Desc is undefined, return false.

    // 2. If Desc has a [[Value]] field, return true.
    if (value.has_value())
        return true;

    // 3. If Desc has a [[Writable]] field, return true.
    if (writable.has_value())
        return true;

    // 4. Return false.
    return false;
}

// 6.2.5.3 IsGenericDescriptor ( Desc ), https://tc39.es/ecma262/#sec-isgenericdescriptor
bool PropertyDescriptor::is_generic_descriptor() const
{
    // 1. If Desc is undefined, return false.

    // 2. If IsAccessorDescriptor(Desc) is true, return false.
    if (is_accessor_descriptor())
        return false;

    // 3. If IsDataDescriptor(Desc) is true, return false.
    if (is_data_descriptor())
        return false;

    // 4. Return true.
    return true;
}

// 6.2.5.4 FromPropertyDescriptor ( Desc ), https://tc39.es/ecma262/#sec-frompropertydescriptor
Value from_property_descriptor(VM& vm, Optional<PropertyDescriptor> const& property_descriptor)
{
    auto& realm = *vm.current_realm();

    if (!property_descriptor.has_value())
        return js_undefined();
    auto object = Object::create(realm, realm.intrinsics().object_prototype());
    if (property_descriptor->value.has_value())
        MUST(object->create_data_property_or_throw(vm.names.value, *property_descriptor->value));
    if (property_descriptor->writable.has_value())
        MUST(object->create_data_property_or_throw(vm.names.writable, Value(*property_descriptor->writable)));
    if (property_descriptor->get.has_value())
        MUST(object->create_data_property_or_throw(vm.names.get, *property_descriptor->get ? Value(*property_descriptor->get) : js_undefined()));
    if (property_descriptor->set.has_value())
        MUST(object->create_data_property_or_throw(vm.names.set, *property_descriptor->set ? Value(*property_descriptor->set) : js_undefined()));
    if (property_descriptor->enumerable.has_value())
        MUST(object->create_data_property_or_throw(vm.names.enumerable, Value(*property_descriptor->enumerable)));
    if (property_descriptor->configurable.has_value())
        MUST(object->create_data_property_or_throw(vm.names.configurable, Value(*property_descriptor->configurable)));
    return object;
}

// 6.2.5.5 ToPropertyDescriptor ( Obj ), https://tc39.es/ecma262/#sec-topropertydescriptor
ThrowCompletionOr<PropertyDescriptor> to_property_descriptor(VM& vm, Value argument)
{
    // 1. If Type(Obj) is not Object, throw a TypeError exception.
    if (!argument.is_object())
        return vm.throw_completion<TypeError>(ErrorType::NotAnObject, argument.to_string_without_side_effects());

    auto& object = argument.as_object();

    // 2. Let desc be a new Property Descriptor that initially has no fields.
    PropertyDescriptor descriptor;

    // 3. Let hasEnumerable be ? HasProperty(Obj, "enumerable").
    auto has_enumerable = TRY(object.has_property(vm.names.enumerable));

    // 4. If hasEnumerable is true, then
    if (has_enumerable) {
        // a. Let enumerable be ToBoolean(? Get(Obj, "enumerable")).
        auto enumerable = TRY(object.get(vm.names.enumerable)).to_boolean();

        // b. Set desc.[[Enumerable]] to enumerable.
        descriptor.enumerable = enumerable;
    }

    // 5. Let hasConfigurable be ? HasProperty(Obj, "configurable").
    auto has_configurable = TRY(object.has_property(vm.names.configurable));

    // 6. If hasConfigurable is true, then
    if (has_configurable) {
        // a. Let configurable be ToBoolean(? Get(Obj, "configurable")).
        auto configurable = TRY(object.get(vm.names.configurable)).to_boolean();

        // b. Set desc.[[Configurable]] to configurable.
        descriptor.configurable = configurable;
    }

    // 7. Let hasValue be ? HasProperty(Obj, "value").
    auto has_value = TRY(object.has_property(vm.names.value));

    // 8. If hasValue is true, then
    if (has_value) {
        // a. Let value be ? Get(Obj, "value").
        auto value = TRY(object.get(vm.names.value));

        // b. Set desc.[[Value]] to value.
        descriptor.value = value;
    }

    // 9. Let hasWritable be ? HasProperty(Obj, "writable").
    auto has_writable = TRY(object.has_property(vm.names.writable));

    // 10. If hasWritable is true, then
    if (has_writable) {
        // a. Let writable be ToBoolean(? Get(Obj, "writable")).
        auto writable = TRY(object.get(vm.names.writable)).to_boolean();

        // b. Set desc.[[Writable]] to writable.
        descriptor.writable = writable;
    }

    // 11. Let hasGet be ? HasProperty(Obj, "get").
    auto has_get = TRY(object.has_property(vm.names.get));

    // 12. If hasGet is true, then
    if (has_get) {
        // a. Let getter be ? Get(Obj, "get").
        auto getter = TRY(object.get(vm.names.get));

        // b. If IsCallable(getter) is false and getter is not undefined, throw a TypeError exception.
        if (!getter.is_function() && !getter.is_undefined())
            return vm.throw_completion<TypeError>(ErrorType::AccessorBadField, "get");

        // c. Set desc.[[Get]] to getter.
        descriptor.get = getter.is_function() ? &getter.as_function() : nullptr;
    }

    // 13. Let hasSet be ? HasProperty(Obj, "set").
    auto has_set = TRY(object.has_property(vm.names.set));

    // 14. If hasSet is true, then
    if (has_set) {
        // a. Let setter be ? Get(Obj, "set").
        auto setter = TRY(object.get(vm.names.set));

        // b. If IsCallable(setter) is false and setter is not undefined, throw a TypeError exception.
        if (!setter.is_function() && !setter.is_undefined())
            return vm.throw_completion<TypeError>(ErrorType::AccessorBadField, "set");

        // c. Set desc.[[Set]] to setter.
        descriptor.set = setter.is_function() ? &setter.as_function() : nullptr;
    }

    // 15. If desc has a [[Get]] field or desc has a [[Set]] field, then
    if (descriptor.get.has_value() || descriptor.set.has_value()) {
        // a. If desc has a [[Value]] field or desc has a [[Writable]] field, throw a TypeError exception.
        if (descriptor.value.has_value() || descriptor.writable.has_value())
            return vm.throw_completion<TypeError>(ErrorType::AccessorValueOrWritable);
    }

    // 16. Return desc.
    return descriptor;
}

// 6.2.5.6 CompletePropertyDescriptor ( Desc ), https://tc39.es/ecma262/#sec-completepropertydescriptor
void PropertyDescriptor::complete()
{
    if (is_generic_descriptor() || is_data_descriptor()) {
        if (!value.has_value())
            value = Value {};
        if (!writable.has_value())
            writable = false;
    } else {
        if (!get.has_value())
            get = nullptr;
        if (!set.has_value())
            set = nullptr;
    }
    if (!enumerable.has_value())
        enumerable = false;
    if (!configurable.has_value())
        configurable = false;
}

// Non-standard, just a convenient way to get from three Optional<bool> to PropertyAttributes.
PropertyAttributes PropertyDescriptor::attributes() const
{
    u8 attributes = 0;
    if (writable.value_or(false))
        attributes |= Attribute::Writable;
    if (enumerable.value_or(false))
        attributes |= Attribute::Enumerable;
    if (configurable.value_or(false))
        attributes |= Attribute::Configurable;
    return { attributes };
}

}
