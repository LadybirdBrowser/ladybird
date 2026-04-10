/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringView.h>
#include <AK/Try.h>
#include <LibGC/Root.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Runtime/VM.h>
#include <LibTest/TestCase.h>

using namespace JS;

namespace {

struct TestVM {
    TestVM()
        : vm(VM::create())
        , execution_context(MUST(Realm::initialize_host_defined_realm(*vm, nullptr, nullptr)))
    {
    }

    ~TestVM()
    {
        vm->pop_execution_context();
    }

    NonnullRefPtr<VM> vm;
    NonnullOwnPtr<ExecutionContext> execution_context;
};

NEVER_INLINE static void materialize_temporary_rope(VM& vm)
{
    auto rope = PrimitiveString::create(vm,
        *PrimitiveString::create(vm, "f"_string),
        *PrimitiveString::create(vm, "oo"_string));
    EXPECT(rope->utf8_string_view() == "foo"sv);
}

NEVER_INLINE static void materialize_temporary_substring(VM& vm)
{
    auto source = PrimitiveString::create(vm, "foobar"_string);
    auto substring = PrimitiveString::create(vm, *source, 0, 3);
    EXPECT(substring->utf8_string_view() == "foo"sv);
}

NEVER_INLINE static void clobber_stack()
{
    char volatile stack_bytes[4096] {};
    for (size_t i = 0; i < sizeof(stack_bytes); ++i)
        stack_bytes[i] = static_cast<char>(i);
}

}

TEST_CASE(primitive_string_substring_supports_nested_ranges)
{
    TestVM test_vm;

    auto string = PrimitiveString::create(*test_vm.vm, "abcdef"_string);
    auto substring = PrimitiveString::create(*test_vm.vm, *string, 1, 4);
    auto nested_substring = PrimitiveString::create(*test_vm.vm, *substring, 1, 2);

    EXPECT_EQ(substring->length_in_utf16_code_units(), 4u);
    EXPECT(substring->utf8_string_view() == "bcde"sv);
    EXPECT_EQ(nested_substring->length_in_utf16_code_units(), 2u);
    EXPECT(nested_substring->utf8_string_view() == "cd"sv);
}

TEST_CASE(primitive_string_substring_materializes_rope_ranges)
{
    TestVM test_vm;

    auto rope = PrimitiveString::create(*test_vm.vm,
        *PrimitiveString::create(*test_vm.vm, "ab"_string),
        *PrimitiveString::create(*test_vm.vm, "cd"_string));
    auto substring = PrimitiveString::create(*test_vm.vm, *rope, 1, 2);

    EXPECT_EQ(substring->length_in_utf16_code_units(), 2u);
    EXPECT(substring->utf8_string_view() == "bc"sv);
}

TEST_CASE(primitive_string_substring_handles_surrogate_boundaries)
{
    TestVM test_vm;

    auto string = PrimitiveString::create(*test_vm.vm, "😀x"_string);
    auto leading_surrogate = PrimitiveString::create(*test_vm.vm, *string, 0, 1);
    auto trailing_surrogate = PrimitiveString::create(*test_vm.vm, *string, 1, 1);
    auto full_code_point = PrimitiveString::create(*test_vm.vm, *string, 0, 2);

    EXPECT_EQ(leading_surrogate->length_in_utf16_code_units(), 1u);
    EXPECT_EQ(leading_surrogate->utf16_string_view().code_unit_at(0), static_cast<u16>(0xd83d));
    EXPECT_EQ(trailing_surrogate->length_in_utf16_code_units(), 1u);
    EXPECT_EQ(trailing_surrogate->utf16_string_view().code_unit_at(0), static_cast<u16>(0xde00));
    EXPECT(full_code_point->utf8_string_view() == "😀"sv);
}

TEST_CASE(primitive_string_substring_utf16_views_stay_deferred)
{
    TestVM test_vm;

    auto string = PrimitiveString::create(*test_vm.vm, Utf16View { u"abcd", 4 });
    auto substring = PrimitiveString::create(*test_vm.vm, *string, 1, 2);

    EXPECT(string->has_utf16_string());
    EXPECT(!substring->has_utf16_string());

    auto view = substring->utf16_string_view();

    EXPECT_EQ(view.length_in_code_units(), 2u);
    EXPECT_EQ(view.code_unit_at(0), 'b');
    EXPECT_EQ(view.code_unit_at(1), 'c');
    EXPECT(!substring->has_utf16_string());
}

TEST_CASE(primitive_string_substring_equality_uses_utf16_code_units)
{
    TestVM test_vm;

    char16_t const source_code_units[] = { u'x', u'x', u'x', 0xd800, 0xdc00, 0xdc00, u'x', u'x' };
    char16_t const substring_code_units[] = { 0xd800, 0xdc00, 0xdc00 };

    auto source = PrimitiveString::create(*test_vm.vm, Utf16View { source_code_units, 8 });
    auto substring = PrimitiveString::create(*test_vm.vm, *source, 3, 3);
    auto expected = PrimitiveString::create(*test_vm.vm, Utf16View { substring_code_units, 3 });

    EXPECT(*substring == *expected);
}

TEST_CASE(deferred_primitive_strings_do_not_evict_cached_strings)
{
    TestVM test_vm;

    GC::Root<PrimitiveString> cached_foo = PrimitiveString::create(*test_vm.vm, "foo"_string);

    materialize_temporary_rope(*test_vm.vm);
    clobber_stack();

    test_vm.vm->heap().collect_garbage();
    EXPECT_EQ(PrimitiveString::create(*test_vm.vm, "foo"_string).ptr(), cached_foo.ptr());

    materialize_temporary_substring(*test_vm.vm);
    clobber_stack();

    test_vm.vm->heap().collect_garbage();
    EXPECT_EQ(PrimitiveString::create(*test_vm.vm, "foo"_string).ptr(), cached_foo.ptr());
}
