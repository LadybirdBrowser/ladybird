/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>

namespace JS {

class Intrinsics final : public Cell {
    JS_CELL(Intrinsics, Cell);
    JS_DECLARE_ALLOCATOR(Intrinsics);

public:
    static ThrowCompletionOr<NonnullGCPtr<Intrinsics>> create(Realm&);

    NonnullGCPtr<Shape> empty_object_shape() { return *m_empty_object_shape; }

    NonnullGCPtr<Shape> new_object_shape() { return *m_new_object_shape; }

    [[nodiscard]] NonnullGCPtr<Shape> iterator_result_object_shape() { return *m_iterator_result_object_shape; }
    [[nodiscard]] u32 iterator_result_object_value_offset() { return m_iterator_result_object_value_offset; }
    [[nodiscard]] u32 iterator_result_object_done_offset() { return m_iterator_result_object_done_offset; }

    // Not included in JS_ENUMERATE_NATIVE_OBJECTS due to missing distinct prototype
    NonnullGCPtr<ProxyConstructor> proxy_constructor() { return *m_proxy_constructor; }

    // Not included in JS_ENUMERATE_NATIVE_OBJECTS due to missing distinct constructor
    NonnullGCPtr<Object> async_from_sync_iterator_prototype() { return *m_async_from_sync_iterator_prototype; }
    NonnullGCPtr<Object> async_generator_prototype() { return *m_async_generator_prototype; }
    NonnullGCPtr<Object> generator_prototype() { return *m_generator_prototype; }
    NonnullGCPtr<Object> wrap_for_valid_iterator_prototype() { return *m_wrap_for_valid_iterator_prototype; }

    // Alias for the AsyncGenerator Prototype Object used by the spec (%AsyncGeneratorFunction.prototype.prototype%)
    NonnullGCPtr<Object> async_generator_function_prototype_prototype() { return *m_async_generator_prototype; }
    // Alias for the Generator Prototype Object used by the spec (%GeneratorFunction.prototype.prototype%)
    NonnullGCPtr<Object> generator_function_prototype_prototype() { return *m_generator_prototype; }

    // Not included in JS_ENUMERATE_INTL_OBJECTS due to missing distinct constructor
    NonnullGCPtr<Object> intl_segments_prototype() { return *m_intl_segments_prototype; }

    // Global object functions
    NonnullGCPtr<FunctionObject> eval_function() const { return *m_eval_function; }
    NonnullGCPtr<FunctionObject> is_finite_function() const { return *m_is_finite_function; }
    NonnullGCPtr<FunctionObject> is_nan_function() const { return *m_is_nan_function; }
    NonnullGCPtr<FunctionObject> parse_float_function() const { return *m_parse_float_function; }
    NonnullGCPtr<FunctionObject> parse_int_function() const { return *m_parse_int_function; }
    NonnullGCPtr<FunctionObject> decode_uri_function() const { return *m_decode_uri_function; }
    NonnullGCPtr<FunctionObject> decode_uri_component_function() const { return *m_decode_uri_component_function; }
    NonnullGCPtr<FunctionObject> encode_uri_function() const { return *m_encode_uri_function; }
    NonnullGCPtr<FunctionObject> encode_uri_component_function() const { return *m_encode_uri_component_function; }
    NonnullGCPtr<FunctionObject> escape_function() const { return *m_escape_function; }
    NonnullGCPtr<FunctionObject> unescape_function() const { return *m_unescape_function; }

    // Namespace/constructor object functions
    NonnullGCPtr<FunctionObject> array_prototype_values_function() const { return *m_array_prototype_values_function; }
    NonnullGCPtr<FunctionObject> date_constructor_now_function() const { return *m_date_constructor_now_function; }
    NonnullGCPtr<FunctionObject> json_parse_function() const { return *m_json_parse_function; }
    NonnullGCPtr<FunctionObject> json_stringify_function() const { return *m_json_stringify_function; }
    NonnullGCPtr<FunctionObject> object_prototype_to_string_function() const { return *m_object_prototype_to_string_function; }
    NonnullGCPtr<FunctionObject> throw_type_error_function() const { return *m_throw_type_error_function; }

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    NonnullGCPtr<ConstructorName> snake_name##_constructor();                            \
    NonnullGCPtr<Object> snake_name##_prototype();
    JS_ENUMERATE_BUILTIN_TYPES
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName) \
    NonnullGCPtr<Intl::ConstructorName> intl_##snake_name##_constructor();    \
    NonnullGCPtr<Object> intl_##snake_name##_prototype();
    JS_ENUMERATE_INTL_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName)      \
    NonnullGCPtr<Temporal::ConstructorName> temporal_##snake_name##_constructor(); \
    NonnullGCPtr<Object> temporal_##snake_name##_prototype();
    JS_ENUMERATE_TEMPORAL_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name) \
    NonnullGCPtr<ClassName> snake_name##_object();
    JS_ENUMERATE_BUILTIN_NAMESPACE_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name)     \
    NonnullGCPtr<Object> snake_name##_prototype() \
    {                                             \
        return *m_##snake_name##_prototype;       \
    }
    JS_ENUMERATE_ITERATOR_PROTOTYPES
#undef __JS_ENUMERATE

private:
    Intrinsics(Realm& realm)
        : m_realm(realm)
    {
    }

    virtual void visit_edges(Visitor&) override;

    ThrowCompletionOr<void> initialize_intrinsics(Realm&);

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    void initialize_##snake_name();
    JS_ENUMERATE_BUILTIN_TYPES
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName) \
    void initialize_intl_##snake_name();
    JS_ENUMERATE_INTL_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName) \
    void initialize_temporal_##snake_name();
    JS_ENUMERATE_TEMPORAL_OBJECTS
#undef __JS_ENUMERATE

    NonnullGCPtr<Realm> m_realm;

    GCPtr<Shape> m_empty_object_shape;
    GCPtr<Shape> m_new_object_shape;

    GCPtr<Shape> m_iterator_result_object_shape;
    u32 m_iterator_result_object_value_offset { 0 };
    u32 m_iterator_result_object_done_offset { 0 };

    // Not included in JS_ENUMERATE_NATIVE_OBJECTS due to missing distinct prototype
    GCPtr<ProxyConstructor> m_proxy_constructor;

    // Not included in JS_ENUMERATE_NATIVE_OBJECTS due to missing distinct constructor
    GCPtr<Object> m_async_from_sync_iterator_prototype;
    GCPtr<Object> m_async_generator_prototype;
    GCPtr<Object> m_generator_prototype;
    GCPtr<Object> m_wrap_for_valid_iterator_prototype;

    // Not included in JS_ENUMERATE_INTL_OBJECTS due to missing distinct constructor
    GCPtr<Object> m_intl_segments_prototype;

    // Global object functions
    GCPtr<FunctionObject> m_eval_function;
    GCPtr<FunctionObject> m_is_finite_function;
    GCPtr<FunctionObject> m_is_nan_function;
    GCPtr<FunctionObject> m_parse_float_function;
    GCPtr<FunctionObject> m_parse_int_function;
    GCPtr<FunctionObject> m_decode_uri_function;
    GCPtr<FunctionObject> m_decode_uri_component_function;
    GCPtr<FunctionObject> m_encode_uri_function;
    GCPtr<FunctionObject> m_encode_uri_component_function;
    GCPtr<FunctionObject> m_escape_function;
    GCPtr<FunctionObject> m_unescape_function;

    // Namespace/constructor object functions
    GCPtr<FunctionObject> m_array_prototype_values_function;
    GCPtr<FunctionObject> m_date_constructor_now_function;
    GCPtr<FunctionObject> m_json_parse_function;
    GCPtr<FunctionObject> m_json_stringify_function;
    GCPtr<FunctionObject> m_object_prototype_to_string_function;
    GCPtr<FunctionObject> m_throw_type_error_function;

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    GCPtr<ConstructorName> m_##snake_name##_constructor;                                 \
    GCPtr<Object> m_##snake_name##_prototype;
    JS_ENUMERATE_BUILTIN_TYPES
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName) \
    GCPtr<Intl::ConstructorName> m_intl_##snake_name##_constructor;           \
    GCPtr<Object> m_intl_##snake_name##_prototype;
    JS_ENUMERATE_INTL_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName) \
    GCPtr<Temporal::ConstructorName> m_temporal_##snake_name##_constructor;   \
    GCPtr<Object> m_temporal_##snake_name##_prototype;
    JS_ENUMERATE_TEMPORAL_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name) \
    GCPtr<ClassName> m_##snake_name##_object;
    JS_ENUMERATE_BUILTIN_NAMESPACE_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name) \
    GCPtr<Object> m_##snake_name##_prototype;
    JS_ENUMERATE_ITERATOR_PROTOTYPES
#undef __JS_ENUMERATE
};

void add_restricted_function_properties(FunctionObject&, Realm&);

}
