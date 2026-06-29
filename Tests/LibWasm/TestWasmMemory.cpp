/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/PrimitiveStorage.h>
#include <LibTest/TestCase.h>
#include <LibWasm/AbstractMachine/AbstractMachine.h>

static size_t memory_capacity(Wasm::MemoryInstance const& memory)
{
    return GC::PrimitiveStorage::the().capacity(memory.data().primitive_storage_handle());
}

TEST_CASE(wasm32_memory_without_max_uses_default_reservation)
{
    auto memory = MUST(Wasm::MemoryInstance::create(Wasm::MemoryType { Wasm::Limits(Wasm::AddressType::I32, 1) }));
    auto* data_before_grow = memory.data().data();

    EXPECT_EQ(memory.size(), Wasm::Constants::page_size);
    EXPECT_EQ(memory_capacity(memory), static_cast<size_t>(Wasm::Constants::wasm32_default_memory_reservation_size));

    EXPECT(memory.grow(Wasm::Constants::page_size));

    EXPECT_EQ(memory.size(), 2uz * Wasm::Constants::page_size);
    EXPECT_EQ(memory_capacity(memory), static_cast<size_t>(Wasm::Constants::wasm32_default_memory_reservation_size));
    EXPECT_EQ(memory.data().data(), data_before_grow);
}

TEST_CASE(wasm32_memory_without_max_grows_reservation_geometrically)
{
    constexpr auto default_reservation_size = static_cast<size_t>(Wasm::Constants::wasm32_default_memory_reservation_size);
    constexpr auto default_reservation_pages = default_reservation_size / Wasm::Constants::page_size;
    static_assert(default_reservation_pages * Wasm::Constants::page_size == default_reservation_size);

    auto memory = MUST(Wasm::MemoryInstance::create(Wasm::MemoryType { Wasm::Limits(Wasm::AddressType::I32, default_reservation_pages) }));

    EXPECT_EQ(memory.size(), default_reservation_size);
    EXPECT_EQ(memory_capacity(memory), default_reservation_size);

    EXPECT(memory.grow(Wasm::Constants::page_size));

    EXPECT_EQ(memory.size(), default_reservation_size + Wasm::Constants::page_size);
    EXPECT_EQ(memory_capacity(memory), 2uz * default_reservation_size);
}

TEST_CASE(wasm32_memory_with_max_reserves_explicit_max)
{
    auto memory = MUST(Wasm::MemoryInstance::create(Wasm::MemoryType { Wasm::Limits(Wasm::AddressType::I32, 1, 3) }));

    EXPECT_EQ(memory.size(), Wasm::Constants::page_size);
    EXPECT_EQ(memory_capacity(memory), 3uz * Wasm::Constants::page_size);
}

TEST_CASE(memory64_memory_without_max_can_grow)
{
    auto memory = MUST(Wasm::MemoryInstance::create(Wasm::MemoryType { Wasm::Limits(Wasm::AddressType::I64, 0) }));

    EXPECT_EQ(memory.size(), 0u);

    EXPECT(memory.grow(Wasm::Constants::page_size));
    EXPECT_EQ(memory.size(), Wasm::Constants::page_size);

    EXPECT(memory.grow(4uz * Wasm::Constants::page_size));
    EXPECT_EQ(memory.size(), 5uz * Wasm::Constants::page_size);
}

TEST_CASE(memory64_memory_without_max_cannot_grow_past_current_compiled_address_width)
{
    static_assert(Wasm::Constants::wasm64_max_pages == Wasm::Constants::wasm32_max_pages);
    constexpr auto maximum_supported_size = static_cast<size_t>(Wasm::Constants::wasm64_max_pages * Wasm::Constants::page_size);

    auto memory = MUST(Wasm::MemoryInstance::create(Wasm::MemoryType { Wasm::Limits(Wasm::AddressType::I64, 0) }));

    EXPECT(!memory.grow(maximum_supported_size + Wasm::Constants::page_size));
    EXPECT_EQ(memory.size(), 0u);
}

TEST_CASE(memory64_memory_initial_size_cannot_exceed_current_compiled_address_width)
{
    auto memory = Wasm::MemoryInstance::create(Wasm::MemoryType { Wasm::Limits(Wasm::AddressType::I64, Wasm::Constants::wasm64_max_pages + 1) });
    EXPECT(memory.is_error());
}

TEST_CASE(memory_buffer_failed_resize_and_reserve_preserves_backing_storage)
{
    Wasm::MemoryBuffer buffer;
    MUST(buffer.try_resize(Wasm::Constants::page_size));
    auto handle = buffer.primitive_storage_handle();
    auto offset = GC::PrimitiveStorage::the().offset(handle);
    auto capacity = buffer.capacity();
    auto* data = buffer.data();
    buffer[0] = 0x42;
    buffer[Wasm::Constants::page_size - 1] = 0x24;

    auto result = buffer.try_resize(2uz * Wasm::Constants::page_size, GC::PrimitiveStorage::default_cage_size + Wasm::Constants::page_size);

    EXPECT(result.is_error());
    EXPECT_EQ(buffer.primitive_storage_handle().index, handle.index);
    EXPECT_EQ(buffer.primitive_storage_handle().generation, handle.generation);
    EXPECT_EQ(GC::PrimitiveStorage::the().offset(handle), offset);
    EXPECT_EQ(buffer.size(), Wasm::Constants::page_size);
    EXPECT_EQ(buffer.capacity(), capacity);
    EXPECT_EQ(buffer.data(), data);
    EXPECT_EQ(buffer[0], 0x42);
    EXPECT_EQ(buffer[Wasm::Constants::page_size - 1], 0x24);
}

TEST_CASE(memory_buffer_copy_fill_and_move_helpers)
{
    Wasm::MemoryBuffer source;
    Wasm::MemoryBuffer destination;
    MUST(source.try_resize(32));
    MUST(destination.try_resize(32));

    for (u8 i = 0; i < 16; ++i)
        source[i] = i;

    destination.fill(0, 0xaa, 32);
    for (size_t i = 0; i < 32; ++i)
        EXPECT_EQ(destination[i], 0xaa);

    destination.copy_from(source, 4, 8, 8);
    for (size_t i = 0; i < 8; ++i)
        EXPECT_EQ(destination[8 + i], source[4 + i]);

    destination.move_data(10, 8, 8);
    EXPECT_EQ(destination[10], 4);
    EXPECT_EQ(destination[11], 5);
    EXPECT_EQ(destination[16], 10);
    EXPECT_EQ(destination[17], 11);

    u8 replacement[] = { 1, 2, 3, 4 };
    destination.overwrite(28, replacement, sizeof(replacement));
    EXPECT_EQ(destination[28], 1);
    EXPECT_EQ(destination[29], 2);
    EXPECT_EQ(destination[30], 3);
    EXPECT_EQ(destination[31], 4);
}
