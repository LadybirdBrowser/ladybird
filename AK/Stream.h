/*
 * Copyright (c) 2021, sin-ack <sin-ack@protonmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/Forward.h>
#include <AK/StringView.h>
#include <AK/Traits.h>

namespace AK {

/// The base, abstract class for stream operations. This class defines the
/// operations one can perform on every stream.
/// Operations without a sensible default that are unsupported by an implementation
/// of a Stream should return EBADF as an error.
class Stream {
public:
    /// Reads into a buffer, with the maximum size being the size of the buffer.
    /// The amount of bytes read can be smaller than the size of the buffer.
    /// Returns either the bytes that were read, or an errno in the case of
    /// failure.
    virtual ErrorOr<Bytes> read_some(Bytes) = 0;
    /// Tries to fill the entire buffer through reading. Returns whether the
    /// buffer was filled without an error.
    virtual ErrorOr<void> read_until_filled(Bytes);
    /// Reads the stream until EOF, storing the contents into a ByteBuffer which
    /// is returned once EOF is encountered. The block size determines the size
    /// of newly allocated chunks while reading.
    virtual ErrorOr<ByteBuffer> read_until_eof(size_t block_size = 4096);
    /// Discards the given number of bytes from the stream. As this is usually used
    /// as an efficient version of `read_until_filled`, it returns an error
    /// if reading failed or if not all bytes could be discarded.
    /// Unless specifically overwritten, this just uses read() to read into an
    /// internal stack-based buffer.
    virtual ErrorOr<void> discard(size_t discarded_bytes);

    /// Tries to write the entire contents of the buffer. It is possible for
    /// less than the full buffer to be written. Returns either the amount of
    /// bytes written into the stream, or an errno in the case of failure.
    virtual ErrorOr<size_t> write_some(ReadonlyBytes) = 0;
    /// Same as write, but does not return until either the entire buffer
    /// contents are written or an error occurs.
    virtual ErrorOr<void> write_until_depleted(ReadonlyBytes);

    template<Concepts::AnyString T>
    ErrorOr<void> write_until_depleted(T const& buffer)
    {
        return write_until_depleted(StringView { buffer }.bytes());
    }

    template<typename T>
    requires(requires(Stream& stream) { { T::read_from_stream(stream) } -> SameAs<ErrorOr<T>>; })
    ErrorOr<T> read_value()
    {
        return T::read_from_stream(*this);
    }

    template<typename T>
    requires(Traits<T>::is_trivially_serializable())
    ErrorOr<T> read_value()
    {
        alignas(T) u8 buffer[sizeof(T)] = {};
        TRY(read_until_filled({ &buffer, sizeof(buffer) }));
        return bit_cast<T>(buffer);
    }

    template<typename T>
    requires(requires(T t, Stream& stream) { { t.write_to_stream(stream) } -> SameAs<ErrorOr<void>>; })
    ErrorOr<void> write_value(T const& value)
    {
        return value.write_to_stream(*this);
    }

    template<typename T>
    requires(Traits<T>::is_trivially_serializable())
    ErrorOr<void> write_value(T const& value)
    {
        return write_until_depleted({ &value, sizeof(value) });
    }

    template<typename... Parameters>
    ErrorOr<void> write_formatted(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
    {
        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_params { parameters... };
        TRY(write_formatted_impl(fmtstr.view(), variadic_format_params));
        return {};
    }

    /// Returns whether the stream has reached the end of file. For sockets,
    /// this most likely means that the protocol has disconnected (in the case
    /// of TCP). For seekable streams, this means the end of the file. Note that
    /// is_eof will only return true _after_ a read with 0 length, so this
    /// method should be called after a read.
    virtual bool is_eof() const = 0;

    virtual bool is_open() const = 0;
    virtual void close() = 0;

    virtual ~Stream()
    {
    }

protected:
    /// Provides a default implementation of read_until_eof that works for streams
    /// that behave like POSIX file descriptors. expected_file_size can be
    /// passed as a heuristic for what the Stream subclass expects the file
    /// content size to be in order to reduce allocations (does not affect
    /// actual reading).
    ErrorOr<ByteBuffer> read_until_eof_impl(size_t block_size, size_t expected_file_size = 0);

private:
    ErrorOr<void> write_formatted_impl(StringView, TypeErasedFormatParams&);
};

enum class SeekMode {
    SetPosition,
    FromCurrentPosition,
    FromEndPosition,
};

/// Adds seekability to a Stream. Classes inheriting from SeekableStream
/// will be seekable to any point in the stream.
class SeekableStream : public Stream {
public:
    /// Seeks to the given position in the given mode. Returns either the
    /// current position of the file, or an errno in the case of an error.
    virtual ErrorOr<size_t> seek(i64 offset, SeekMode) = 0;
    /// Returns the current position of the file, or an errno in the case of
    /// an error.
    virtual ErrorOr<size_t> tell() const;
    /// Returns the total size of the stream, or an errno in the case of an
    /// error. May not preserve the original position on the stream on failure.
    virtual ErrorOr<size_t> size();
    /// Shrinks or extends the stream to the given size. Returns an errno in
    /// the case of an error.
    virtual ErrorOr<void> truncate(size_t length) = 0;
    /// Seeks until after the given amount of bytes to be discarded instead of
    /// reading and discarding everything manually;
    virtual ErrorOr<void> discard(size_t discarded_bytes) override;
};

}

#if USING_AK_GLOBALLY
using AK::SeekMode;
#endif
