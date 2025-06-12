/*
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/ObjectEnvironment.h>
#include <LibJS/Runtime/ValueInlines.h>

namespace JS {

GC_DEFINE_ALLOCATOR(ObjectEnvironment);

ObjectEnvironment::ObjectEnvironment(Object& binding_object, IsWithEnvironment is_with_environment, Environment* outer_environment)
    : Environment(outer_environment)
    , m_binding_object(binding_object)
    , m_with_environment(is_with_environment == IsWithEnvironment::Yes)
{
}

void ObjectEnvironment::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_binding_object);
}

// 9.1.1.2.1 HasBinding ( N ), https://tc39.es/ecma262/#sec-object-environment-records-hasbinding-n
ThrowCompletionOr<bool> ObjectEnvironment::has_binding(FlyString const& name, Optional<size_t>*) const
{
    auto& vm = this->vm();

    // 1. Let bindingObject be envRec.[[BindingObject]].

    // 2. Let foundBinding be ? HasProperty(bindingObject, N).
    bool found_binding = TRY(m_binding_object->has_property(name));

    // 3. If foundBinding is false, return false.
    if (!found_binding)
        return false;

    // 4. If envRec.[[IsWithEnvironment]] is false, return true.
    if (!m_with_environment)
        return true;

    // 5. Let unscopables be ? Get(bindingObject, @@unscopables).
    auto unscopables = TRY(m_binding_object->get(vm.well_known_symbol_unscopables()));

    // 6. If Type(unscopables) is Object, then
    if (unscopables.is_object()) {
        // a. Let blocked be ToBoolean(? Get(unscopables, N)).
        auto blocked = TRY(unscopables.as_object().get(name)).to_boolean();

        // b. If blocked is true, return false.
        if (blocked)
            return false;
    }

    // 7. Return true.
    return true;
}

// 9.1.1.2.2 CreateMutableBinding ( N, D ), https://tc39.es/ecma262/#sec-object-environment-records-createmutablebinding-n-d
ThrowCompletionOr<void> ObjectEnvironment::create_mutable_binding(VM&, FlyString const& name, bool can_be_deleted)
{
    // 1. Let bindingObject be envRec.[[BindingObject]].
    // 2. Perform ? DefinePropertyOrThrow(bindingObject, N, PropertyDescriptor { [[Value]]: undefined, [[Writable]]: true, [[Enumerable]]: true, [[Configurable]]: D }).
    TRY(m_binding_object->define_property_or_throw(name, { .value = js_undefined(), .writable = true, .enumerable = true, .configurable = can_be_deleted }));

    // 3. Return unused.
    return {};
}

// 9.1.1.2.3 CreateImmutableBinding ( N, S ), https://tc39.es/ecma262/#sec-object-environment-records-createimmutablebinding-n-s
ThrowCompletionOr<void> ObjectEnvironment::create_immutable_binding(VM&, FlyString const&, bool)
{
    // "The CreateImmutableBinding concrete method of an object Environment Record is never used within this specification."
    VERIFY_NOT_REACHED();
}

// 9.1.1.2.4 InitializeBinding ( N, V ), https://tc39.es/ecma262/#sec-object-environment-records-initializebinding-n-v
ThrowCompletionOr<void> ObjectEnvironment::initialize_binding(VM& vm, FlyString const& name, Value value, Environment::InitializeBindingHint hint)
{
    // 1. Assert: hint is normal.
    VERIFY(hint == Environment::InitializeBindingHint::Normal);

    // 2. Perform ? envRec.SetMutableBinding(N, V, false).
    TRY(set_mutable_binding(vm, name, value, false));

    // 2. Return unused.
    return {};
}

// 9.1.1.2.5 SetMutableBinding ( N, V, S ), https://tc39.es/ecma262/#sec-object-environment-records-setmutablebinding-n-v-s
ThrowCompletionOr<void> ObjectEnvironment::set_mutable_binding(VM&, FlyString const& name, Value value, bool strict)
{
    auto& vm = this->vm();

    // OPTIMIZATION: For non-with environments in non-strict mode, we don't need the separate HasProperty check since we only use that
    //               information to throw errors in strict mode.
    //               We can't do this for with environments, since it would be observable (e.g via a Proxy)
    // FIXME: I think we could combine HasProperty and Set in strict mode if Set would return a bit more failure information.
    if (!m_with_environment && !strict)
        return m_binding_object->set(name, value, Object::ShouldThrowExceptions::No);

    // 1. Let bindingObject be envRec.[[BindingObject]].
    // 2. Let stillExists be ? HasProperty(bindingObject, N).
    auto still_exists = TRY(m_binding_object->has_property(name));

    // 3. If stillExists is false and S is true, throw a ReferenceError exception.
    if (!still_exists && strict)
        return vm.throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, name);

    // 4. Perform ? Set(bindingObject, N, V, S).
    auto result_or_error = m_binding_object->set(name, value, strict ? Object::ShouldThrowExceptions::Yes : Object::ShouldThrowExceptions::No);

    // Note: Nothing like this in the spec, this is here to produce nicer errors instead of the generic one thrown by Object::set().
    if (result_or_error.is_error() && strict) {
        auto property_or_error = m_binding_object->internal_get_own_property(name);
        // Return the initial error instead of masking it with the new error
        if (property_or_error.is_error())
            return result_or_error.release_error();
        auto property = property_or_error.release_value();
        if (property.has_value() && !property->writable.value_or(true)) {
            return vm.throw_completion<TypeError>(ErrorType::DescWriteNonWritable, name);
        }
    }

    if (result_or_error.is_error())
        return result_or_error.release_error();

    // 5. Return unused.
    return {};
}

// 9.1.1.2.6 GetBindingValue ( N, S ), https://tc39.es/ecma262/#sec-object-environment-records-getbindingvalue-n-s
ThrowCompletionOr<Value> ObjectEnvironment::get_binding_value(VM&, FlyString const& name, bool strict)
{
    auto& vm = this->vm();

    // OPTIMIZATION: For non-with environments in non-strict mode, we don't need the separate HasProperty check
    //               since Get will return undefined for missing properties anyway. So we take advantage of this
    //               to avoid doing both HasProperty and Get.
    //               We can't do this for with environments, since it would be observable (e.g via a Proxy)
    // FIXME: We could combine HasProperty and Get in non-strict mode if Get would return a bit more failure information.
    if (!m_with_environment && !strict)
        return m_binding_object->get(name);

    // 1. Let bindingObject be envRec.[[BindingObject]].
    // 2. Let value be ? HasProperty(bindingObject, N).
    auto value = TRY(m_binding_object->has_property(name));

    // 3. If value is false, then
    if (!value) {
        // a. If S is false, return undefined; otherwise throw a ReferenceError exception.
        if (!strict)
            return js_undefined();
        return vm.throw_completion<ReferenceError>(ErrorType::UnknownIdentifier, name);
    }

    // 4. Return ? Get(bindingObject, N).
    return m_binding_object->get(name);
}

// 9.1.1.2.7 DeleteBinding ( N ), https://tc39.es/ecma262/#sec-object-environment-records-deletebinding-n
ThrowCompletionOr<bool> ObjectEnvironment::delete_binding(VM&, FlyString const& name)
{
    // 1. Let bindingObject be envRec.[[BindingObject]].
    // 2. Return ? bindingObject.[[Delete]](N).
    return m_binding_object->internal_delete(name);
}

}
