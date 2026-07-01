/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/PrimitiveStorage.h>
#include <LibTest/TestCase.h>

#if !defined(AK_OS_WINDOWS)
#    include <AK/Platform.h>
#    include <signal.h>
#    include <sys/wait.h>
#    include <unistd.h>
#endif

TEST_CASE(small_allocation)
{
    auto& storage = GC::PrimitiveStorage::the();
    auto handle = MUST(storage.try_allocate(32));
    EXPECT(storage.is_valid(handle));
    EXPECT_EQ(storage.size(handle), 32u);
    EXPECT(storage.capacity(handle) >= 32u);
    EXPECT(storage.data(handle) != nullptr);
    EXPECT(storage.contains(storage.data(handle), 32));

    *storage.data(handle, 0) = 0xaa;
    EXPECT_EQ(*storage.data(handle, 0), 0xaa);

    storage.free(handle);
}

TEST_CASE(free_maximum_small_allocation)
{
    auto& storage = GC::PrimitiveStorage::the();
    auto handle = MUST(storage.try_allocate(64 * KiB));
    EXPECT_EQ(storage.capacity(handle), 64u * KiB);
    storage.free(handle);
}

TEST_CASE(large_allocation)
{
    auto& storage = GC::PrimitiveStorage::the();
    auto handle = MUST(storage.try_allocate(128 * KiB));
    EXPECT(storage.is_valid(handle));
    EXPECT_EQ(storage.size(handle), 128u * KiB);
    EXPECT(storage.capacity(handle) >= 128u * KiB);
    EXPECT(storage.data(handle) != nullptr);
    EXPECT(storage.contains(handle, storage.data(handle), storage.size(handle)));

    storage.free(handle);
}

TEST_CASE(freeing_top_large_allocation_reclaims_tail_capacity)
{
    auto& storage = GC::PrimitiveStorage::the();

    auto first = MUST(storage.try_reserve(0, 1ull * GiB));
    auto first_offset = storage.offset(first);
    storage.free(first);

    auto replacement_capacity = GC::PrimitiveStorage::default_cage_size - first_offset;
    ASSUME(replacement_capacity > 1ull * GiB);

    auto replacement = MUST(storage.try_reserve(0, replacement_capacity));
    EXPECT(storage.capacity(replacement) >= replacement_capacity);
    storage.free(replacement);
}

#if !defined(AK_OS_WINDOWS)
TEST_CASE(freeing_large_allocation_decommits_storage)
{
    auto& storage = GC::PrimitiveStorage::the();
    auto handle = MUST(storage.try_allocate(128 * KiB));
    auto tail = MUST(storage.try_allocate(128 * KiB));
    auto* data = storage.data(handle);
    *data = 0xaa;

    storage.free(handle);

    auto child = fork();
    ASSUME(child >= 0);

    if (child == 0) {
        *data = 0x55;
        _exit(0);
    }

    int status = 0;
    EXPECT_EQ(waitpid(child, &status, 0), child);

    auto died_from_inaccessible_memory = WIFSIGNALED(status) && (WTERMSIG(status) == SIGSEGV || WTERMSIG(status) == SIGBUS);
#    if defined(HAS_ADDRESS_SANITIZER)
    auto died_from_asan = (WIFEXITED(status) && WEXITSTATUS(status) != 0) || (WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    EXPECT(died_from_inaccessible_memory || died_from_asan);
#    else
    EXPECT(died_from_inaccessible_memory);
#    endif

    storage.free(tail);
}
#endif

TEST_CASE(resize_and_zero_fill_growth)
{
    auto& storage = GC::PrimitiveStorage::the();
    auto handle = MUST(storage.try_allocate(8, GC::PrimitiveStorage::ZeroFillNewBytes::Yes));
    for (size_t i = 0; i < storage.size(handle); ++i)
        *storage.data(handle, i) = 0x7b;

    MUST(storage.try_resize(handle, 64, GC::PrimitiveStorage::ZeroFillNewBytes::Yes));
    EXPECT_EQ(storage.size(handle), 64u);
    EXPECT_EQ(*storage.data(handle, 0), 0x7b);
    EXPECT_EQ(*storage.data(handle, 7), 0x7b);
    for (size_t i = 8; i < storage.size(handle); ++i)
        EXPECT_EQ(*storage.data(handle, i), 0u);

    storage.free(handle);
}

TEST_CASE(reserve_capacity)
{
    auto& storage = GC::PrimitiveStorage::the();
    auto handle = MUST(storage.try_reserve(16, 128 * KiB));
    auto offset = storage.offset(handle);

    MUST(storage.try_resize(handle, 96 * KiB, GC::PrimitiveStorage::ZeroFillNewBytes::Yes));
    EXPECT_EQ(storage.offset(handle), offset);
    EXPECT_EQ(storage.size(handle), 96u * KiB);
    EXPECT(storage.capacity(handle) >= 128u * KiB);

    storage.free(handle);
}

TEST_CASE(resize_and_reserve_failure_preserves_existing_storage)
{
    auto& storage = GC::PrimitiveStorage::the();
    auto handle = MUST(storage.try_reserve(16, 64 * KiB, GC::PrimitiveStorage::ZeroFillNewBytes::Yes));
    auto offset = storage.offset(handle);
    auto capacity = storage.capacity(handle);
    auto* data = storage.data(handle);
    *storage.data(handle, 0) = 0x42;
    *storage.data(handle, 15) = 0x24;

    auto result = storage.try_resize_and_reserve(handle, 32, GC::PrimitiveStorage::default_cage_size + 64 * KiB, GC::PrimitiveStorage::ZeroFillNewBytes::Yes);

    EXPECT(result.is_error());
    EXPECT_EQ(storage.offset(handle), offset);
    EXPECT_EQ(storage.size(handle), 16u);
    EXPECT_EQ(storage.capacity(handle), capacity);
    EXPECT_EQ(storage.data(handle), data);
    EXPECT_EQ(*storage.data(handle, 0), 0x42);
    EXPECT_EQ(*storage.data(handle, 15), 0x24);

    storage.free(handle);
}

TEST_CASE(free_invalidates_generation)
{
    auto& storage = GC::PrimitiveStorage::the();
    auto handle = MUST(storage.try_allocate(32));
    storage.free(handle);

    EXPECT(!storage.is_valid(handle));
    EXPECT(storage.data(handle) == nullptr);
    EXPECT_EQ(storage.offset(handle), GC::PrimitiveStorage::invalid_offset);
    EXPECT(storage.try_resize(handle, 64).is_error());
}

TEST_CASE(invalid_handle)
{
    auto& storage = GC::PrimitiveStorage::the();
    GC::PrimitiveStorageHandle handle;

    EXPECT(!storage.is_valid(handle));
    EXPECT(storage.data(handle) == nullptr);
    EXPECT_EQ(storage.size(handle), 0u);
    EXPECT_EQ(storage.capacity(handle), 0u);
}

TEST_CASE(cage_has_tail_guard_outside_logical_size)
{
    auto& storage = GC::PrimitiveStorage::the();
    auto handle = MUST(storage.try_allocate(1));
    auto* cage_base = storage.cage_base();
    VERIFY(cage_base);

    EXPECT_EQ(storage.cage_size(), GC::PrimitiveStorage::default_cage_size);
    EXPECT(storage.contains(cage_base + GC::PrimitiveStorage::default_cage_size - 1, 1));
    EXPECT(!storage.contains(cage_base + GC::PrimitiveStorage::default_cage_size, 1));

    storage.free(handle);
}

TEST_CASE(out_of_cage_rejection)
{
    auto& storage = GC::PrimitiveStorage::the();
    auto handle = MUST(storage.try_allocate(32));
    u8 local = 0;

    EXPECT(!storage.contains(&local, 1));
    EXPECT(!storage.contains(handle, &local, 1));
    EXPECT(storage.contains(handle, storage.data(handle), 32));
    EXPECT(!storage.contains(handle, storage.data(handle), storage.capacity(handle) + 1));

    storage.free(handle);
}
