/*
 * Copyright (c) 2025, Andrew Kaster <andrew@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/ByteReader.h>
#include <AK/Function.h>
#include <AK/Platform.h>

TEST_CASE(SimpleBlock)
{
    auto b = ^{ };

    static_assert(IsBlockClosure<decltype(b)>);

    auto f = Function<void()>(b);

    f();
}

TEST_CASE(BlockCaptureInt)
{
    __block int x = 0;
    auto b = ^{
        x = 2;
    };
    auto f = Function<void()>(b);

    f();

    EXPECT_EQ(x, 2);
}

TEST_CASE(BlockCaptureString)
{
    __block String s = "hello"_string;
    auto b = ^{
        s = "world"_string;
    };
    auto f = Function<void()>(b);

    f();

    EXPECT_EQ(s, "world"_string);
}

TEST_CASE(BlockCaptureLongStringAndInt)
{
    __block String s = "hello, world, this is a long string to avoid small string optimization"_string;
    __block int x = 0;
    auto b = ^{
        s = "world, hello, this is a long string to avoid small string optimization"_string;
        x = 2;
    };
    auto f = Function<void()>(b);

    f();

    EXPECT_EQ(s, "world, hello, this is a long string to avoid small string optimization"_string);
    EXPECT_EQ(x, 2);
}

// Struct definitions from llvm-project/compiler-rt/lib/BlocksRuntime/Block_private.h @ d0177670a0e59e9d9719386f85bb78de0929407c
struct Block_descriptor {
    unsigned long int reserved;
    unsigned long int size;
    void (*copy)(void* dst, void* src);
    void (*dispose)(void*);
};

struct Block_layout {
    void* isa;
    int flags;
    int reserved;
    void (*invoke)(void*, ...);
    struct Block_descriptor* descriptor;
    /* Imported variables. */
};
// This check is super important for proper tracking of block closure captures
static_assert(sizeof(Block_layout) == AK::Detail::block_layout_size);

TEST_CASE(BlockPointerCaptures)
{
    int x = 0;
    int* p = &x;

    auto b = ^{
        *p = 2;
    };

    auto f = Function<void()>(b);
    auto span = f.raw_capture_range();

    int* captured_p = ByteReader::load_pointer<int>(span.data());
    EXPECT_EQ(captured_p, p);

    f();

    EXPECT_EQ(x, 2);
}

TEST_CASE(AssignBlock)
{
    auto b = ^{ };

    auto f = Function<void()>(b);

    auto b2 = ^{ };

    f = b2;

    f();

    f = b;

    f();
}

#ifdef AK_HAS_OBJC_ARC
TEST_CASE(AssignWeakBlock)
{
    __block int count = 0;
    Function<void()> f;

    {
        auto b = ^{ ++count; };
        f = b;
    }
    f();
    EXPECT_EQ(count, 1);

    {
        auto b = ^{ ++count; };
        auto const __weak weak_b = b;
        f = weak_b;
        f();
        EXPECT_EQ(count, 2);
    }
    f();
    EXPECT_EQ(count, 3);
}
#endif
