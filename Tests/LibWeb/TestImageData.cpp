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
#include <LibWeb/Bindings/DOMException.h>
#include <LibWeb/Bindings/ImageData.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/WebIDL/DOMException.h>

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

}

// new ImageData(data, sw, sh), whose implementation is in Rust.
static JS::ThrowCompletionOr<GC::Ref<JS::Object>> construct_image_data(JS::Realm& realm, JS::Value data, u32 sw, u32 sh)
{
    auto& constructor = Web::Bindings::ensure_web_constructor<Web::Bindings::ImageDataPrototype>(realm, "ImageData"_utf16_fly_string);
    return JS::construct(realm.vm(), constructor, data, JS::Value(static_cast<i32>(sw)), JS::Value(static_cast<i32>(sh)));
}

TEST_CASE(construct_rejects_detached_uint8_clamped_array)
{
    TestVM test_vm;
    auto& realm = *test_vm.realm;
    auto data = MUST(JS::Uint8ClampedArray::create(realm, 4));
    MUST(JS::detach_array_buffer(realm.vm(), *data->viewed_array_buffer()));

    auto result = construct_image_data(realm, JS::Value(static_cast<JS::Object*>(data.ptr())), 1, 1);

    EXPECT(result.is_throw_completion());
    auto thrown = result.throw_completion().value();
    EXPECT(thrown.is_object());
    auto* exception = Web::Bindings::impl_from<Web::WebIDL::DOMException>(&thrown.as_object());
    EXPECT(exception);
    EXPECT_EQ(exception->name(), "InvalidStateError"_fly_string);
}

TEST_CASE(construct_views_the_pixels_of_its_data)
{
    TestVM test_vm;
    auto& realm = *test_vm.realm;
    auto data = MUST(JS::Uint8ClampedArray::create(realm, 8));

    auto wrapper = MUST(construct_image_data(realm, JS::Value(static_cast<JS::Object*>(data.ptr())), 2, 1));

    auto* image_data = Web::Bindings::impl_from<Web::HTML::ImageData>(wrapper.ptr());
    EXPECT(image_data);
    EXPECT_EQ(image_data->width(), 2u);
    EXPECT_EQ(image_data->height(), 1u);
    EXPECT_EQ(image_data->pixels(), data->viewed_array_buffer()->data_at(0));

    MUST(JS::detach_array_buffer(realm.vm(), *data->viewed_array_buffer()));
    EXPECT(image_data->is_detached());
    EXPECT_EQ(image_data->pixels(), nullptr);
}
