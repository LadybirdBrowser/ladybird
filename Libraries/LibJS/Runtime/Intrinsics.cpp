/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/EmbeddingABIConversions.h>
#include <LibJS/Runtime/ConsoleObject.h>
#include <LibJS/Runtime/DataViewConstructor.h>
#include <LibJS/Runtime/ErrorConstructor.h>
#include <LibJS/Runtime/FunctionObject.h>
#include <LibJS/Runtime/Intrinsics.h>
#include <LibJS/Runtime/PromiseConstructor.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/SharedArrayBufferConstructor.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>

namespace JS {

using namespace EmbeddingABI;

enum class TypedArrayIntrinsicIndex : JSIntrinsic {
#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType) ClassName,
    JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE
};

// The runtime numbers the typed array constructors in the order of JS_ENUMERATE_TYPED_ARRAYS.
static_assert(JS_INTRINSIC_UINT8_CLAMPED_ARRAY_CONSTRUCTOR == JS_INTRINSIC_UINT8_ARRAY_CONSTRUCTOR + to_underlying(TypedArrayIntrinsicIndex::Uint8ClampedArray));
static_assert(JS_INTRINSIC_BIG_UINT64_ARRAY_CONSTRUCTOR == JS_INTRINSIC_UINT8_ARRAY_CONSTRUCTOR + to_underlying(TypedArrayIntrinsicIndex::BigUint64Array));
static_assert(JS_INTRINSIC_INT8_ARRAY_CONSTRUCTOR == JS_INTRINSIC_UINT8_ARRAY_CONSTRUCTOR + to_underlying(TypedArrayIntrinsicIndex::Int8Array));
static_assert(JS_INTRINSIC_BIG_INT64_ARRAY_CONSTRUCTOR == JS_INTRINSIC_UINT8_ARRAY_CONSTRUCTOR + to_underlying(TypedArrayIntrinsicIndex::BigInt64Array));
static_assert(JS_INTRINSIC_FLOAT16_ARRAY_CONSTRUCTOR == JS_INTRINSIC_UINT8_ARRAY_CONSTRUCTOR + to_underlying(TypedArrayIntrinsicIndex::Float16Array));
static_assert(JS_INTRINSIC_FLOAT64_ARRAY_CONSTRUCTOR == JS_INTRINSIC_UINT8_ARRAY_CONSTRUCTOR + to_underlying(TypedArrayIntrinsicIndex::Float64Array));

template<typename T>
static GC::Ref<T> intrinsic_of_realm(Realm& realm, JSIntrinsic intrinsic)
{
    auto* object = js_realm_intrinsic(vm_to_abi(realm.vm()), cell_to_abi<JSRealm>(realm), intrinsic);
    VERIFY(object);
    return cell_ref_from_abi<T>(object);
}

Realm& Intrinsics::realm() const
{
    return *reinterpret_cast<Realm*>(const_cast<Intrinsics*>(this));
}

GC::Ref<Object> Intrinsics::object_prototype() { return intrinsic_of_realm<Object>(realm(), JS_INTRINSIC_OBJECT_PROTOTYPE); }
GC::Ref<Object> Intrinsics::function_prototype() { return intrinsic_of_realm<Object>(realm(), JS_INTRINSIC_FUNCTION_PROTOTYPE); }
GC::Ref<Object> Intrinsics::array_prototype() { return intrinsic_of_realm<Object>(realm(), JS_INTRINSIC_ARRAY_PROTOTYPE); }
GC::Ref<Object> Intrinsics::string_prototype() { return intrinsic_of_realm<Object>(realm(), JS_INTRINSIC_STRING_PROTOTYPE); }
GC::Ref<Object> Intrinsics::error_prototype() { return intrinsic_of_realm<Object>(realm(), JS_INTRINSIC_ERROR_PROTOTYPE); }
GC::Ref<Object> Intrinsics::iterator_prototype() { return intrinsic_of_realm<Object>(realm(), JS_INTRINSIC_ITERATOR_PROTOTYPE); }
GC::Ref<Object> Intrinsics::async_iterator_prototype() { return intrinsic_of_realm<Object>(realm(), JS_INTRINSIC_ASYNC_ITERATOR_PROTOTYPE); }

GC::Ref<ErrorConstructor> Intrinsics::error_constructor() { return intrinsic_of_realm<ErrorConstructor>(realm(), JS_INTRINSIC_ERROR_CONSTRUCTOR); }
GC::Ref<PromiseConstructor> Intrinsics::promise_constructor() { return intrinsic_of_realm<PromiseConstructor>(realm(), JS_INTRINSIC_PROMISE_CONSTRUCTOR); }
GC::Ref<SharedArrayBufferConstructor> Intrinsics::shared_array_buffer_constructor() { return intrinsic_of_realm<SharedArrayBufferConstructor>(realm(), JS_INTRINSIC_SHARED_ARRAY_BUFFER_CONSTRUCTOR); }
GC::Ref<DataViewConstructor> Intrinsics::data_view_constructor() { return intrinsic_of_realm<DataViewConstructor>(realm(), JS_INTRINSIC_DATA_VIEW_CONSTRUCTOR); }

#define __JS_ENUMERATE(ClassName, snake_name, PrototypeName, ConstructorName, ArrayType)                                                                \
    GC::Ref<ConstructorName> Intrinsics::snake_name##_constructor()                                                                                     \
    {                                                                                                                                                   \
        return intrinsic_of_realm<ConstructorName>(realm(), JS_INTRINSIC_UINT8_ARRAY_CONSTRUCTOR + to_underlying(TypedArrayIntrinsicIndex::ClassName)); \
    }
JS_ENUMERATE_TYPED_ARRAYS
#undef __JS_ENUMERATE

GC::Ref<FunctionObject> Intrinsics::json_stringify_function() const { return intrinsic_of_realm<FunctionObject>(realm(), JS_INTRINSIC_JSON_STRINGIFY_FUNCTION); }

GC::Ref<ConsoleObject> Intrinsics::console_object() { return intrinsic_of_realm<ConsoleObject>(realm(), JS_INTRINSIC_CONSOLE_OBJECT); }

}
