/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace {

struct TestVM {
    TestVM()
        : vm(JS::VM::create())
        , execution_context(MUST(JS::Realm::initialize_host_defined_realm(*vm, nullptr, nullptr)))
    {
        auto& realm = *vm->current_realm();
        auto intrinsics = realm.create<Web::Bindings::Intrinsics>(realm);
        realm.set_host_defined(make<Web::Bindings::HostDefined>(intrinsics));
    }

    ~TestVM()
    {
        vm->pop_execution_context();
    }

    NonnullRefPtr<JS::VM> vm;
    NonnullOwnPtr<JS::ExecutionContext> execution_context;
};

GC::Ref<JS::Uint8ClampedArray> create_out_of_bounds_uint8_clamped_array(JS::Realm& realm)
{
    auto array_buffer = MUST(JS::ArrayBuffer::create(realm, 16));
    array_buffer->set_max_byte_length(16);
    MUST(array_buffer->try_resize(4));

    auto typed_array = JS::Uint8ClampedArray::create(realm, 0, array_buffer);
    typed_array->set_viewed_array_buffer(array_buffer.ptr());
    typed_array->set_array_length(4);
    typed_array->set_byte_length(4);
    typed_array->set_byte_offset(8);

    return typed_array;
}

}

TEST_CASE(create_rejects_out_of_bounds_uint8_clamped_array)
{
    TestVM test_vm;
    auto& realm = *test_vm.vm->current_realm();
    auto data = create_out_of_bounds_uint8_clamped_array(realm);

    auto result = Web::HTML::ImageData::create(realm, data, 1, 1);

    EXPECT(result.is_exception());
    auto exception = result.exception();
    EXPECT(exception.has<GC::Ref<Web::WebIDL::DOMException>>());
    EXPECT_EQ(exception.get<GC::Ref<Web::WebIDL::DOMException>>()->name(), "InvalidStateError"_fly_string);
}
