/*
 * Copyright (c) 2022-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2020-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/CellAllocator.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/Cell.h>

namespace JS {

class Intrinsics final : public Cell {
    GC_CELL(Intrinsics, Cell);
    GC_DECLARE_ALLOCATOR(Intrinsics);

public:
    static GC::Ref<Intrinsics> create(Realm&);

    GC::Ref<Shape> empty_object_shape() { return *m_empty_object_shape; }

    GC::Ref<Shape> new_object_shape() { return *m_new_object_shape; }

    [[nodiscard]] GC::Ref<Shape> iterator_result_object_shape() { return *m_iterator_result_object_shape; }
    [[nodiscard]] u32 iterator_result_object_value_offset() { return m_iterator_result_object_value_offset; }
    [[nodiscard]] u32 iterator_result_object_done_offset() { return m_iterator_result_object_done_offset; }

    [[nodiscard]] GC::Ref<Shape> normal_function_prototype_shape() { return *m_normal_function_prototype_shape; }
    [[nodiscard]] u32 normal_function_prototype_constructor_offset() const { return m_normal_function_prototype_constructor_offset; }

    [[nodiscard]] GC::Ref<Shape> normal_function_shape() { return *m_normal_function_shape; }
    [[nodiscard]] u32 normal_function_length_offset() const { return m_normal_function_length_offset; }
    [[nodiscard]] u32 normal_function_name_offset() const { return m_normal_function_name_offset; }
    [[nodiscard]] u32 normal_function_prototype_offset() const { return m_normal_function_prototype_offset; }

    // Not included in JS_ENUMERATE_NATIVE_OBJECTS due to missing distinct prototype
    GC::Ref<ProxyConstructor> proxy_constructor() { return *m_proxy_constructor; }

    // Not included in JS_ENUMERATE_NATIVE_OBJECTS due to missing distinct constructor
    GC::Ref<Object> async_from_sync_iterator_prototype() { return *m_async_from_sync_iterator_prototype; }
    GC::Ref<Object> async_generator_prototype() { return *m_async_generator_prototype; }
    GC::Ref<Object> generator_prototype() { return *m_generator_prototype; }
    GC::Ref<Object> wrap_for_valid_iterator_prototype() { return *m_wrap_for_valid_iterator_prototype; }

    // Alias for the AsyncGenerator Prototype Object used by the spec (%AsyncGeneratorFunction.prototype.prototype%)
    GC::Ref<Object> async_generator_function_prototype_prototype() { return *m_async_generator_prototype; }
    // Alias for the Generator Prototype Object used by the spec (%GeneratorFunction.prototype.prototype%)
    GC::Ref<Object> generator_function_prototype_prototype() { return *m_generator_prototype; }

    // Not included in JS_ENUMERATE_INTL_OBJECTS due to missing distinct constructor
    GC::Ref<Object> intl_segments_prototype() { return *m_intl_segments_prototype; }

    // Global object functions
    GC::Ref<FunctionObject> eval_function() const { return *m_eval_function; }
    GC::Ref<FunctionObject> is_finite_function() const { return *m_is_finite_function; }
    GC::Ref<FunctionObject> is_nan_function() const { return *m_is_nan_function; }
    GC::Ref<FunctionObject> parse_float_function() const { return *m_parse_float_function; }
    GC::Ref<FunctionObject> parse_int_function() const { return *m_parse_int_function; }
    GC::Ref<FunctionObject> decode_uri_function() const { return *m_decode_uri_function; }
    GC::Ref<FunctionObject> decode_uri_component_function() const { return *m_decode_uri_component_function; }
    GC::Ref<FunctionObject> encode_uri_function() const { return *m_encode_uri_function; }
    GC::Ref<FunctionObject> encode_uri_component_function() const { return *m_encode_uri_component_function; }
    GC::Ref<FunctionObject> escape_function() const { return *m_escape_function; }
    GC::Ref<FunctionObject> unescape_function() const { return *m_unescape_function; }

    // Namespace/constructor object functions
    GC::Ref<FunctionObject> array_prototype_values_function() const { return *m_array_prototype_values_function; }
    GC::Ref<FunctionObject> date_constructor_now_function() const { return *m_date_constructor_now_function; }
    GC::Ref<FunctionObject> json_parse_function() const { return *m_json_parse_function; }
    GC::Ref<FunctionObject> json_stringify_function() const { return *m_json_stringify_function; }
    GC::Ref<FunctionObject> object_prototype_to_string_function() const { return *m_object_prototype_to_string_function; }
    GC::Ref<FunctionObject> throw_type_error_function() const { return *m_throw_type_error_function; }

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    GC::Ref<ConstructorName> snake_name##_constructor();                                 \
    GC::Ref<Object> snake_name##_prototype();
    JS_ENUMERATE_BUILTIN_TYPES
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName) \
    GC::Ref<Intl::ConstructorName> intl_##snake_name##_constructor();         \
    GC::Ref<Object> intl_##snake_name##_prototype();
    JS_ENUMERATE_INTL_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName) \
    GC::Ref<Temporal::ConstructorName> temporal_##snake_name##_constructor(); \
    GC::Ref<Object> temporal_##snake_name##_prototype();
    JS_ENUMERATE_TEMPORAL_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name) \
    GC::Ref<ClassName> snake_name##_object();
    JS_ENUMERATE_BUILTIN_NAMESPACE_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name) \
    GC::Ref<Object> snake_name##_prototype()  \
    {                                         \
        return *m_##snake_name##_prototype;   \
    }
    JS_ENUMERATE_ITERATOR_PROTOTYPES
#undef __JS_ENUMERATE

    [[nodiscard]] GC::Ref<Intl::Collator> default_collator();

private:
    Intrinsics(Realm& realm)
        : m_realm(realm)
    {
    }

    virtual void visit_edges(Visitor&) override;

    void initialize_intrinsics(Realm&);

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

    GC::Ref<Realm> m_realm;

    GC::Ptr<Shape> m_empty_object_shape;
    GC::Ptr<Shape> m_new_object_shape;

    GC::Ptr<Shape> m_iterator_result_object_shape;
    u32 m_iterator_result_object_value_offset { 0 };
    u32 m_iterator_result_object_done_offset { 0 };

    GC::Ptr<Shape> m_normal_function_prototype_shape;
    u32 m_normal_function_prototype_constructor_offset { 0 };

    GC::Ptr<Shape> m_normal_function_shape;
    u32 m_normal_function_length_offset { 0 };
    u32 m_normal_function_name_offset { 0 };
    u32 m_normal_function_prototype_offset { 0 };

    // Not included in JS_ENUMERATE_NATIVE_OBJECTS due to missing distinct prototype
    GC::Ptr<ProxyConstructor> m_proxy_constructor;

    // Not included in JS_ENUMERATE_NATIVE_OBJECTS due to missing distinct constructor
    GC::Ptr<Object> m_async_from_sync_iterator_prototype;
    GC::Ptr<Object> m_async_generator_prototype;
    GC::Ptr<Object> m_generator_prototype;
    GC::Ptr<Object> m_wrap_for_valid_iterator_prototype;

    // Not included in JS_ENUMERATE_INTL_OBJECTS due to missing distinct constructor
    GC::Ptr<Object> m_intl_segments_prototype;

    // Global object functions
    GC::Ptr<FunctionObject> m_eval_function;
    GC::Ptr<FunctionObject> m_is_finite_function;
    GC::Ptr<FunctionObject> m_is_nan_function;
    GC::Ptr<FunctionObject> m_parse_float_function;
    GC::Ptr<FunctionObject> m_parse_int_function;
    GC::Ptr<FunctionObject> m_decode_uri_function;
    GC::Ptr<FunctionObject> m_decode_uri_component_function;
    GC::Ptr<FunctionObject> m_encode_uri_function;
    GC::Ptr<FunctionObject> m_encode_uri_component_function;
    GC::Ptr<FunctionObject> m_escape_function;
    GC::Ptr<FunctionObject> m_unescape_function;

    // Namespace/constructor object functions
    GC::Ptr<FunctionObject> m_array_prototype_values_function;
    GC::Ptr<FunctionObject> m_date_constructor_now_function;
    GC::Ptr<FunctionObject> m_json_parse_function;
    GC::Ptr<FunctionObject> m_json_stringify_function;
    GC::Ptr<FunctionObject> m_object_prototype_to_string_function;
    GC::Ptr<FunctionObject> m_throw_type_error_function;

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) \
    GC::Ptr<ConstructorName> m_##snake_name##_constructor;                               \
    GC::Ptr<Object> m_##snake_name##_prototype;
    JS_ENUMERATE_BUILTIN_TYPES
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName) \
    GC::Ptr<Intl::ConstructorName> m_intl_##snake_name##_constructor;         \
    GC::Ptr<Object> m_intl_##snake_name##_prototype;
    JS_ENUMERATE_INTL_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName) \
    GC::Ptr<Temporal::ConstructorName> m_temporal_##snake_name##_constructor; \
    GC::Ptr<Object> m_temporal_##snake_name##_prototype;
    JS_ENUMERATE_TEMPORAL_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name) \
    GC::Ptr<ClassName> m_##snake_name##_object;
    JS_ENUMERATE_BUILTIN_NAMESPACE_OBJECTS
#undef __JS_ENUMERATE

#define __JS_ENUMERATE(ClassName, snake_name) \
    GC::Ptr<Object> m_##snake_name##_prototype;
    JS_ENUMERATE_ITERATOR_PROTOTYPES
#undef __JS_ENUMERATE

    GC::Ptr<Intl::Collator> m_default_collator;
};

void add_restricted_function_properties(FunctionObject&, Realm&);

}
