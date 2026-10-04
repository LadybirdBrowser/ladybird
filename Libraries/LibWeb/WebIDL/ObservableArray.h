/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Optional.h>
#include <LibGC/CellAllocator.h>
#include <LibGC/Function.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/Value.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebIDL {

// https://webidl.spec.whatwg.org/#idl-observable-array
// Script sees an observable array as an Array exotic object of its own host class, whose [[Set]] and [[Delete]] run
// the callbacks kept in this companion cell before doing what an Array does.
class WEB_API ObservableArray final : public JS::Cell {
    GC_CELL(ObservableArray, JS::Cell);
    GC_DECLARE_ALLOCATOR(ObservableArray);

public:
    using JSValueConversionIsForbidden = void;

    static GC::Ref<ObservableArray> create(JS::Realm&);

    using SetAnIndexedValueCallbackFunction = Function<ExceptionOr<void>(u32 index, JS::Value&)>;
    using DeleteAnIndexedValueCallbackFunction = Function<ExceptionOr<void>(JS::Value)>;

    void set_on_set_an_indexed_value_callback(SetAnIndexedValueCallbackFunction&& callback);
    void set_on_delete_an_indexed_value_callback(DeleteAnIndexedValueCallbackFunction&& callback);

    GC::Ref<JS::Object> array_object() const { return m_array_object; }

    u32 length() const;

    // The value of the element at index, which is nothing for a hole, an accessor or an index past the end. Reading it
    // never runs script.
    Optional<JS::Value> element_value(u32 index) const;

    template<typename Callback>
    void for_each_element_value(Callback callback) const
    {
        for (u32 index = 0; index < length(); ++index) {
            if (auto value = element_value(index); value.has_value())
                callback(*value);
        }
    }

    JS::ThrowCompletionOr<void> append(JS::Value value);
    void clear();

private:
    friend struct ObservableArrayHostClassTraits;

    ObservableArray(JS::Realm&, JS::Object& array_object);

    virtual void visit_edges(JS::Cell::Visitor&) override;

    JS::ThrowCompletionOr<void> run_set_an_indexed_value_callback(u32 index, JS::Value& value);
    JS::ThrowCompletionOr<void> run_delete_an_indexed_value_callback(u32 index);

    using SetAnIndexedValueCallbackHeapFunction = GC::Function<SetAnIndexedValueCallbackFunction::FunctionType>;
    using DeleteAnIndexedValueCallbackHeapFunction = GC::Function<DeleteAnIndexedValueCallbackFunction::FunctionType>;

    GC::Ref<JS::Realm> m_realm;
    GC::Ref<JS::Object> m_array_object;
    GC::Ptr<SetAnIndexedValueCallbackHeapFunction> m_on_set_an_indexed_value;
    GC::Ptr<DeleteAnIndexedValueCallbackHeapFunction> m_on_delete_an_indexed_value;
};

}
