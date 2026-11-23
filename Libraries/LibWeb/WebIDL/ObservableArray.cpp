/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/HostArray.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/WebIDL/ExceptionOrUtils.h>
#include <LibWeb/WebIDL/ObservableArray.h>

namespace Web::WebIDL {

GC_DEFINE_ALLOCATOR(ObservableArray);

struct ObservableArrayHostClassTraits {
    static ObservableArray& observable_array_of(JS::HostArray const& array)
    {
        auto* observable_array = JS::host_data_if<ObservableArray>(array);
        VERIFY(observable_array);
        return *observable_array;
    }

    // Object.preventExtensions() can make the backing array refuse a store. Check before running the callbacks.
    static bool can_store(JS::HostArray& array, JS::PropertyKey const& property_key)
    {
        auto descriptor = MUST(array.internal_get_own_property(property_key));
        if (!descriptor.has_value())
            return array.extensible();
        return descriptor->is_data_descriptor() && descriptor->writable.value_or(false);
    }

    static bool can_remove(JS::HostArray& array, u32 index)
    {
        auto descriptor = MUST(array.internal_get_own_property(index));
        return !descriptor.has_value() || descriptor->configurable.value_or(false);
    }

    // https://webidl.spec.whatwg.org/#js-observable-array-defineProperty
    static JS::ThrowCompletionOr<bool> define_own_property(JS::HostArray& array, JS::PropertyKey const& property_key, JS::PropertyDescriptor& descriptor, Optional<JS::PropertyDescriptor>* precomputed_get_own_property)
    {
        // 1. Let handler be the this value.
        // 2. Let descriptor be ! ToPropertyDescriptor(descriptorObj).
        // NB: LibJS passes the host array and the converted property descriptor to this hook.

        // 3. If P is "length", then:
        if (property_key == array.vm().names.length) {
            // 1. If IsAccessorDescriptor(descriptor) is true, then return false.
            if (descriptor.is_accessor_descriptor())
                return false;

            // 2. If descriptor.[[Configurable]] is present and has the value true, then return false.
            if (descriptor.configurable.value_or(false))
                return false;

            // 3. If descriptor.[[Enumerable]] is present and has the value true, then return false.
            if (descriptor.enumerable.value_or(false))
                return false;

            // 4. If descriptor.[[Writable]] is present and has the value false, then return false.
            if (!descriptor.writable.value_or(true))
                return false;

            // 5. If descriptor.[[Value]] is present, then return the result of setting the length given handler and descriptor.[[Value]].
            if (descriptor.value.has_value())
                return set_the_length(array, *descriptor.value);

            // 6. Return true.
            return true;
        }

        // 4. If P is an array index, then:
        if (property_key.is_number()) {
            // 1. If IsAccessorDescriptor(descriptor) is true, then return false.
            if (descriptor.is_accessor_descriptor())
                return false;

            // 2. If descriptor.[[Configurable]] is present and has the value false, then return false.
            if (!descriptor.configurable.value_or(true))
                return false;

            // 3. If descriptor.[[Enumerable]] is present and has the value false, then return false.
            if (!descriptor.enumerable.value_or(true))
                return false;

            // 4. If descriptor.[[Writable]] is present and has the value false, then return false.
            if (!descriptor.writable.value_or(true))
                return false;

            // 5. If descriptor.[[Value]] is present, then return the result of setting the indexed value given handler, P, and descriptor.[[Value]].
            if (descriptor.value.has_value())
                return set_the_indexed_value(array, property_key.as_number(), *descriptor.value);

            // 6. Return true.
            return true;
        }

        // 5. Return ? O.[[DefineOwnProperty]](P, descriptor).
        return array.array_define_own_property(property_key, descriptor, precomputed_get_own_property);
    }

    // https://webidl.spec.whatwg.org/#js-observable-array-set
    static JS::ThrowCompletionOr<bool> set(JS::HostArray& array, JS::PropertyKey const& property_key, JS::Value value, JS::Value receiver, JS::CacheableSetPropertyMetadata* metadata, JS::Object::PropertyLookupPhase phase)
    {
        auto& vm = array.vm();

        // 2. If P is "length", then return the result of setting the length given handler and V.
        if (property_key == vm.names.length)
            return set_the_length(array, value);

        // 3. If P is an array index, then return the result of setting the indexed value given handler, P, and V.
        if (property_key.is_number())
            return set_the_indexed_value(array, property_key.as_number(), value);

        // 4. Return ? O.[[Set]](P, V, Receiver).
        return array.array_set(property_key, value, receiver, metadata, phase);
    }

    // https://webidl.spec.whatwg.org/#js-observable-array-deleteProperty
    static JS::ThrowCompletionOr<bool> delete_property(JS::HostArray& array, JS::PropertyKey const& property_key)
    {
        auto& vm = array.vm();
        auto& observable_array = observable_array_of(array);

        // 2. If P is "length", then return false.
        if (property_key == vm.names.length)
            return false;

        // 3. If P is an array index, then:
        if (property_key.is_number()) {
            // 1. Let oldLen be handler.[[BackingList]]'s size.
            auto old_length = observable_array.length();

            // 2. Let index be ! ToUint32(P).
            auto index = property_key.as_number();

            // 3. If index ≠ oldLen − 1, then return false.
            if (old_length == 0 || index != old_length - 1)
                return false;

            if (!can_store(array, vm.names.length) || !can_remove(array, index))
                return false;

            // 4. Perform the algorithm steps given by handler.[[DeleteAlgorithm]], given handler.[[BackingList]][index]
            //    and index.
            TRY(observable_array.run_delete_an_indexed_value_callback(index));

            // 5. Remove the last item from handler.[[BackingList]].
            // 6. Return true.
            JS::PropertyDescriptor descriptor { .value = JS::Value(index) };
            if (!TRY(array.array_define_own_property(vm.names.length, descriptor)))
                return false;
            observable_array.did_change_backing_list();
            return true;
        }

        // 4. Return ? O.[[Delete]](P).
        return array.array_delete(property_key);
    }

    // https://webidl.spec.whatwg.org/#observable-array-exotic-object-set-the-length
    static JS::ThrowCompletionOr<bool> set_the_length(JS::HostArray& array, JS::Value new_length)
    {
        auto& vm = array.vm();
        auto& observable_array = observable_array_of(array);

        // 1. Let uint32Len be ? ToUint32(newLen).
        auto uint32_length = TRY(new_length.to_u32(vm));

        // 2. Let numberLen be ? ToNumber(newLen).
        auto number_length = TRY(new_length.to_double(vm));

        // 3. If uint32Len ≠ numberLen, then throw a RangeError exception.
        if (uint32_length != number_length)
            return vm.throw_completion<JS::RangeError>(JS::ErrorType::InvalidLength, "array");

        // 4. Let oldLen be handler.[[BackingList]]'s size.
        auto old_length = observable_array.length();

        // 5. If uint32Len > oldLen, then return false.
        if (uint32_length > old_length)
            return false;

        if (!can_store(array, vm.names.length))
            return false;

        // 6. Let indexToDelete be oldLen − 1.
        // 7. While indexToDelete ≥ uint32Len:
        for (auto index_to_delete = old_length; index_to_delete > uint32_length;) {
            --index_to_delete;

            if (!can_remove(array, index_to_delete))
                return false;

            // 1. Perform the algorithm steps given by handler.[[DeleteAlgorithm]], given
            //    handler.[[BackingList]][indexToDelete] and indexToDelete.
            TRY(observable_array.run_delete_an_indexed_value_callback(index_to_delete));

            // 2. Remove the last item from handler.[[BackingList]].
            JS::PropertyDescriptor descriptor { .value = JS::Value(index_to_delete) };
            if (!TRY(array.array_define_own_property(vm.names.length, descriptor)))
                return false;

            observable_array.did_change_backing_list();

            // 3. Set indexToDelete to indexToDelete − 1.
        }

        // 8. Return true.
        return true;
    }

    // https://webidl.spec.whatwg.org/#observable-array-exotic-object-set-the-indexed-value
    static JS::ThrowCompletionOr<bool> set_the_indexed_value(JS::HostArray& array, u32 index, JS::Value value)
    {
        auto& observable_array = observable_array_of(array);

        // 1. Let oldLen be handler.[[BackingList]]'s size.
        auto old_length = observable_array.length();

        // 2. Let index be ! ToUint32(P).
        // 3. If index > oldLen, return false.
        if (index > old_length)
            return false;

        if (!can_store(array, JS::PropertyKey { index }))
            return false;

        // NB: Appending also writes the length.
        if (index == old_length && !can_store(array, array.vm().names.length))
            return false;

        // 4. Let idlValue be the result of converting V to the type given by handler.[[Type]].
        TRY(observable_array.run_convert_to_idl_value_callback(value));

        // 5. If index < oldLen, then:
        if (index < old_length) {
            // 1. Perform the algorithm steps given by handler.[[DeleteAlgorithm]], given handler.[[BackingList]][index]
            //    and index.
            TRY(observable_array.run_delete_an_indexed_value_callback(index));
        }

        // 6. Perform the algorithm steps given by handler.[[SetAlgorithm]], given idlValue and index.
        TRY(observable_array.run_set_an_indexed_value_callback(index, value));

        // 7. If index = oldLen, then append idlValue to handler.[[BackingList]].
        // 8. Otherwise, set handler.[[BackingList]][index] to idlValue.
        // 9. Return true.
        // NB: [[Set]] would run a setter that script put on the prototype for a missing index, and leave the array
        //     without the element. The backing list takes the value directly, so define the element instead.
        JS::PropertyKey property_key { index };
        JS::PropertyDescriptor descriptor { .value = value };
        if (!MUST(array.internal_get_own_property(property_key)).has_value()) {
            descriptor.writable = true;
            descriptor.enumerable = true;
            descriptor.configurable = true;
        }
        if (!TRY(array.array_define_own_property(property_key, descriptor)))
            return false;
        observable_array.did_change_backing_list();
        return true;
    }
};

static constexpr JSHostArrayHooks observable_array_hooks = JS::make_host_array_hooks<ObservableArrayHostClassTraits>();
static constexpr JSHostClass observable_array_host_class = JS::make_host_class(JS_HOST_CLASS_ARRAY, "ObservableArray"sv, nullptr, &observable_array_hooks, nullptr, JS_HOST_CLASS_MAY_INTERFERE_WITH_INDEXED_PROPERTY_ACCESS);

GC::Ref<ObservableArray> ObservableArray::create(JS::Realm& realm)
{
    auto array_object = JS::HostArray::create(realm, observable_array_host_class);
    auto observable_array = realm.create<ObservableArray>(realm, array_object);
    array_object->set_host_data(observable_array);
    return observable_array;
}

ObservableArray::ObservableArray(JS::Realm& realm, JS::Object& array_object)
    : m_realm(realm)
    , m_array_object(array_object)
{
}

void ObservableArray::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_realm);
    visitor.visit(m_array_object);
    visitor.visit(m_convert_to_idl_value);
    visitor.visit(m_on_set_an_indexed_value);
    visitor.visit(m_on_delete_an_indexed_value);
    visitor.visit(m_on_backing_list_changed);
}

void ObservableArray::set_convert_to_idl_value_callback(GC::Ref<GC::Function<ConvertToIDLValueCallbackFunction::FunctionType>> callback)
{
    m_convert_to_idl_value = callback;
}

void ObservableArray::set_on_set_an_indexed_value_callback(GC::Ref<GC::Function<SetAnIndexedValueCallbackFunction::FunctionType>> callback)
{
    m_on_set_an_indexed_value = callback;
}

void ObservableArray::set_on_delete_an_indexed_value_callback(GC::Ref<GC::Function<DeleteAnIndexedValueCallbackFunction::FunctionType>> callback)
{
    m_on_delete_an_indexed_value = callback;
}

u32 ObservableArray::length() const
{
    return static_cast<u32>(MUST(JS::length_of_array_like(m_realm->vm(), m_array_object)));
}

Optional<JS::Value> ObservableArray::element_value(u32 index) const
{
    if (index >= length())
        return {};
    auto descriptor = MUST(m_array_object->internal_get_own_property(index));
    if (!descriptor.has_value())
        return {};
    return descriptor->value;
}

JS::ThrowCompletionOr<void> ObservableArray::run_convert_to_idl_value_callback(JS::Value value)
{
    if (!m_convert_to_idl_value)
        return {};
    TRY(WebIDL::throw_dom_exception_if_needed(m_realm->vm(), m_realm, [&] { return m_convert_to_idl_value->function()(value); }));
    return {};
}

JS::ThrowCompletionOr<void> ObservableArray::run_set_an_indexed_value_callback(u32 index, JS::Value& value)
{
    if (!m_on_set_an_indexed_value)
        return {};
    TRY(WebIDL::throw_dom_exception_if_needed(m_realm->vm(), m_realm, [&] { return m_on_set_an_indexed_value->function()(index, value); }));
    return {};
}

JS::ThrowCompletionOr<void> ObservableArray::run_delete_an_indexed_value_callback(u32 index)
{
    if (!m_on_delete_an_indexed_value)
        return {};
    auto deleted_value = element_value(index);
    if (!deleted_value.has_value())
        return {};
    TRY(WebIDL::throw_dom_exception_if_needed(m_realm->vm(), m_realm, [&] { return m_on_delete_an_indexed_value->function()(*deleted_value, index); }));
    return {};
}

void ObservableArray::did_change_backing_list()
{
    if (m_on_backing_list_changed)
        m_on_backing_list_changed->function()();
}

JS::ThrowCompletionOr<void> ObservableArray::append(JS::Value value)
{
    TRY(run_convert_to_idl_value_callback(value));
    TRY(run_set_an_indexed_value_callback(length(), value));
    // Unlike CreateDataProperty, this also appends to an array that script has made non-extensible or frozen.
    m_array_object->indexed_append(value);
    did_change_backing_list();
    return {};
}

void ObservableArray::clear()
{
    // Each callback runs while its element is still in the array, as in the deleteProperty trap, and may read the
    // elements that remain. Unlike [[Delete]], taking an element ignores any attributes that script has given it.
    while (length() != 0) {
        // NB: A hole holds no value to delete.
        if (auto deleted_value = element_value(0); deleted_value.has_value() && m_on_delete_an_indexed_value)
            MUST(m_on_delete_an_indexed_value->function()(*deleted_value, 0));
        m_array_object->indexed_take_first();
        did_change_backing_list();
    }
}

}
