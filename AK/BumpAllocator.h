/*
 * Copyright (c) 2021, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>
#if !defined(AK_OS_WINDOWS)
#    include <sys/mman.h>
#else
extern "C" __declspec(dllimport) void* __stdcall VirtualAlloc(void* lpAddress, size_t size, u32 flAllocationType, u32 flProtect);
extern "C" __declspec(dllimport) bool __stdcall VirtualFree(void* lpAddress, size_t size, u32 dwFreeType);
#    define MEM_COMMIT 0x00001000
#    define PAGE_READWRITE 0x04
#    define MEM_RELEASE 0x00008000
#endif

namespace AK {

template<bool use_mmap = false, size_t chunk_size = use_mmap ? 4 * MiB : 4 * KiB>
class BumpAllocator {
public:
    BumpAllocator()
    {
        if constexpr (use_mmap)
            m_chunk_size = chunk_size;
        else
            m_chunk_size = kmalloc_good_size(chunk_size);
    }

    ~BumpAllocator()
    {
        deallocate_all();
    }

    void* allocate(size_t size, size_t align)
    {
        VERIFY(size < m_chunk_size - sizeof(ChunkHeader));
        if (!m_current_chunk) {
            if (!allocate_a_chunk())
                return nullptr;
        }

    allocate_again:;
        VERIFY(m_current_chunk != 0);

        auto aligned_ptr = align_up_to(m_byte_offset_into_current_chunk + m_current_chunk, align);
        auto next_offset = aligned_ptr + size - m_current_chunk;
        if (next_offset > m_chunk_size) {
            if (!allocate_a_chunk())
                return nullptr;
            goto allocate_again;
        }
        m_byte_offset_into_current_chunk = next_offset;
        return (void*)aligned_ptr;
    }

    void deallocate_all()
    {
        if (!m_head_chunk)
            return;
        // Note that 'cache_filled' is just an educated guess, and we don't rely on it.
        // If we determine 'cache_filled=true' and the cache becomes empty in the meantime,
        // then we haven't lost much; it was a close call anyway.
        // If we determine 'cache_filled=false' and the cache becomes full in the meantime,
        // then we'll end up with a different chunk to munmap(), no big difference.
        bool cache_filled = s_unused_allocation_cache.load(MemoryOrder::memory_order_relaxed);
        for_each_chunk([&](auto chunk) {
            if (!cache_filled) {
                cache_filled = true;
                (reinterpret_cast<ChunkHeader*>(chunk))->next_chunk = 0;
                chunk = s_unused_allocation_cache.exchange(chunk);
                if (!chunk)
                    return;
                // The cache got filled in the meantime. Oh well, we have to call munmap() anyway.
            }

            if constexpr (use_mmap) {
#if defined(AK_OS_WINDOWS)
                VirtualFree((void*)chunk, m_chunk_size, MEM_RELEASE);
#else
                munmap((void*)chunk, m_chunk_size);
#endif
            } else {
                kfree_sized((void*)chunk, m_chunk_size);
            }
        });
    }

protected:
    template<typename TFn>
    void for_each_chunk(TFn&& fn)
    {
        auto head_chunk = m_head_chunk;
        while (head_chunk) {
            auto& chunk_header = *reinterpret_cast<ChunkHeader const*>(head_chunk);
            VERIFY(chunk_header.magic == chunk_magic);
            if (head_chunk == m_current_chunk)
                VERIFY(chunk_header.next_chunk == 0);
            auto next_chunk = chunk_header.next_chunk;
            fn(head_chunk);
            head_chunk = next_chunk;
        }
    }

    bool allocate_a_chunk()
    {
        // dbgln("Allocated {} entries in previous chunk and have {} unusable bytes", m_allocations_in_previous_chunk, m_chunk_size - m_byte_offset_into_current_chunk);
        // m_allocations_in_previous_chunk = 0;
        void* new_chunk = reinterpret_cast<void*>(s_unused_allocation_cache.exchange(0));
        if (!new_chunk) {
            if constexpr (use_mmap) {
#ifdef AK_OS_SERENITY
                new_chunk = serenity_mmap(nullptr, m_chunk_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_RANDOMIZED | MAP_PRIVATE, 0, 0, m_chunk_size, "BumpAllocator Chunk");
#elif defined(AK_OS_WINDOWS)
                new_chunk = VirtualAlloc(NULL, m_chunk_size, MEM_COMMIT, PAGE_READWRITE);
#else
                new_chunk = mmap(nullptr, m_chunk_size, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
#endif
#if defined(AK_OS_WINDOWS)
                if (new_chunk == NULL)
                    return false;
#else
                if (new_chunk == MAP_FAILED)
                    return false;
#endif
            } else {
                new_chunk = kmalloc(m_chunk_size);
                if (!new_chunk)
                    return false;
            }
        }

        auto& new_header = *reinterpret_cast<ChunkHeader*>(new_chunk);
        new_header.magic = chunk_magic;
        new_header.next_chunk = 0;
        m_byte_offset_into_current_chunk = sizeof(ChunkHeader);

        if (!m_head_chunk) {
            VERIFY(!m_current_chunk);
            m_head_chunk = reinterpret_cast<FlatPtr>(new_chunk);
            m_current_chunk = reinterpret_cast<FlatPtr>(new_chunk);
            return true;
        }

        VERIFY(m_current_chunk);
        auto& old_header = *reinterpret_cast<ChunkHeader*>(m_current_chunk);
        VERIFY(old_header.magic == chunk_magic);
        VERIFY(old_header.next_chunk == 0);
        old_header.next_chunk = reinterpret_cast<FlatPtr>(new_chunk);
        m_current_chunk = reinterpret_cast<FlatPtr>(new_chunk);
        return true;
    }

    constexpr static FlatPtr chunk_magic = explode_byte(0xdf);
    struct ChunkHeader {
        FlatPtr magic;
        FlatPtr next_chunk;
    };
    FlatPtr m_head_chunk { 0 };
    FlatPtr m_current_chunk { 0 };
    size_t m_byte_offset_into_current_chunk { 0 };
    size_t m_chunk_size { 0 };
    static Atomic<FlatPtr> s_unused_allocation_cache;
};

template<typename T, bool use_mmap = false, size_t chunk_size = use_mmap ? 4 * MiB : 4 * KiB>
class UniformBumpAllocator : protected BumpAllocator<use_mmap, chunk_size> {
    using Allocator = BumpAllocator<use_mmap, chunk_size>;

public:
    UniformBumpAllocator() = default;
    ~UniformBumpAllocator()
    {
        destroy_all();
    }

    template<typename... Args>
    T* allocate(Args&&... args)
    {
        auto ptr = (T*)Allocator::allocate(sizeof(T), alignof(T));
        if (!ptr)
            return nullptr;
        return new (ptr) T { forward<Args>(args)... };
    }

    void deallocate_all()
    {
        destroy_all();
        Allocator::deallocate_all();
    }

    void destroy_all()
    {
        this->for_each_chunk([&](auto chunk) {
            auto base_ptr = align_up_to(chunk + sizeof(typename Allocator::ChunkHeader), alignof(T));
            // Compute the offset of the first byte *after* this chunk:
            FlatPtr end_offset = base_ptr + this->m_chunk_size - chunk - sizeof(typename Allocator::ChunkHeader);
            if (chunk == this->m_current_chunk)
                end_offset = this->m_byte_offset_into_current_chunk;
            // Compute the offset of the first byte *after* the last valid object, in case the end of the chunk does not align with the end of an object:
            end_offset = (end_offset / sizeof(T)) * sizeof(T);
            for (; base_ptr - chunk < end_offset; base_ptr += sizeof(T))
                reinterpret_cast<T*>(base_ptr)->~T();
        });
    }
};

template<bool use_mmap, size_t size>
inline Atomic<FlatPtr> BumpAllocator<use_mmap, size>::s_unused_allocation_cache { 0 };

}

#if USING_AK_GLOBALLY
using AK::BumpAllocator;
using AK::UniformBumpAllocator;
#endif
