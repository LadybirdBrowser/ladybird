/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/AggregateError.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/PromiseCapability.h>
#include <LibJS/Runtime/PromiseResolvingElementFunctions.h>

namespace JS {

JS_DEFINE_ALLOCATOR(RemainingElements);
JS_DEFINE_ALLOCATOR(PromiseValueList);
JS_DEFINE_ALLOCATOR(PromiseResolvingElementFunction);
JS_DEFINE_ALLOCATOR(PromiseAllResolveElementFunction);
JS_DEFINE_ALLOCATOR(PromiseAllSettledResolveElementFunction);
JS_DEFINE_ALLOCATOR(PromiseAllSettledRejectElementFunction);
JS_DEFINE_ALLOCATOR(PromiseAnyRejectElementFunction);

void PromiseValueList::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_values);
}

PromiseResolvingElementFunction::PromiseResolvingElementFunction(size_t index, PromiseValueList& values, NonnullGCPtr<PromiseCapability const> capability, RemainingElements& remaining_elements, Object& prototype)
    : NativeFunction(prototype)
    , m_index(index)
    , m_values(values)
    , m_capability(capability)
    , m_remaining_elements(remaining_elements)
{
}

void PromiseResolvingElementFunction::initialize(Realm& realm)
{
    Base::initialize(realm);
    define_direct_property(vm().names.length, Value(1), Attribute::Configurable);
}

ThrowCompletionOr<Value> PromiseResolvingElementFunction::call()
{
    if (m_already_called)
        return js_undefined();
    m_already_called = true;

    return resolve_element();
}

void PromiseResolvingElementFunction::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_values);
    visitor.visit(m_capability);
    visitor.visit(m_remaining_elements);
}

NonnullGCPtr<PromiseAllResolveElementFunction> PromiseAllResolveElementFunction::create(Realm& realm, size_t index, PromiseValueList& values, NonnullGCPtr<PromiseCapability const> capability, RemainingElements& remaining_elements)
{
    return realm.heap().allocate<PromiseAllResolveElementFunction>(realm, index, values, capability, remaining_elements, realm.intrinsics().function_prototype());
}

PromiseAllResolveElementFunction::PromiseAllResolveElementFunction(size_t index, PromiseValueList& values, NonnullGCPtr<PromiseCapability const> capability, RemainingElements& remaining_elements, Object& prototype)
    : PromiseResolvingElementFunction(index, values, capability, remaining_elements, prototype)
{
}

ThrowCompletionOr<Value> PromiseAllResolveElementFunction::resolve_element()
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 8. Set values[index] to x.
    m_values->values()[m_index] = vm.argument(0);

    // 9. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
    // 10. If remainingElementsCount.[[Value]] is 0, then
    if (--m_remaining_elements->value == 0) {
        // a. Let valuesArray be CreateArrayFromList(values).
        auto values_array = Array::create_from(realm, m_values->values());

        // b. Return ? Call(promiseCapability.[[Resolve]], undefined, « valuesArray »).
        return JS::call(vm, *m_capability->resolve(), js_undefined(), values_array);
    }

    // 11. Return undefined.
    return js_undefined();
}

NonnullGCPtr<PromiseAllSettledResolveElementFunction> PromiseAllSettledResolveElementFunction::create(Realm& realm, size_t index, PromiseValueList& values, NonnullGCPtr<PromiseCapability const> capability, RemainingElements& remaining_elements)
{
    return realm.heap().allocate<PromiseAllSettledResolveElementFunction>(realm, index, values, capability, remaining_elements, realm.intrinsics().function_prototype());
}

PromiseAllSettledResolveElementFunction::PromiseAllSettledResolveElementFunction(size_t index, PromiseValueList& values, NonnullGCPtr<PromiseCapability const> capability, RemainingElements& remaining_elements, Object& prototype)
    : PromiseResolvingElementFunction(index, values, capability, remaining_elements, prototype)
{
}

ThrowCompletionOr<Value> PromiseAllSettledResolveElementFunction::resolve_element()
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 9. Let obj be OrdinaryObjectCreate(%Object.prototype%).
    auto object = Object::create(realm, realm.intrinsics().object_prototype());

    // 10. Perform ! CreateDataPropertyOrThrow(obj, "status", "fulfilled").
    MUST(object->create_data_property_or_throw(vm.names.status, PrimitiveString::create(vm, "fulfilled"_string)));

    // 11. Perform ! CreateDataPropertyOrThrow(obj, "value", x).
    MUST(object->create_data_property_or_throw(vm.names.value, vm.argument(0)));

    // 12. Set values[index] to obj.
    m_values->values()[m_index] = object;

    // 13. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
    // 14. If remainingElementsCount.[[Value]] is 0, then
    if (--m_remaining_elements->value == 0) {
        // a. Let valuesArray be CreateArrayFromList(values).
        auto values_array = Array::create_from(realm, m_values->values());

        // b. Return ? Call(promiseCapability.[[Resolve]], undefined, « valuesArray »).
        return JS::call(vm, *m_capability->resolve(), js_undefined(), values_array);
    }

    // 15. Return undefined.
    return js_undefined();
}

NonnullGCPtr<PromiseAllSettledRejectElementFunction> PromiseAllSettledRejectElementFunction::create(Realm& realm, size_t index, PromiseValueList& values, NonnullGCPtr<PromiseCapability const> capability, RemainingElements& remaining_elements)
{
    return realm.heap().allocate<PromiseAllSettledRejectElementFunction>(realm, index, values, capability, remaining_elements, realm.intrinsics().function_prototype());
}

PromiseAllSettledRejectElementFunction::PromiseAllSettledRejectElementFunction(size_t index, PromiseValueList& values, NonnullGCPtr<PromiseCapability const> capability, RemainingElements& remaining_elements, Object& prototype)
    : PromiseResolvingElementFunction(index, values, capability, remaining_elements, prototype)
{
}

ThrowCompletionOr<Value> PromiseAllSettledRejectElementFunction::resolve_element()
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 9. Let obj be OrdinaryObjectCreate(%Object.prototype%).
    auto object = Object::create(realm, realm.intrinsics().object_prototype());

    // 10. Perform ! CreateDataPropertyOrThrow(obj, "status", "rejected").
    MUST(object->create_data_property_or_throw(vm.names.status, PrimitiveString::create(vm, "rejected"_string)));

    // 11. Perform ! CreateDataPropertyOrThrow(obj, "reason", x).
    MUST(object->create_data_property_or_throw(vm.names.reason, vm.argument(0)));

    // 12. Set values[index] to obj.
    m_values->values()[m_index] = object;

    // 13. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
    // 14. If remainingElementsCount.[[Value]] is 0, then
    if (--m_remaining_elements->value == 0) {
        // a. Let valuesArray be CreateArrayFromList(values).
        auto values_array = Array::create_from(realm, m_values->values());

        // b. Return ? Call(promiseCapability.[[Resolve]], undefined, « valuesArray »).
        return JS::call(vm, *m_capability->resolve(), js_undefined(), values_array);
    }

    // 15. Return undefined.
    return js_undefined();
}

NonnullGCPtr<PromiseAnyRejectElementFunction> PromiseAnyRejectElementFunction::create(Realm& realm, size_t index, PromiseValueList& errors, NonnullGCPtr<PromiseCapability const> capability, RemainingElements& remaining_elements)
{
    return realm.heap().allocate<PromiseAnyRejectElementFunction>(realm, index, errors, capability, remaining_elements, realm.intrinsics().function_prototype());
}

PromiseAnyRejectElementFunction::PromiseAnyRejectElementFunction(size_t index, PromiseValueList& errors, NonnullGCPtr<PromiseCapability const> capability, RemainingElements& remaining_elements, Object& prototype)
    : PromiseResolvingElementFunction(index, errors, capability, remaining_elements, prototype)
{
}

ThrowCompletionOr<Value> PromiseAnyRejectElementFunction::resolve_element()
{
    auto& vm = this->vm();
    auto& realm = *vm.current_realm();

    // 8. Set errors[index] to x.
    m_values->values()[m_index] = vm.argument(0);

    // 9. Set remainingElementsCount.[[Value]] to remainingElementsCount.[[Value]] - 1.
    // 10. If remainingElementsCount.[[Value]] is 0, then
    if (--m_remaining_elements->value == 0) {
        // a. Let error be a newly created AggregateError object.
        auto error = AggregateError::create(realm);

        // b. Perform ! DefinePropertyOrThrow(error, "errors", PropertyDescriptor { [[Configurable]]: true, [[Enumerable]]: false, [[Writable]]: true, [[Value]]: CreateArrayFromList(errors) }).
        auto errors_array = Array::create_from(realm, m_values->values());
        MUST(error->define_property_or_throw(vm.names.errors, { .value = errors_array, .writable = true, .enumerable = false, .configurable = true }));

        // c. Return ? Call(promiseCapability.[[Reject]], undefined, « error »).
        return JS::call(vm, *m_capability->reject(), js_undefined(), error);
    }

    return js_undefined();
}

}
