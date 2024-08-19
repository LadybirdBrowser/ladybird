/*
 * Copyright (c) 2022, David Tuin <davidot@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Value.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibTest/TestCase.h>

using namespace JS;

template<typename Type>
static void test_nullptr_input()
{
    Type* ptr = nullptr;
    JS::Value val { ptr };
    EXPECT(val.is_null());
    EXPECT(!val.is_object());
    EXPECT(!val.is_string());
    EXPECT(!val.is_bigint());
    EXPECT(!val.is_symbol());
    EXPECT(!val.is_accessor());
    EXPECT(!val.is_cell());
    EXPECT(!val.is_number());
    EXPECT(!val.is_undefined());
}

#define TEST_NULLPTR_INPUT(type)          \
    TEST_CASE(value_nullptr_input_##type) \
    {                                     \
        test_nullptr_input<type>();       \
    }

TEST_NULLPTR_INPUT(Object);
TEST_NULLPTR_INPUT(PrimitiveString);
TEST_NULLPTR_INPUT(Symbol);
TEST_NULLPTR_INPUT(BigInt);
TEST_NULLPTR_INPUT(Accessor);

#undef TEST_NULLPTR_INPUT

TEST_CASE(valid_pointer_in_gives_same_pointer_out)
{
    if (sizeof(void*) < sizeof(double))
        return;

#define EXPECT_POINTER_TO_SURVIVE(input)                                           \
    {                                                                              \
        JS::Value value(reinterpret_cast<Object*>(static_cast<u64>(input)));       \
        EXPECT(value.is_object());                                                 \
        EXPECT(!value.is_null());                                                  \
        auto extracted_pointer = JS::Value::extract_pointer_bits(value.encoded()); \
        EXPECT_EQ(static_cast<u64>(input), extracted_pointer);                     \
    }

    EXPECT_POINTER_TO_SURVIVE(0x1);
    EXPECT_POINTER_TO_SURVIVE(0x10);
    EXPECT_POINTER_TO_SURVIVE(0x100);
    EXPECT_POINTER_TO_SURVIVE(0x00007fffffffffff);
    EXPECT_POINTER_TO_SURVIVE(0x0000700000000000);
    EXPECT_POINTER_TO_SURVIVE(0x0000100000000000);

#if ARCH(X86_64)
    // On x86-64, the top 16 bits of pointers are equal to bit 47.
    EXPECT_POINTER_TO_SURVIVE(0xffff800000000000);
    EXPECT_POINTER_TO_SURVIVE(0xffff800000000001);
    EXPECT_POINTER_TO_SURVIVE(0xffff800000000010);
#elif ARCH(AARCH64)
    // ... but they should contain zeroes on AArch64.
    EXPECT_POINTER_TO_SURVIVE(0x0000800000000000);
    EXPECT_POINTER_TO_SURVIVE(0x0000800000000001);
    EXPECT_POINTER_TO_SURVIVE(0x0000800000000010);
#endif

#undef EXPECT_POINTER_TO_SURVIVE
}

TEST_CASE(non_canon_nans)
{
#define EXPECT_TO_BE_NAN(input)                \
    {                                          \
        Value val { bit_cast<double>(input) }; \
        EXPECT(val.is_nan());                  \
        EXPECT(val.is_number());               \
        EXPECT(!val.is_integral_number());     \
        EXPECT(!val.is_finite_number());       \
        EXPECT(!val.is_infinity());            \
        EXPECT(!val.is_empty());               \
        EXPECT(!val.is_nullish());             \
    }

    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | 0x1);
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | 0x10);
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | (NULL_TAG << GC::TAG_SHIFT));
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | (UNDEFINED_TAG << GC::TAG_SHIFT));
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | (INT32_TAG << GC::TAG_SHIFT) | 0x88);
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | (OBJECT_TAG << GC::TAG_SHIFT));
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | (OBJECT_TAG << GC::TAG_SHIFT) | 0x1230);
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | (STRING_TAG << GC::TAG_SHIFT));
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | (STRING_TAG << GC::TAG_SHIFT) | 0x1230);

    u64 sign_bit = 1ULL << 63;

    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | sign_bit | 0x1);
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | sign_bit | 0x10);
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | sign_bit | (NULL_TAG << GC::TAG_SHIFT));
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | sign_bit | (UNDEFINED_TAG << GC::TAG_SHIFT));
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | sign_bit | (INT32_TAG << GC::TAG_SHIFT) | 0x88);
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | sign_bit | (OBJECT_TAG << GC::TAG_SHIFT));
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | sign_bit | (OBJECT_TAG << GC::TAG_SHIFT) | 0x1230);
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | sign_bit | (STRING_TAG << GC::TAG_SHIFT));
    EXPECT_TO_BE_NAN(GC::CANON_NAN_BITS | sign_bit | (STRING_TAG << GC::TAG_SHIFT) | 0x1230);

#undef EXPECT_TO_BE_NAN
}
