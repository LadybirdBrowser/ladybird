/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Runtime/Object.h>

namespace JS {

class JSONObject final : public Object {
    JS_OBJECT(JSONObject, Object);
    GC_DECLARE_ALLOCATOR(JSONObject);

public:
    virtual void initialize(Realm&) override;
    virtual ~JSONObject() override = default;

    // The base implementation of stringify is exposed because it is used by
    // test-js to communicate between the JS tests and the C++ test runner.
    static ThrowCompletionOr<Optional<String>> stringify_impl(VM&, Value value, Value replacer, Value space);

    static Value parse_json_value(VM&, JsonValue const&);

private:
    explicit JSONObject(Realm&);

    struct StringifyState {
        GC::Ptr<FunctionObject> replacer_function;
        HashTable<GC::Ptr<Object>> seen_objects;
        String indent;
        String gap;
        Optional<Vector<String>> property_list;
    };

    // Stringify helpers
    static ThrowCompletionOr<Optional<String>> serialize_json_property(VM&, StringifyState&, PropertyKey const& key, Object* holder);
    static ThrowCompletionOr<String> serialize_json_object(VM&, StringifyState&, Object&);
    static ThrowCompletionOr<String> serialize_json_array(VM&, StringifyState&, Object&);
    static String quote_json_string(String);

    // Parse helpers
    static Object* parse_json_object(VM&, JsonObject const&);
    static Array* parse_json_array(VM&, JsonArray const&);
    static ThrowCompletionOr<Value> internalize_json_property(VM&, Object* holder, PropertyKey const& name, FunctionObject& reviver);

    JS_DECLARE_NATIVE_FUNCTION(stringify);
    JS_DECLARE_NATIVE_FUNCTION(parse);
};

}
