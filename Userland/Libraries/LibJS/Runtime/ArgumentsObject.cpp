/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/ArgumentsObject.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/GlobalObject.h>

namespace JS {

JS_DEFINE_ALLOCATOR(ArgumentsObject);

ArgumentsObject::ArgumentsObject(Realm& realm, Environment& environment)
    : Object(ConstructWithPrototypeTag::Tag, realm.intrinsics().object_prototype(), MayInterfereWithIndexedPropertyAccess::Yes)
    , m_environment(environment)
{
}

void ArgumentsObject::initialize(Realm& realm)
{
    Base::initialize(realm);
    set_has_parameter_map();
    m_parameter_map = Object::create(realm, nullptr);
}

void ArgumentsObject::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_environment);
    visitor.visit(m_parameter_map);
}

// 10.4.4.3 [[Get]] ( P, Receiver ), https://tc39.es/ecma262/#sec-arguments-exotic-objects-get-p-receiver
ThrowCompletionOr<Value> ArgumentsObject::internal_get(PropertyKey const& property_key, Value receiver, CacheablePropertyMetadata* cacheable_metadata, PropertyLookupPhase phase) const
{
    // 1. Let map be args.[[ParameterMap]].
    auto& map = *m_parameter_map;

    // 2. Let isMapped be ! HasOwnProperty(map, P).
    bool is_mapped = MUST(m_parameter_map->has_own_property(property_key));

    // 3. If isMapped is false, then
    if (!is_mapped) {
        // a. Return ? OrdinaryGet(args, P, Receiver).
        return Object::internal_get(property_key, receiver, cacheable_metadata, phase);
    }

    // FIXME: a. Assert: map contains a formal parameter mapping for P.

    // b. Return ! Get(map, P).
    return MUST(map.get(property_key));
}

// 10.4.4.4 [[Set]] ( P, V, Receiver ), https://tc39.es/ecma262/#sec-arguments-exotic-objects-set-p-v-receiver
ThrowCompletionOr<bool> ArgumentsObject::internal_set(PropertyKey const& property_key, Value value, Value receiver, CacheablePropertyMetadata*)
{
    bool is_mapped = false;

    // 1. If SameValue(args, Receiver) is false, then
    if (!same_value(this, receiver)) {
        // a. Let isMapped be false.
        is_mapped = false;
    } else {
        // a. Let map be args.[[ParameterMap]].
        // b. Let isMapped be ! HasOwnProperty(map, P).
        is_mapped = MUST(parameter_map().has_own_property(property_key));
    }

    // 3. If isMapped is true, then
    if (is_mapped) {
        // a. Assert: The following Set will succeed, since formal parameters mapped by arguments objects are always writable.

        // b. Perform ! Set(map, P, V, false).
        MUST(m_parameter_map->set(property_key, value, Object::ShouldThrowExceptions::No));
    }

    // 4. Return ? OrdinarySet(args, P, V, Receiver).
    return Object::internal_set(property_key, value, receiver);
}

// 10.4.4.5 [[Delete]] ( P ), https://tc39.es/ecma262/#sec-arguments-exotic-objects-delete-p
ThrowCompletionOr<bool> ArgumentsObject::internal_delete(PropertyKey const& property_key)
{
    // 1. Let map be args.[[ParameterMap]].
    auto& map = parameter_map();

    // 2. Let isMapped be ! HasOwnProperty(map, P).
    bool is_mapped = MUST(map.has_own_property(property_key));

    // 3. Let result be ? OrdinaryDelete(args, P).
    bool result = TRY(Object::internal_delete(property_key));

    // 4. If result is true and isMapped is true, then
    if (result && is_mapped) {
        // a. Perform ! map.[[Delete]](P).
        MUST(map.internal_delete(property_key));
    }

    // 5. Return result.
    return result;
}

// 10.4.4.1 [[GetOwnProperty]] ( P ), https://tc39.es/ecma262/#sec-arguments-exotic-objects-getownproperty-p
ThrowCompletionOr<Optional<PropertyDescriptor>> ArgumentsObject::internal_get_own_property(PropertyKey const& property_key) const
{
    // 1. Let desc be OrdinaryGetOwnProperty(args, P).
    auto desc = MUST(Object::internal_get_own_property(property_key));

    // 2. If desc is undefined, return desc.
    if (!desc.has_value())
        return desc;

    // 3. Let map be args.[[ParameterMap]].
    // 4. Let isMapped be ! HasOwnProperty(map, P).
    bool is_mapped = MUST(m_parameter_map->has_own_property(property_key));

    // 5. If isMapped is true, then
    if (is_mapped) {
        // a. Set desc.[[Value]] to ! Get(map, P).
        desc->value = MUST(m_parameter_map->get(property_key));
    }

    // 6. Return desc.
    return desc;
}

// 10.4.4.2 [[DefineOwnProperty]] ( P, Desc ), https://tc39.es/ecma262/#sec-arguments-exotic-objects-defineownproperty-p-desc
ThrowCompletionOr<bool> ArgumentsObject::internal_define_own_property(PropertyKey const& property_key, PropertyDescriptor const& descriptor)
{
    // 1. Let map be args.[[ParameterMap]].
    auto& map = parameter_map();

    // 2. Let isMapped be ! HasOwnProperty(map, P).
    bool is_mapped = MUST(map.has_own_property(property_key));

    // 3. Let newArgDesc be Desc.
    auto new_arg_desc = descriptor;

    // 4. If isMapped is true and IsDataDescriptor(Desc) is true, then
    if (is_mapped && descriptor.is_data_descriptor()) {
        // a. If Desc does not have a [[Value]] field and Desc has a [[Writable]] field, and Desc.[[Writable]] is false, then
        if (!descriptor.value.has_value() && descriptor.writable.has_value() && descriptor.writable == false) {
            // i. Set newArgDesc to a copy of Desc.
            new_arg_desc = descriptor;
            // ii. Set newArgDesc.[[Value]] to ! Get(map, P).
            new_arg_desc.value = MUST(map.get(property_key));
        }
    }

    // 5. Let allowed be ! OrdinaryDefineOwnProperty(args, P, newArgDesc).
    bool allowed = MUST(Object::internal_define_own_property(property_key, new_arg_desc));

    // 6. If allowed is false, return false.
    if (!allowed)
        return false;

    // 7. If isMapped is true, then
    if (is_mapped) {
        // a. If IsAccessorDescriptor(Desc) is true, then
        if (descriptor.is_accessor_descriptor()) {
            // i. Perform ! map.[[Delete]](P).
            MUST(map.internal_delete(property_key));
        } else {
            // i. If Desc has a [[Value]] field, then
            if (descriptor.value.has_value()) {
                // 1. Assert: The following Set will succeed, since formal parameters mapped by arguments objects are always writable.

                // 2. Perform ! Set(map, P, Desc.[[Value]], false).
                MUST(map.set(property_key, descriptor.value.value(), Object::ShouldThrowExceptions::No));
            }
            // ii. If Desc has a [[Writable]] field and Desc.[[Writable]] is false, then
            if (descriptor.writable == false) {
                // 1. Perform ! map.[[Delete]](P).
                MUST(map.internal_delete(property_key));
            }
        }
    }

    // 8. Return true.
    return true;
}

}
