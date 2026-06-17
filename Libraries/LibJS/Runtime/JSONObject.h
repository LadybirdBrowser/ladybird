/*
 * Copyright (c) 2020, Matthew Olsson <mattco@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Export.h>
#include <LibJS/Runtime/Object.h>

namespace JS {

// A JSON Parse Record is a Record value used to describe the initial state of a
// value parsed from JSON text. https://tc39.es/ecma262/#sec-json-parse-record
struct JSONParseRecord {
    // [[Value]]: the value produced by evaluation of [[ParseNode]].
    Value value;
    // [[Key]]: the property name with which [[Value]] is associated.
    Utf16String key;
    // NB: In place of the spec's [[ParseNode]] field, we capture the source text
    //     matched by the parse node up front. It is present only when [[Value]] is
    //     a primitive, since the reviver context only exposes "source" for those.
    Optional<String> source;
    // [[Elements]]: if [[Value]] is an Array, the records for its elements; else empty.
    Vector<JSONParseRecord> elements;
    // [[Entries]]: if [[Value]] is a non-Array Object, the records for its entries; else empty.
    Vector<JSONParseRecord> entries;
};

class JS_API JSONObject final : public Object {
    JS_OBJECT(JSONObject, Object);
    GC_DECLARE_ALLOCATOR(JSONObject);

public:
    virtual void initialize(Realm&) override;
    virtual ~JSONObject() override = default;

    // The base implementation of stringify is exposed because it is used by
    // test-js to communicate between the JS tests and the C++ test runner.
    static ThrowCompletionOr<Optional<String>> stringify_impl(VM&, Value value, Value replacer, Value space);

    static ThrowCompletionOr<Value> parse_json(VM&, StringView text, JSONParseRecord* root_record = nullptr);

private:
    explicit JSONObject(Realm&);

    struct StringifyState {
        GC::Ptr<FunctionObject> replacer_function;
        HashTable<GC::Ptr<Object>> seen_objects;
        size_t indent_depth { 0 };
        String gap;
        Optional<Vector<Utf16String>> property_list;
        StringBuilder builder;
    };

    // Stringify helpers
    static ThrowCompletionOr<bool> serialize_json_property(VM&, StringifyState&, PropertyKey const& key, Object* holder);
    static ThrowCompletionOr<void> serialize_json_object(VM&, StringifyState&, Object&);
    static ThrowCompletionOr<void> serialize_json_array(VM&, StringifyState&, Object&);
    static void quote_json_string(StringBuilder&, Utf16View const&);

    // Parse helpers
    static ThrowCompletionOr<Value> internalize_json_property(VM&, Object* holder, PropertyKey const& name, FunctionObject& reviver, JSONParseRecord const* parse_record);

    JS_DECLARE_NATIVE_FUNCTION(stringify);
    JS_DECLARE_NATIVE_FUNCTION(parse);
    JS_DECLARE_NATIVE_FUNCTION(raw_json);
    JS_DECLARE_NATIVE_FUNCTION(is_raw_json);
};

}
