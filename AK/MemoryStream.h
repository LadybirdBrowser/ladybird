/*
 * Copyright (c) 2021, kleines Filmröllchen <filmroellchen@serenityos.org>.
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/OwnPtr.h>
#include <AK/Stream.h>
#include <AK/Vector.h>

namespace AK {

/// A stream class that allows for reading/writing on a preallocated memory area
/// using a single read/write head.
class FixedMemoryStream : public SeekableStream {
public:
    enum class Mode {
        ReadOnly,
        ReadWrite,
    };

    explicit FixedMemoryStream(Bytes bytes, Mode mode = Mode::ReadWrite);
    explicit FixedMemoryStream(ReadonlyBytes bytes);

    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;
    virtual ErrorOr<void> truncate(size_t) override;
    virtual ErrorOr<Bytes> read_some(Bytes bytes) override;
    virtual ErrorOr<void> read_until_filled(Bytes bytes) override;

    virtual ErrorOr<size_t> seek(i64 offset, SeekMode seek_mode = SeekMode::SetPosition) override;

    virtual ErrorOr<size_t> write_some(ReadonlyBytes bytes) override;
    virtual ErrorOr<void> write_until_depleted(ReadonlyBytes bytes) override;

    size_t offset() const;
    size_t remaining() const;

    /// Read a value, but referring to the stream's underlying data instead of copying it.
    /// Of course, only use this if you know the lifetime of the data will exceed the value's.
    // FIXME: Would be nicer to be able to return T& but Variant (and thus ErrorOr) can't hold references.
    template<typename T>
    requires(Traits<T>::is_trivially_serializable())
    ErrorOr<T*> read_in_place()
    {
        if constexpr (!IsConst<T>) {
            if (!m_writing_enabled)
                return Error::from_string_literal("Tried to obtain a non-const reference from a read-only FixedMemoryStream");
        }

        T* value = reinterpret_cast<T*>(m_bytes.offset_pointer(m_offset));
        TRY(discard(sizeof(T)));
        return value;
    }

    /// Read a span of values, referring to the stream's underlying data instead of copying it.
    /// Of course, only use this if you know the lifetime of the data will exceed the span's.
    template<typename T>
    requires(Traits<T>::is_trivially_serializable())
    ErrorOr<Span<T>> read_in_place(size_t count)
    {
        if constexpr (!IsConst<T>) {
            if (!m_writing_enabled)
                return Error::from_string_literal("Tried to obtain a non-const span from a read-only FixedMemoryStream");
        }

        Span<T> span { reinterpret_cast<T*>(m_bytes.offset_pointer(m_offset)), count };
        TRY(discard(sizeof(T) * count));
        return span;
    }

private:
    Bytes m_bytes;
    size_t m_offset { 0 };
    bool m_writing_enabled { true };
};

/// A stream class that allows for writing to an automatically allocating memory area
/// and reading back the written data afterwards.
class AllocatingMemoryStream final : public Stream {
public:
    static constexpr size_t CHUNK_SIZE = 4096;

    void peek_some(Bytes) const;

    virtual ErrorOr<Bytes> read_some(Bytes) override;
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) override;
    virtual ErrorOr<void> discard(size_t) override;
    virtual bool is_eof() const override;
    virtual bool is_open() const override;
    virtual void close() override;

    size_t used_buffer_size() const;

    ErrorOr<Optional<size_t>> offset_of(ReadonlyBytes needle) const;

private:
    // Note: We set the inline buffer capacity to zero to make moving chunks as efficient as possible.
    using Chunk = AK::Detail::ByteBuffer<0>;

    ErrorOr<ReadonlyBytes> next_read_range(size_t read_offset) const;
    ErrorOr<Bytes> next_write_range();
    void cleanup_unused_chunks();

    Vector<Chunk> m_chunks;
    size_t m_read_offset = 0;
    size_t m_write_offset = 0;
};

}

#if USING_AK_GLOBALLY
using AK::AllocatingMemoryStream;
using AK::FixedMemoryStream;
#endif
