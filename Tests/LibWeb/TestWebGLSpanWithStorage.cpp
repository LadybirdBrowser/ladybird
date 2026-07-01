/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/ByteBuffer.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

namespace {

class WebGLRenderingContextBaseAccessor : public Web::WebGL::WebGLRenderingContextBase {
    WEB_NON_IDL_PLATFORM_OBJECT(WebGLRenderingContextBaseAccessor, Web::WebGL::WebGLRenderingContextBase);

public:
    template<typename T>
    using SpanWithStorage = Web::WebGL::WebGLRenderingContextBase::SpanWithStorage<T>;

    using Web::WebGL::WebGLRenderingContextBase::span_from_float32_list;
    using Web::WebGL::WebGLRenderingContextBase::span_from_int32_list;
    using Web::WebGL::WebGLRenderingContextBase::span_from_uint32_list;

    template<typename T>
    static SpanWithStorage<T> make_span_with_storage(ByteBuffer storage)
    {
        return SpanWithStorage<T> { move(storage) };
    }

    template<typename T>
    static SpanWithStorage<T> make_span_with_storage(Span<T> span)
    {
        return SpanWithStorage<T> { span };
    }
};

struct TestVM {
    TestVM()
        : vm(JS::VM::create())
        , execution_context(MUST(JS::Realm::initialize_host_defined_realm(*vm, nullptr, nullptr)))
    {
    }

    ~TestVM()
    {
        vm->pop_execution_context();
    }

    NonnullRefPtr<JS::VM> vm;
    NonnullOwnPtr<JS::ExecutionContext> execution_context;
};

static ByteBuffer make_inline_u32_buffer(ReadonlySpan<u32> values)
{
    auto buffer = MUST(ByteBuffer::create_uninitialized(values.size() * sizeof(u32)));
    EXPECT(buffer.is_inline());
    buffer.overwrite(0, values.data(), values.size() * sizeof(u32));
    return buffer;
}

template<typename ArrayType>
static GC::Ref<ArrayType> create_out_of_bounds_array(JS::Realm& realm)
{
    auto array_buffer = MUST(JS::ArrayBuffer::create(realm, 32));
    array_buffer->set_max_byte_length(32);
    MUST(array_buffer->try_resize(4));

    auto typed_array = ArrayType::create(realm, 0, array_buffer);
    typed_array->set_viewed_array_buffer(array_buffer.ptr());
    typed_array->set_array_length(4);
    typed_array->set_byte_length(4 * typed_array->element_size());
    typed_array->set_byte_offset(16);

    return typed_array;
}

}

TEST_CASE(owning_span_rebuilds_storage_after_move_construction)
{
    Array values { 0x10203040u, 0x50607080u, 0x90a0b0c0u, 0xd0e0f000u };
    auto original = WebGLRenderingContextBaseAccessor::make_span_with_storage<u32>(make_inline_u32_buffer(values.span()));
    auto* original_data = original.data();

    auto moved = move(original);

    EXPECT_NE(moved.data(), original_data);
    EXPECT_EQ(moved.size(), values.size());
    for (size_t i = 0; i < values.size(); ++i)
        EXPECT_EQ(moved.data()[i], values[i]);
}

TEST_CASE(owning_span_rebuilds_storage_after_move_assignment)
{
    Array values { 0x12345678u, 0x9abcdef0u, 0x0fedcba9u, 0x87654321u };
    auto original = WebGLRenderingContextBaseAccessor::make_span_with_storage<u32>(make_inline_u32_buffer(values.span()));
    auto* original_data = original.data();

    Array other_values { 1u, 2u, 3u, 4u };
    auto moved = WebGLRenderingContextBaseAccessor::make_span_with_storage<u32>(make_inline_u32_buffer(other_values.span()));
    moved = move(original);

    EXPECT_NE(moved.data(), original_data);
    EXPECT_EQ(moved.size(), values.size());
    for (size_t i = 0; i < values.size(); ++i)
        EXPECT_EQ(moved.data()[i], values[i]);
}

TEST_CASE(non_owning_span_preserves_external_span_after_move)
{
    Array values { 10u, 20u, 30u, 40u };
    auto original = WebGLRenderingContextBaseAccessor::make_span_with_storage<u32>(values.span());

    auto moved = move(original);

    EXPECT_EQ(moved.data(), values.data());
    EXPECT_EQ(moved.size(), values.size());
    for (size_t i = 0; i < values.size(); ++i)
        EXPECT_EQ(moved.data()[i], values[i]);
}

TEST_CASE(out_of_bounds_float32_list_without_offset_is_empty)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    Web::WebGL::WebGLRenderingContextBase::Float32List list { create_out_of_bounds_array<JS::Float32Array>(realm) };

    auto span = MUST(WebGLRenderingContextBaseAccessor::span_from_float32_list(list, 0));

    EXPECT_EQ(span.size(), 0u);
    EXPECT(WebGLRenderingContextBaseAccessor::span_from_float32_list(list, 1).is_error());
    EXPECT(WebGLRenderingContextBaseAccessor::span_from_float32_list(list, 0, 1).is_error());
}

TEST_CASE(out_of_bounds_int32_list_without_offset_is_empty)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    Web::WebGL::WebGLRenderingContextBase::Int32List list { create_out_of_bounds_array<JS::Int32Array>(realm) };

    auto span = MUST(WebGLRenderingContextBaseAccessor::span_from_int32_list(list, 0));

    EXPECT_EQ(span.size(), 0u);
    EXPECT(WebGLRenderingContextBaseAccessor::span_from_int32_list(list, 1).is_error());
    EXPECT(WebGLRenderingContextBaseAccessor::span_from_int32_list(list, 0, 1).is_error());
}

TEST_CASE(out_of_bounds_uint32_list_without_offset_is_empty)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    Web::WebGL::WebGLRenderingContextBase::Uint32List list { create_out_of_bounds_array<JS::Uint32Array>(realm) };

    auto span = MUST(WebGLRenderingContextBaseAccessor::span_from_uint32_list(list, 0));

    EXPECT_EQ(span.size(), 0u);
    EXPECT(WebGLRenderingContextBaseAccessor::span_from_uint32_list(list, 1).is_error());
    EXPECT(WebGLRenderingContextBaseAccessor::span_from_uint32_list(list, 0, 1).is_error());
}
