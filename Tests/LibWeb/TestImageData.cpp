/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FlyString.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/WebIDL/DOMException.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace {

struct TestVM {
    TestVM()
        : realm(Web::Bindings::create_a_principal_javascript_realm())
    {
        depth = realm->vm().execution_context_stack().size() - 1;
    }

    ~TestVM()
    {
        auto& vm = realm->vm();
        while (vm.execution_context_stack().size() > depth)
            vm.pop_execution_context();
    }

    size_t depth { 0 };
    GC::Ref<JS::Realm> realm;
};

GC::Ref<JS::Uint8ClampedArray> create_out_of_bounds_uint8_clamped_array(JS::Realm& realm)
{
    auto array_buffer = MUST(JS::ArrayBuffer::create(realm, 16));
    array_buffer->set_max_byte_length(16);

    auto typed_array = JS::TypedArrayBase::create_from_slots(realm, JS::TypedArrayBase::Kind::Uint8ClampedArray, array_buffer, JS::ByteLength { 4 }, JS::ByteLength { 4 }, 8);
    MUST(JS::call(realm.vm(), MUST(array_buffer->get(realm.vm().names.resize)), JS::Value { array_buffer.ptr() }, JS::Value(4)));

    return as<JS::Uint8ClampedArray>(*typed_array);
}

}

TEST_CASE(create_rejects_out_of_bounds_uint8_clamped_array)
{
    TestVM test_vm;
    auto& realm = *test_vm.realm;
    auto data = create_out_of_bounds_uint8_clamped_array(realm);

    auto result = Web::HTML::ImageData::create(realm, data, 1, 1);

    EXPECT(result.is_exception());
    auto exception = result.exception();
    EXPECT(exception.has<GC::Ref<Web::WebIDL::DOMException>>());
    EXPECT_EQ(exception.get<GC::Ref<Web::WebIDL::DOMException>>()->name(), "InvalidStateError"_fly_string);
}
