/*
 * Copyright (c) 2018-2025, the SerenityOS developers.
 * Copyright (c) 2025, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */


#pragma once

#include <AK/HashMap.h>
#include <AK/IntrusiveList.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/OwnPtr.h>
#include <AK/Types.h>

#ifdef AK_OS_SERENITY
#    include <Kernel/API/Syscall.h>
#    include <sys/mman.h>
extern "C" void* serenity_mmap(void*, size_t, int, int, int, off_t, size_t, char const*);
#elif defined(AK_OS_WINDOWS)
#    include <windows.h>
#else
#    include <sys/mman.h>
#endif

namespace AK {

class TracingAllocator {
public:
    static constexpr size_t DefaultChunkSize = 64 * MiB;
    static constexpr size_t MinAllocationSize = 16;
    static constexpr size_t AllocationAlignment = 16;
    static constexpr u32 BlockMagic = 0xafcffede;

    struct Block {
        Block* next { nullptr };
        Block* prev { nullptr };
        size_t size { 0 };
        bool is_free { true };
        u32 magic { BlockMagic };

        void* user_ptr() { return reinterpret_cast<u8*>(this) + sizeof(Block); }

        static Block* from_user_ptr(void* ptr)
        {
            if (!ptr)
                return nullptr;
            return reinterpret_cast<Block*>(reinterpret_cast<u8*>(ptr) - sizeof(Block));
        }

        size_t user_size() const { return size - sizeof(Block); }
    };

    struct Chunk {
        void* base { nullptr };
        size_t size { 0 };
        Block* first_block { nullptr };

        IntrusiveListNode<Chunk> list_node;
        using List = IntrusiveList<&Chunk::list_node>;
    };

    class AllocatedRangeIterator {
    public:
        struct Range {
            void* start;
            size_t size;
        };

        AllocatedRangeIterator(Chunk::List const& chunks)
            : m_chunks(chunks)
        {
            if (!m_chunks.is_empty()) {
                m_current_chunk = m_chunks.begin();
                m_current_block = m_current_chunk->first_block;
                advance_to_next_allocation();
            }
        }

        struct EndIterator {};

        AllocatedRangeIterator& operator++()
        {
            m_current_block = m_current_block->next;
            advance_to_next_allocation();
            return *this;
        }

        Range operator*() const { return { .start = m_current_block->user_ptr(), .size = m_current_block->user_size() }; }

        bool operator==(EndIterator) const { return m_current_chunk == m_chunks.end(); }

        AllocatedRangeIterator& begin() { return *this; }
        EndIterator end() { return {}; }

    private:
        void advance_to_next_allocation()
        {
            while (m_current_chunk != m_chunks.end()) {
                while (m_current_block && m_current_block->is_free) {
                    m_current_block = m_current_block->next;
                }

                if (m_current_block && !m_current_block->is_free)
                    return;

                ++m_current_chunk;
                if (m_current_chunk != m_chunks.end()) {
                    m_current_block = m_current_chunk->first_block;
                } else {
                    m_current_block = nullptr;
                }
            }
        }

        Chunk::List const& m_chunks;
        Chunk::List::ConstIterator m_current_chunk { nullptr };
        Block* m_current_block { nullptr };
    };

    explicit TracingAllocator(size_t chunk_size = DefaultChunkSize)
        : m_chunk_size(chunk_size)
    {
        ASSERT(m_chunk_size >= 1 * MiB);
        ASSERT(is_power_of_two(m_chunk_size));
    }

    ~TracingAllocator()
    {
        while (!m_chunks.is_empty()) {
            auto* chunk = m_chunks.first();
            m_chunks.remove(*chunk);
            free_chunk(chunk);
        }
    }

    void* allocate(size_t size)
    {
        if (size == 0)
            return nullptr;

        size = align_up(size + sizeof(Block), AllocationAlignment);
        auto* block = find_free_block(size);

        if (!block) {
            auto* new_chunk = allocate_chunk();
            if (!new_chunk)
                return nullptr;

            block = new_chunk->first_block;
        }

        if (block->size > size + sizeof(Block) + MinAllocationSize)
            split_block(block, size);

        block->is_free = false;
        m_allocated_bytes += block->size;
        m_allocation_count++;

        return block->user_ptr();
    }

    void deallocate(void* ptr)
    {
        if (!ptr)
            return;

        Block* block = Block::from_user_ptr(ptr);
        VERIFY(block->magic == BlockMagic);
        VERIFY(!block->is_free);

        block->is_free = true;
        m_allocated_bytes -= block->size;
        m_allocation_count--;

        coalesce_block(block);
    }

    AllocatedRangeIterator allocated_ranges() const
    {
        return AllocatedRangeIterator(m_chunks);
    }

    size_t allocated_bytes() const { return m_allocated_bytes; }
    size_t total_bytes() const { return m_total_bytes; }
    size_t allocation_count() const { return m_allocation_count; }
    size_t chunk_count() const { return m_chunks.size_slow(); }

    void dump_allocated_ranges() const
    {
        dbgln("TracingAllocator: Allocated ranges:");
        dbgln("  Total: {} bytes in {} allocations", m_allocated_bytes, m_allocation_count);

        size_t index = 0;
        for (auto [start, size] : allocated_ranges())
            dbgln("  [{}] Address: {:p}, Size: {} bytes", index++, start, size);
    }

private:
    static constexpr size_t align_up(size_t size, size_t alignment) { return (size + alignment - 1) & ~(alignment - 1); }

    Chunk* allocate_chunk()
    {
        void* new_chunk_memory = nullptr;

#ifdef AK_OS_SERENITY
        new_chunk_memory = serenity_mmap(nullptr, m_chunk_size,
            PROT_READ | PROT_WRITE,
            MAP_ANONYMOUS | MAP_RANDOMIZED | MAP_PRIVATE,
            0, 0, m_chunk_size, "TracingAllocatorChunk");
#elif defined(AK_OS_WINDOWS)
        new_chunk_memory = VirtualAlloc(NULL, m_chunk_size, MEM_COMMIT, PAGE_READWRITE);
#else
        new_chunk_memory = mmap(nullptr, m_chunk_size,
            PROT_READ | PROT_WRITE,
            MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#endif

        if (!new_chunk_memory || new_chunk_memory == MAP_FAILED)
            return nullptr;

        auto* chunk = new (new_chunk_memory) Chunk;
        chunk->base = new_chunk_memory;
        chunk->size = m_chunk_size;

        auto* first_block = bit_cast<Block*>(static_cast<u8*>(new_chunk_memory) + sizeof(Chunk));
        first_block->next = nullptr;
        first_block->prev = nullptr;
        first_block->size = m_chunk_size - sizeof(Chunk);
        first_block->is_free = true;
        first_block->magic = BlockMagic;

        chunk->first_block = first_block;

        m_chunks.append(*chunk);
        m_total_bytes += m_chunk_size;

        return chunk;
    }

    void free_chunk(Chunk* chunk)
    {
        m_total_bytes -= chunk->size;

#ifdef AK_OS_SERENITY
        munmap(chunk->base, chunk->size);
#elif defined(AK_OS_WINDOWS)
        VirtualFree(chunk->base, 0, MEM_RELEASE);
#else
        munmap(chunk->base, chunk->size);
#endif
    }

    Block* find_free_block(size_t required_size)
    {
        for (auto& chunk : m_chunks) {
            Block* block = chunk.first_block;
            while (block) {
                if (block->is_free && block->size >= required_size) {
                    return block;
                }
                block = block->next;
            }
        }
        return nullptr;
    }

    void split_block(Block* block, size_t new_size)
    {
        VERIFY(block->size > new_size + sizeof(Block));

        Block* new_block = bit_cast<Block*>(bit_cast<u8*>(block) + new_size);

        new_block->size = block->size - new_size;
        new_block->is_free = true;
        new_block->magic = BlockMagic;
        new_block->next = block->next;
        new_block->prev = block;

        if (block->next)
            block->next->prev = new_block;

        block->next = new_block;
        block->size = new_size;
    }

    void coalesce_block(Block* block)
    {
        VERIFY(block->is_free);

        if (block->next && block->next->is_free) {
            block->size += block->next->size;
            block->next = block->next->next;
            if (block->next) {
                block->next->prev = block;
            }
        }

        if (block->prev && block->prev->is_free) {
            block->prev->size += block->size;
            block->prev->next = block->next;
            if (block->next) {
                block->next->prev = block->prev;
            }
        }
    }

    size_t m_chunk_size;
    size_t m_allocated_bytes { 0 };
    size_t m_total_bytes { 0 };
    size_t m_allocation_count { 0 };
    Chunk::List m_chunks;
};

} // namespace AK

#ifdef USING_AK_GLOBALLY
using AK::TracingAllocator;
#endif
