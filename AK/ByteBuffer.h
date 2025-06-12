/*
 * Copyright (c) 2018-2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Gunnar Beutner <gbeutner@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Badge.h>
#include <AK/Error.h>
#include <AK/Span.h>
#include <AK/Types.h>
#include <AK/kmalloc.h>

namespace AK {
namespace Detail {

template<size_t inline_capacity>
class ByteBuffer {
public:
    ByteBuffer() = default;

    ~ByteBuffer()
    {
        clear();
    }

    ByteBuffer(ByteBuffer const& other)
    {
        MUST(try_resize(other.size()));
        VERIFY(m_size == other.size());
        __builtin_memcpy(data(), other.data(), other.size());
    }

    ByteBuffer(ByteBuffer&& other)
    {
        move_from(move(other));
    }

    ByteBuffer& operator=(ByteBuffer&& other)
    {
        if (this != &other) {
            if (!m_inline)
                kfree_sized(m_outline_buffer, m_outline_capacity);
            move_from(move(other));
        }
        return *this;
    }

    ByteBuffer& operator=(ByteBuffer const& other)
    {
        if (this != &other) {
            if (m_size > other.size()) {
                trim(other.size(), true);
            } else {
                MUST(try_resize(other.size()));
            }
            __builtin_memcpy(data(), other.data(), other.size());
        }
        return *this;
    }

    [[nodiscard]] static ErrorOr<ByteBuffer> create_uninitialized(size_t size)
    {
        auto buffer = ByteBuffer();
        TRY(buffer.try_resize(size));
        return { move(buffer) };
    }

    [[nodiscard]] static ErrorOr<ByteBuffer> create_zeroed(size_t size)
    {
        auto buffer = TRY(create_uninitialized(size));

        buffer.zero_fill();
        VERIFY(size == 0 || (buffer[0] == 0 && buffer[size - 1] == 0));
        return { move(buffer) };
    }

    [[nodiscard]] static ErrorOr<ByteBuffer> copy(void const* data, size_t size)
    {
        auto buffer = TRY(create_uninitialized(size));
        if (buffer.m_inline && size > inline_capacity)
            VERIFY_NOT_REACHED();
        if (size != 0)
            __builtin_memcpy(buffer.data(), data, size);
        return { move(buffer) };
    }

    [[nodiscard]] static ErrorOr<ByteBuffer> copy(ReadonlyBytes bytes)
    {
        return copy(bytes.data(), bytes.size());
    }

    [[nodiscard]] static ErrorOr<ByteBuffer> xor_buffers(ReadonlyBytes first, ReadonlyBytes second)
    {
        if (first.size() != second.size())
            return Error::from_errno(EINVAL);

        auto buffer = TRY(create_uninitialized(first.size()));
        auto buffer_data = buffer.data();
        auto first_data = first.data();
        auto second_data = second.data();
        for (size_t i = 0; i < first.size(); ++i)
            buffer_data[i] = first_data[i] ^ second_data[i];

        return { move(buffer) };
    }

    template<size_t other_inline_capacity>
    bool operator==(ByteBuffer<other_inline_capacity> const& other) const
    {
        if (size() != other.size())
            return false;

        // So they both have data, and the same length.
        return !__builtin_memcmp(data(), other.data(), size());
    }

    [[nodiscard]] u8& operator[](size_t i)
    {
        VERIFY(i < m_size);
        return data()[i];
    }

    [[nodiscard]] u8 const& operator[](size_t i) const
    {
        VERIFY(i < m_size);
        return data()[i];
    }

    [[nodiscard]] bool is_empty() const { return m_size == 0; }
    [[nodiscard]] size_t size() const { return m_size; }

#ifdef AK_COMPILER_GCC
#    pragma GCC diagnostic push
//   Workaround for https://gcc.gnu.org/bugzilla/show_bug.cgi?id=109727
#    pragma GCC diagnostic ignored "-Warray-bounds"
#endif
    [[nodiscard]] u8* data()
    {
        return m_inline ? m_inline_buffer : m_outline_buffer;
    }
    [[nodiscard]] u8 const* data() const { return m_inline ? m_inline_buffer : m_outline_buffer; }
#ifdef AK_COMPILER_GCC
#    pragma GCC diagnostic pop
#endif

    [[nodiscard]] Bytes bytes()
    {
        return { data(), size() };
    }
    [[nodiscard]] ReadonlyBytes bytes() const { return { data(), size() }; }

    [[nodiscard]] AK::Bytes span() { return { data(), size() }; }
    [[nodiscard]] AK::ReadonlyBytes span() const { return { data(), size() }; }

    [[nodiscard]] u8* offset_pointer(size_t offset) { return data() + offset; }
    [[nodiscard]] u8 const* offset_pointer(size_t offset) const { return data() + offset; }

    [[nodiscard]] void* end_pointer() { return data() + m_size; }
    [[nodiscard]] void const* end_pointer() const { return data() + m_size; }

    [[nodiscard]] ErrorOr<ByteBuffer> slice(size_t offset, size_t size) const
    {
        // I cannot hand you a slice I don't have
        VERIFY(offset + size <= this->size());

        return copy(offset_pointer(offset), size);
    }

    void clear()
    {
        if (!m_inline) {
            kfree_sized(m_outline_buffer, m_outline_capacity);
            m_inline = true;
        }
        m_size = 0;
    }

    enum class ZeroFillNewElements {
        No,
        Yes,
    };

    ALWAYS_INLINE void resize(size_t new_size, ZeroFillNewElements zero_fill_new_elements = ZeroFillNewElements::No)
    {
        MUST(try_resize(new_size, zero_fill_new_elements));
    }

    void trim(size_t size, bool may_discard_existing_data)
    {
        VERIFY(size <= m_size);
        if (!m_inline && size <= inline_capacity)
            shrink_into_inline_buffer(size, may_discard_existing_data);
        m_size = size;
    }

    ALWAYS_INLINE void ensure_capacity(size_t new_capacity)
    {
        MUST(try_ensure_capacity(new_capacity));
    }

    void set_size(size_t new_size, ZeroFillNewElements zero_fill_new_elements = ZeroFillNewElements::No)
    {
        ASSERT(new_size <= capacity());

        if (zero_fill_new_elements == ZeroFillNewElements::Yes) {
            __builtin_memset(data() + m_size, 0, new_size - m_size);
        }

        m_size = new_size;
    }

    ErrorOr<void> try_resize(size_t new_size, ZeroFillNewElements zero_fill_new_elements = ZeroFillNewElements::No)
    {
        if (new_size <= m_size) {
            trim(new_size, false);
            return {};
        }
        TRY(try_ensure_capacity(new_size));

        set_size(new_size, zero_fill_new_elements);

        return {};
    }

    ErrorOr<void> try_ensure_capacity(size_t new_capacity)
    {
        if (new_capacity <= capacity())
            return {};
        return try_ensure_capacity_slowpath(new_capacity);
    }

    /// Return a span of bytes past the end of this ByteBuffer for writing.
    /// Ensures that the required space is available.
    ErrorOr<Bytes> get_bytes_for_writing(size_t length)
    {
        auto const old_size = size();
        TRY(try_resize(old_size + length));
        return Bytes { data() + old_size, length };
    }

    /// Like get_bytes_for_writing, but crashes if allocation fails.
    Bytes must_get_bytes_for_writing(size_t length)
    {
        return MUST(get_bytes_for_writing(length));
    }

    void append(u8 byte)
    {
        MUST(try_append(byte));
    }

    void append(ReadonlyBytes bytes)
    {
        MUST(try_append(bytes));
    }

    void append(void const* data, size_t data_size) { append({ data, data_size }); }

    ErrorOr<void> try_append(u8 byte)
    {
        auto old_size = size();
        auto new_size = old_size + 1;
        VERIFY(new_size > old_size);
        TRY(try_resize(new_size));
        data()[old_size] = byte;
        return {};
    }

    ErrorOr<void> try_append(ReadonlyBytes bytes)
    {
        return try_append(bytes.data(), bytes.size());
    }

    ErrorOr<void> try_append(void const* data, size_t data_size)
    {
        if (data_size == 0)
            return {};
        VERIFY(data != nullptr);
        auto old_size = size();
        TRY(try_resize(size() + data_size));
        __builtin_memcpy(this->data() + old_size, data, data_size);
        return {};
    }

    void operator+=(ByteBuffer const& other)
    {
        MUST(try_append(other.data(), other.size()));
    }

    void overwrite(size_t offset, void const* data, size_t data_size)
    {
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-overflow"
        // make sure we're not told to write past the end
        VERIFY(offset + data_size <= size());
        __builtin_memmove(this->data() + offset, data, data_size);
#pragma GCC diagnostic pop
    }

    void zero_fill()
    {
        __builtin_memset(data(), 0, m_size);
    }

    operator Bytes() { return bytes(); }
    operator ReadonlyBytes() const { return bytes(); }

    ALWAYS_INLINE size_t capacity() const { return m_inline ? inline_capacity : m_outline_capacity; }
    ALWAYS_INLINE bool is_inline() const { return m_inline; }

    struct OutlineBuffer {
        Bytes buffer;
        size_t capacity { 0 };
    };
    Optional<OutlineBuffer> leak_outline_buffer(Badge<StringBuilder>)
    {
        if (m_inline)
            return {};

        auto buffer = bytes();
        m_inline = true;
        m_size = 0;

        return OutlineBuffer { buffer, capacity() };
    }

private:
    void move_from(ByteBuffer&& other)
    {
        m_size = other.m_size;
        m_inline = other.m_inline;
        if (!other.m_inline) {
            m_outline_buffer = other.m_outline_buffer;
            m_outline_capacity = other.m_outline_capacity;
        } else {
            VERIFY(other.m_size <= inline_capacity);
            __builtin_memcpy(m_inline_buffer, other.m_inline_buffer, other.m_size);
        }
        other.m_size = 0;
        other.m_inline = true;
    }

    NEVER_INLINE void shrink_into_inline_buffer(size_t size, bool may_discard_existing_data)
    {
        // m_inline_buffer and m_outline_buffer are part of a union, so save the pointer
        auto* outline_buffer = m_outline_buffer;
        auto outline_capacity = m_outline_capacity;
        if (!may_discard_existing_data)
            __builtin_memcpy(m_inline_buffer, outline_buffer, size);
        kfree_sized(outline_buffer, outline_capacity);
        m_inline = true;
    }

    NEVER_INLINE ErrorOr<void> try_ensure_capacity_slowpath(size_t new_capacity)
    {
        // When we are asked to raise the capacity by very small amounts,
        // the caller is perhaps appending very little data in many calls.
        // To avoid copying the entire ByteBuffer every single time,
        // we raise the capacity exponentially, by a factor of roughly 1.5.
        // This is most noticeable in Lagom, where kmalloc_good_size is just a no-op.
        new_capacity = max(new_capacity, (capacity() * 3) / 2);
        new_capacity = kmalloc_good_size(new_capacity);
        auto* new_buffer = static_cast<u8*>(kmalloc(new_capacity));
        if (!new_buffer)
            return Error::from_errno(ENOMEM);

        if (m_inline) {
            __builtin_memcpy(new_buffer, data(), m_size);
        } else if (m_outline_buffer) {
            __builtin_memcpy(new_buffer, m_outline_buffer, min(new_capacity, m_outline_capacity));
            kfree_sized(m_outline_buffer, m_outline_capacity);
        }

        m_outline_buffer = new_buffer;
        m_outline_capacity = new_capacity;
        m_inline = false;
        return {};
    }

    union {
        u8 m_inline_buffer[inline_capacity];
        struct {
            u8* m_outline_buffer;
            size_t m_outline_capacity;
        };
    };
    size_t m_size { 0 };
    bool m_inline { true };
};

}

template<>
struct Traits<ByteBuffer> : public DefaultTraits<ByteBuffer> {
    static unsigned hash(ByteBuffer const& byte_buffer)
    {
        return Traits<ReadonlyBytes>::hash(byte_buffer.span());
    }
    static bool equals(ByteBuffer const& byte_buffer, Bytes const& other)
    {
        return byte_buffer.bytes() == other;
    }
    static bool equals(ByteBuffer const& byte_buffer, ReadonlyBytes const& other)
    {
        return byte_buffer.bytes() == other;
    }
};

}
