/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/Vector.h>

namespace {

struct CopyOnly {
    int& instance_count;

    CopyOnly(int& count)
        : instance_count(count)
    {
        instance_count++;
    }

    CopyOnly(CopyOnly const& other)
        : instance_count(other.instance_count)
    {
        instance_count++;
    }

    ~CopyOnly()
    {
        instance_count--;
    }
};

}

TEST_CASE(move_construction_destroys_old_inline_wrapper)
{
    int instance_count = 0;

    {
        Function<void()> source = [captured = CopyOnly(instance_count)]() {
            (void)captured;
        };
        EXPECT_EQ(instance_count, 1);

        Function<void()> destination = move(source);
        EXPECT_EQ(instance_count, 1);

        source = nullptr;
        EXPECT_EQ(instance_count, 1);
    }

    EXPECT_EQ(instance_count, 0);
}

TEST_CASE(move_assignment_destroys_old_inline_wrapper)
{
    int instance_count = 0;

    {
        Function<void()> source = [captured = CopyOnly(instance_count)]() {
            (void)captured;
        };
        EXPECT_EQ(instance_count, 1);

        Function<void()> destination;
        destination = move(source);
        EXPECT_EQ(instance_count, 1);

        source = nullptr;
        EXPECT_EQ(instance_count, 1);
    }

    EXPECT_EQ(instance_count, 0);
}

TEST_CASE(outline_wrapper_respects_callable_alignment)
{
    struct alignas(64) OverAligned {
        int value { 42 };
    };

    Function<bool()> function = [captured = OverAligned {}]() {
        return bit_cast<FlatPtr>(&captured) % alignof(OverAligned) == 0 && captured.value == 42;
    };
    EXPECT(function());

    Function<bool()> moved = move(function);
    EXPECT(moved());
}

// The GC scans a callable's bytes conservatively (raw_capture_range()), so the padding between captures, and storage
// a callable doesn't use, must not keep whatever stale data the memory held before.
static constexpr u8 stale_byte = 0xaa;

// The captures in these tests put padding after a bool, before a member with a copy constructor of its own, so no copy
// of the captures writes that padding: It keeps whatever the callable's storage held. A callable that's copied as one
// block of bytes would instead bring along the padding of the callable it's copied from, which Function can't clear.
// Check just the padding, since the reference's bytes are an address, and any of them could be the stale byte.
template<typename Captures>
static bool padding_holds_stale_byte(Captures const& captures)
{
    auto const* start = reinterpret_cast<u8 const*>(&captures);
    auto padding_size = static_cast<size_t>(reinterpret_cast<u8 const*>(&captures.counted) - start) - sizeof(bool);
    VERIFY(padding_size > 0);
    for (auto value : ReadonlyBytes { start + sizeof(bool), padding_size }) {
        if (value == stale_byte)
            return true;
    }
    return false;
}

struct InlineCaptures {
    bool flag { true };
    CopyOnly counted;
};
using InlineCapturesFunction = Function<InlineCaptures const*()>;

TEST_CASE(inline_callable_padding_is_zeroed)
{
    int instance_count = 0;
    alignas(InlineCapturesFunction) u8 buffer[sizeof(InlineCapturesFunction)];
    __builtin_memset(buffer, stale_byte, sizeof(buffer));

    auto* function = new (buffer) InlineCapturesFunction([captures = InlineCaptures { true, CopyOnly(instance_count) }]() {
        return &captures;
    });
    EXPECT(!padding_holds_stale_byte(*(*function)()));
    function->~Function();
    EXPECT_EQ(instance_count, 0);
}

TEST_CASE(moved_inline_callable_padding_is_zeroed)
{
    int instance_count = 0;
    alignas(InlineCapturesFunction) u8 source_buffer[sizeof(InlineCapturesFunction)];
    __builtin_memset(source_buffer, stale_byte, sizeof(source_buffer));
    auto* source = new (source_buffer) InlineCapturesFunction([captures = InlineCaptures { true, CopyOnly(instance_count) }]() {
        return &captures;
    });

    alignas(InlineCapturesFunction) u8 buffer[sizeof(InlineCapturesFunction)];
    __builtin_memset(buffer, stale_byte, sizeof(buffer));
    auto* destination = new (buffer) InlineCapturesFunction(move(*source));
    EXPECT(!padding_holds_stale_byte(*(*destination)()));
    destination->~Function();
    source->~Function();
    EXPECT_EQ(instance_count, 0);
}

TEST_CASE(outline_callable_padding_is_zeroed)
{
    int instance_count = 0;
    struct Captures {
        bool flag { true };
        CopyOnly counted;
        u64 numbers[6] { 1, 2, 3, 4, 5, 6 };
    };
    static constexpr size_t wrapper_size = sizeof(void*) + sizeof(Captures);

    // Fill enough blocks of the wrapper's size class with stale bytes, and free them, that the allocator hands one of
    // them back for the wrapper.
    Vector<void*> stale_blocks;
    for (size_t i = 0; i < 4096; ++i) {
        auto* block = kmalloc(HeapPartition::General, wrapper_size);
        __builtin_memset(block, stale_byte, wrapper_size);
        stale_blocks.append(block);
    }
    for (auto* block : stale_blocks)
        kfree(block);

    Function<Captures const*()> function = [captures = Captures { true, CopyOnly(instance_count) }]() {
        return &captures;
    };
    auto const* captures = function();
    EXPECT_EQ(captures->numbers[5], 6u);
    EXPECT(!padding_holds_stale_byte(*captures));
}
