/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Checked.h>
#include <AK/NonnullRefPtr.h>
#include <AK/NumericLimits.h>
#include <AK/Span.h>
#include <AK/StringView.h>
#include <AK/Types.h>
#include <AK/Utf16View.h>
#include <AK/kmalloc.h>

namespace AK {

class Utf16String;

}

namespace AK::Detail {

void did_destroy_utf16_fly_string_data(Badge<Detail::Utf16StringData>, Detail::Utf16StringData const&);

// This is an intentionally stable representation shared with Rust. Keep all fields fixed-width,
// and update the corresponding Rust layout assertions when changing it.
struct alignas(8) Utf16StringDataHeader {
    mutable u32 reference_count { 1 };
    u32 length_in_code_units { 0 };
    mutable u32 length_in_code_points { NumericLimits<u32>::max() };
    mutable u32 hash { 0 };
    mutable u32 flags { 0 };
};

static_assert(sizeof(Utf16StringDataHeader) == 24);
static_assert(alignof(Utf16StringDataHeader) == 8);
static_assert(offsetof(Utf16StringDataHeader, reference_count) == 0);
static_assert(offsetof(Utf16StringDataHeader, length_in_code_units) == 4);
static_assert(offsetof(Utf16StringDataHeader, length_in_code_points) == 8);
static_assert(offsetof(Utf16StringDataHeader, hash) == 12);
static_assert(offsetof(Utf16StringDataHeader, flags) == 16);

class Utf16StringData final {
    AK_MAKE_NONCOPYABLE(Utf16StringData);
    AK_MAKE_NONMOVABLE(Utf16StringData);

public:
    using RefCountType = u32;
    using AllowOwnPtr = FalseType;

    enum class StorageType : u8 {
        ASCII,
        UTF16,
    };

    enum class AllowASCIIStorage : u8 {
        No,
        Yes,
    };

    enum Flag : u32 {
        HasUtf16Storage = 1 << 0,
        HasHash = 1 << 1,
        IsFlyString = 1 << 2,
    };

    static_assert(HasUtf16Storage == 1);
    static_assert(HasHash == 2);
    static_assert(IsFlyString == 4);

    static NonnullRefPtr<Utf16StringData> from_utf8(StringView, AllowASCIIStorage);
    static NonnullRefPtr<Utf16StringData> from_ascii(ReadonlyBytes);
    static NonnullRefPtr<Utf16StringData> from_utf16(Utf16View const&);
    static NonnullRefPtr<Utf16StringData> from_string_builder(Utf16StringBuilder&);
    static ErrorOr<NonnullRefPtr<Utf16StringData>> from_ipc_stream(Stream&, size_t length_in_code_units, bool is_ascii);

    // Creates the stable shared header and reserves trailing character storage for an FFI caller
    // to initialize before publishing the resulting string.
    static NonnullRefPtr<Utf16StringData> create_uninitialized_for_ffi(StorageType storage_type, size_t code_unit_length)
    {
        return create_uninitialized(storage_type, code_unit_length);
    }

    static NonnullRefPtr<Utf16StringData> to_well_formed(Utf16View const&);

    ~Utf16StringData()
    {
        VERIFY(ref_count() == 0);
        if (is_fly_string())
            did_destroy_utf16_fly_string_data({}, *this);
    }

    ALWAYS_INLINE void ref() const
    {
        auto old_reference_count = atomic_fetch_add(&m_header.reference_count, 1u, memory_order_relaxed);
        VERIFY(old_reference_count > 0);
        VERIFY(!Checked<RefCountType>::addition_would_overflow(old_reference_count, 1));
    }

    [[nodiscard]] bool try_ref() const
    {
        auto expected = atomic_load(&m_header.reference_count, memory_order_relaxed);
        for (;;) {
            if (expected == 0)
                return false;
            VERIFY(!Checked<RefCountType>::addition_would_overflow(expected, 1));
            if (atomic_compare_exchange_strong(&m_header.reference_count, expected, expected + 1, memory_order_acquire))
                return true;
        }
    }

    ALWAYS_INLINE bool unref() const
    {
        auto old_reference_count = atomic_fetch_sub(&m_header.reference_count, 1u, memory_order_release);
        VERIFY(old_reference_count > 0);
        if (old_reference_count != 1)
            return false;

        atomic_thread_fence(memory_order_acquire);
        delete this;
        return true;
    }

    [[nodiscard]] RefCountType ref_count() const { return atomic_load(&m_header.reference_count, memory_order_relaxed); }

    [[nodiscard]] static constexpr size_t offset_of_string_storage()
    {
        return sizeof(Utf16StringDataHeader);
    }

    void operator delete(void* ptr)
    {
        kfree(ptr);
    }

    [[nodiscard]] ALWAYS_INLINE bool operator==(Utf16StringData const& other) const
    {
        if (is_fly_string() && other.is_fly_string())
            return this == &other;
        return utf16_view() == other.utf16_view();
    }

    [[nodiscard]] ALWAYS_INLINE bool operator==(Utf16View const& other) const
    {
        return utf16_view() == other;
    }

    [[nodiscard]] ALWAYS_INLINE bool operator==(StringView const& other) const
    {
        if (has_ascii_storage())
            return ascii_view() == other;
        return utf16_view() == Utf16View { other.characters_without_null_termination(), other.length() };
    }

    [[nodiscard]] ALWAYS_INLINE bool has_ascii_storage() const { return !has_flag(HasUtf16Storage); }
    [[nodiscard]] ALWAYS_INLINE bool has_utf16_storage() const { return has_flag(HasUtf16Storage); }

    ALWAYS_INLINE u32 hash() const
    {
        if (!has_flag(HasHash)) {
            auto hash = utf16_view().hash();
            atomic_store(&m_header.hash, hash, memory_order_relaxed);
            atomic_fetch_or(&m_header.flags, static_cast<u32>(HasHash), memory_order_release);
            return hash;
        }

        return atomic_load(&m_header.hash, memory_order_relaxed);
    }

    [[nodiscard]] ALWAYS_INLINE size_t length_in_code_units() const { return m_header.length_in_code_units; }
    [[nodiscard]] ALWAYS_INLINE size_t length_in_code_points() const
    {
        if (has_ascii_storage())
            return length_in_code_units();

        auto length_in_code_points = atomic_load(&m_header.length_in_code_points, memory_order_acquire);
        if (length_in_code_points != NumericLimits<u32>::max())
            return length_in_code_points;

        auto calculated_length = calculate_code_point_length();
        VERIFY(calculated_length < NumericLimits<u32>::max());

        auto expected = NumericLimits<u32>::max();
        if (atomic_compare_exchange_strong(&m_header.length_in_code_points, expected, static_cast<u32>(calculated_length), memory_order_acq_rel))
            return calculated_length;
        return expected;
    }

    [[nodiscard]] ALWAYS_INLINE StringView ascii_view() const LIFETIME_BOUND
    {
        ASSERT(has_ascii_storage());
        return { ascii_data(), length_in_code_units() };
    }

    [[nodiscard]] ALWAYS_INLINE Utf16View utf16_view() const LIFETIME_BOUND
    {
        if (has_ascii_storage())
            return { ascii_data(), length_in_code_units() };

        Utf16View view { utf16_data(), length_in_code_units() };
        auto length_in_code_points = atomic_load(&m_header.length_in_code_points, memory_order_acquire);
        if (length_in_code_points != NumericLimits<u32>::max())
            view.m_length_in_code_points = length_in_code_points;

        return view;
    }

    ALWAYS_INLINE void mark_as_fly_string(Badge<Utf16FlyString>) const { atomic_fetch_or(&m_header.flags, static_cast<u32>(IsFlyString), memory_order_release); }
    [[nodiscard]] ALWAYS_INLINE bool is_fly_string() const { return has_flag(IsFlyString); }

private:
    friend class AK::Utf16String;

    ALWAYS_INLINE Utf16StringData(StorageType storage_type, size_t code_unit_length)
    {
        VERIFY(code_unit_length < NumericLimits<u32>::max());
        m_header.length_in_code_units = static_cast<u32>(code_unit_length);
        if (storage_type == StorageType::UTF16)
            m_header.flags = HasUtf16Storage;
    }

    static NonnullRefPtr<Utf16StringData> create_uninitialized(StorageType storage_type, size_t code_unit_length);
    static NonnullRefPtr<Utf16StringData> create_uninitialized_ascii(size_t length_in_code_units, Bytes& buffer);

    template<typename ViewType>
    static NonnullRefPtr<Utf16StringData> create_from_code_point_iterable(ViewType const&);

    [[nodiscard]] static constexpr size_t allocation_size_for_string_data(bool has_ascii_storage, size_t code_unit_length)
    {
        return has_ascii_storage
            ? sizeof(Utf16StringData) + (sizeof(char) * code_unit_length)
            : sizeof(Utf16StringData) + (sizeof(char16_t) * code_unit_length);
    }

    [[nodiscard]] size_t calculate_code_point_length() const;

    [[nodiscard]] ALWAYS_INLINE bool has_flag(Flag flag) const
    {
        return atomic_load(&m_header.flags, memory_order_acquire) & static_cast<u32>(flag);
    }

    [[nodiscard]] ALWAYS_INLINE char* ascii_data()
    {
        return reinterpret_cast<char*>(this) + offset_of_string_storage();
    }

    [[nodiscard]] ALWAYS_INLINE char const* ascii_data() const
    {
        return reinterpret_cast<char const*>(this) + offset_of_string_storage();
    }

    [[nodiscard]] ALWAYS_INLINE char16_t* utf16_data()
    {
        return reinterpret_cast<char16_t*>(reinterpret_cast<char*>(this) + offset_of_string_storage());
    }

    [[nodiscard]] ALWAYS_INLINE char16_t const* utf16_data() const
    {
        return reinterpret_cast<char16_t const*>(reinterpret_cast<char const*>(this) + offset_of_string_storage());
    }

    Utf16StringDataHeader m_header;
};

static_assert(Utf16StringData::offset_of_string_storage() == sizeof(Utf16StringDataHeader));
static_assert(sizeof(Utf16StringData) == sizeof(Utf16StringDataHeader));
static_assert(alignof(Utf16StringData) == alignof(Utf16StringDataHeader));

}
