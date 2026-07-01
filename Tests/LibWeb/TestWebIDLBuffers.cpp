/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteBuffer.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/WebIDL/Buffers.h>

namespace {

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

GC::Ref<JS::ArrayBuffer> create_shrunken_resizable_array_buffer(JS::VM& vm)
{
    auto& realm = *vm.current_realm();
    auto array_buffer = MUST(JS::ArrayBuffer::create(realm, 16));
    array_buffer->set_max_byte_length(16);
    MUST(array_buffer->try_resize(4));
    return array_buffer;
}

GC::Ref<JS::Uint8Array> create_out_of_bounds_uint8_array(JS::VM& vm)
{
    auto& realm = *vm.current_realm();
    auto array_buffer = create_shrunken_resizable_array_buffer(vm);
    auto typed_array = JS::Uint8Array::create(realm, 0, array_buffer);

    typed_array->set_viewed_array_buffer(array_buffer.ptr());
    typed_array->set_array_length(4);
    typed_array->set_byte_length(4);
    typed_array->set_byte_offset(8);

    return typed_array;
}

GC::Ref<JS::DataView> create_out_of_bounds_data_view(JS::VM& vm)
{
    auto& realm = *vm.current_realm();
    auto array_buffer = create_shrunken_resizable_array_buffer(vm);
    return JS::DataView::create(realm, array_buffer.ptr(), JS::ByteLength { 4 }, 8);
}

GC::Ref<JS::Uint16Array> create_uint16_array_view(JS::VM& vm, GC::Ref<JS::ArrayBuffer> array_buffer, u32 byte_offset, u32 element_length)
{
    auto& realm = *vm.current_realm();
    auto typed_array = JS::Uint16Array::create(realm, 0, array_buffer);

    typed_array->set_viewed_array_buffer(array_buffer.ptr());
    typed_array->set_array_length(element_length);
    typed_array->set_byte_length(element_length * sizeof(u16));
    typed_array->set_byte_offset(byte_offset);

    return typed_array;
}

Web::WebIDL::ArrayBufferView typed_array_view(JS::VM& vm)
{
    Web::WebIDL::ArrayBufferViewVariant view { create_out_of_bounds_uint8_array(vm) };
    return Web::WebIDL::ArrayBufferView { view };
}

Web::WebIDL::ArrayBufferView make_data_view(JS::VM& vm)
{
    Web::WebIDL::ArrayBufferViewVariant view { create_out_of_bounds_data_view(vm) };
    return Web::WebIDL::ArrayBufferView { view };
}

}

TEST_CASE(buffer_source_reports_out_of_bounds_views_as_empty)
{
    TestVM test_vm;

    Web::WebIDL::BufferSource typed_array_source { typed_array_view(*test_vm.vm) };
    EXPECT(typed_array_source.is_out_of_bounds());
    EXPECT_EQ(typed_array_source.byte_length(), 0u);

    Web::WebIDL::BufferSource data_view_source { make_data_view(*test_vm.vm) };
    EXPECT(data_view_source.is_out_of_bounds());
    EXPECT_EQ(data_view_source.byte_length(), 0u);
}

TEST_CASE(array_buffer_view_checked_write_handles_in_bounds_views)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    auto array_buffer = MUST(JS::ArrayBuffer::create(realm, 16));
    u8 initial_bytes[] {
        0x10, 0x11, 0x12, 0x13,
        0x14, 0x15, 0x16, 0x17,
        0x18, 0x19, 0x1a, 0x1b,
        0x1c, 0x1d, 0x1e, 0x1f
    };
    array_buffer->overwrite(0, initial_bytes, sizeof(initial_bytes));

    Web::WebIDL::ArrayBufferViewVariant typed_array_view_variant { create_uint16_array_view(*test_vm.vm, array_buffer, 4, 2) };
    Web::WebIDL::ArrayBufferView typed_array_view { typed_array_view_variant };
    EXPECT(!typed_array_view.is_out_of_bounds());

    Web::WebIDL::BufferSource typed_array_source { typed_array_view };
    EXPECT_EQ(typed_array_source.byte_length(), 4u);

    u8 one_byte = 0xaa;
    EXPECT(typed_array_view.write_checked({ &one_byte, 1 }).is_error());

    u8 typed_array_replacement[] { 0xf0, 0xf1 };
    EXPECT(!typed_array_view.write_checked(typed_array_replacement, 2).is_error());

    auto data_view = JS::DataView::create(realm, array_buffer.ptr(), JS::ByteLength { 4 }, 8);
    Web::WebIDL::ArrayBufferViewVariant data_view_variant { data_view };
    Web::WebIDL::ArrayBufferView data_view_view { data_view_variant };
    EXPECT(!data_view_view.is_out_of_bounds());

    Web::WebIDL::BufferSource data_view_source { data_view_view };
    EXPECT_EQ(data_view_source.byte_length(), 4u);

    u8 data_view_replacement[] { 0xe0, 0xe1 };
    EXPECT(!data_view_view.write_checked(data_view_replacement, 1).is_error());

    EXPECT_EQ(array_buffer->bytes()[6], 0xf0);
    EXPECT_EQ(array_buffer->bytes()[7], 0xf1);
    EXPECT_EQ(array_buffer->bytes()[9], 0xe0);
    EXPECT_EQ(array_buffer->bytes()[10], 0xe1);
}

TEST_CASE(array_buffer_view_checked_write_handles_out_of_bounds_views)
{
    TestVM test_vm;
    auto empty_bytes = MUST(ByteBuffer::create_uninitialized(0));
    u8 byte = 0xaa;
    ReadonlyBytes one_byte { &byte, 1 };

    auto typed_array = typed_array_view(*test_vm.vm);
    EXPECT(typed_array.is_out_of_bounds());
    EXPECT(!typed_array.write_checked(empty_bytes).is_error());
    EXPECT(typed_array.write_checked(empty_bytes, 1).is_error());
    EXPECT(typed_array.write_checked(one_byte).is_error());

    auto data_view = make_data_view(*test_vm.vm);
    EXPECT(data_view.is_out_of_bounds());
    EXPECT(!data_view.write_checked(empty_bytes).is_error());
    EXPECT(data_view.write_checked(empty_bytes, 1).is_error());
    EXPECT(data_view.write_checked(one_byte).is_error());
}
