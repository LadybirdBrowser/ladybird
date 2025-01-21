/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Array.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::WebIDL {

// https://webidl.spec.whatwg.org/#idl-observable-array
class ObservableArray final : public JS::Array {
    JS_OBJECT(ObservableArray, JS::Array);
    GC_DECLARE_ALLOCATOR(ObservableArray);

public:
    static GC::Ref<ObservableArray> create(JS::Realm& realm);

    virtual JS::ThrowCompletionOr<bool> internal_set(JS::PropertyKey const& property_key, JS::Value value, JS::Value receiver, JS::CacheablePropertyMetadata* metadata = nullptr) override;
    virtual JS::ThrowCompletionOr<bool> internal_delete(JS::PropertyKey const& property_key) override;

    using SetAnIndexedValueCallbackFunction = Function<ExceptionOr<void>(JS::Value&)>;
    using DeleteAnIndexedValueCallbackFunction = Function<ExceptionOr<void>(JS::Value)>;

    void set_on_set_an_indexed_value_callback(SetAnIndexedValueCallbackFunction&& callback);
    void set_on_delete_an_indexed_value_callback(DeleteAnIndexedValueCallbackFunction&& callback);

    JS::ThrowCompletionOr<void> append(JS::Value value);
    void clear();

    template<typename T, typename Callback>
    void for_each(Callback callback)
    {
        for (auto& entry : indexed_properties()) {
            auto value_and_attributes = indexed_properties().storage()->get(entry.index());
            if (value_and_attributes.has_value()) {
                auto& style_sheet = as<T>(value_and_attributes->value.as_object());
                callback(style_sheet);
            }
        }
    }

    explicit ObservableArray(Object& prototype);

    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    using SetAnIndexedValueCallbackHeapFunction = GC::Function<SetAnIndexedValueCallbackFunction::FunctionType>;
    using DeleteAnIndexedValueCallbackHeapFunction = GC::Function<DeleteAnIndexedValueCallbackFunction::FunctionType>;

    GC::Ptr<SetAnIndexedValueCallbackHeapFunction> m_on_set_an_indexed_value;
    GC::Ptr<DeleteAnIndexedValueCallbackHeapFunction> m_on_delete_an_indexed_value;
};

}
