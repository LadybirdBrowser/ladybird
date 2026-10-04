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

    static JS::ThrowCompletionOr<bool> set(JS::HostArray& array, JS::PropertyKey const& property_key, JS::Value value, JS::Value receiver, JS::CacheableSetPropertyMetadata* metadata, JS::Object::PropertyLookupPhase phase)
    {
        if (property_key.is_number())
            TRY(observable_array_of(array).run_set_an_indexed_value_callback(property_key.as_number(), value));
        return array.array_set(property_key, value, receiver, metadata, phase);
    }

    static JS::ThrowCompletionOr<bool> delete_property(JS::HostArray& array, JS::PropertyKey const& property_key)
    {
        if (property_key.is_number())
            TRY(observable_array_of(array).run_delete_an_indexed_value_callback(property_key.as_number()));
        return array.array_delete(property_key);
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
    visitor.visit(m_on_set_an_indexed_value);
    visitor.visit(m_on_delete_an_indexed_value);
}

void ObservableArray::set_on_set_an_indexed_value_callback(SetAnIndexedValueCallbackFunction&& callback)
{
    m_on_set_an_indexed_value = GC::create_function(GC::Heap::the(), move(callback));
}

void ObservableArray::set_on_delete_an_indexed_value_callback(DeleteAnIndexedValueCallbackFunction&& callback)
{
    m_on_delete_an_indexed_value = GC::create_function(GC::Heap::the(), move(callback));
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
    auto deleted_value = element_value(index).value_or(JS::js_undefined());
    TRY(WebIDL::throw_dom_exception_if_needed(m_realm->vm(), m_realm, [&] { return m_on_delete_an_indexed_value->function()(deleted_value); }));
    return {};
}

JS::ThrowCompletionOr<void> ObservableArray::append(JS::Value value)
{
    TRY(run_set_an_indexed_value_callback(length(), value));
    // Unlike CreateDataProperty, this also appends to an array that script has made non-extensible or frozen.
    m_array_object->indexed_append(value);
    return {};
}

void ObservableArray::clear()
{
    // Each element leaves the array before its callback runs, and the callback may read the elements that remain.
    // Unlike [[Delete]], taking an element ignores any attributes that script has given it.
    while (length() != 0) {
        auto deleted_value = element_value(0).value_or(JS::js_undefined());
        m_array_object->indexed_take_first();
        MUST(m_on_delete_an_indexed_value->function()(deleted_value));
    }
}

}
