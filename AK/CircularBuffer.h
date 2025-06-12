/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/HashMap.h>
#include <AK/Noncopyable.h>
#include <AK/Vector.h>

namespace AK {

class CircularBuffer {
    AK_MAKE_NONCOPYABLE(CircularBuffer);
    AK_MAKE_DEFAULT_MOVABLE(CircularBuffer);

public:
    static ErrorOr<CircularBuffer> create_empty(size_t size);
    static ErrorOr<CircularBuffer> create_initialized(ByteBuffer);

    ~CircularBuffer() = default;

    size_t write(ReadonlyBytes bytes);
    Bytes read(Bytes bytes);
    ErrorOr<void> discard(size_t discarded_bytes);
    ErrorOr<size_t> fill_from_stream(Stream&);
    ErrorOr<size_t> flush_to_stream(Stream&);

    /// Compared to `read()`, this starts reading from an offset that is `distance` bytes
    /// before the current write pointer and allows for reading already-read data.
    ErrorOr<Bytes> read_with_seekback(Bytes bytes, size_t distance) const;

    ErrorOr<size_t> copy_from_seekback(size_t distance, size_t length);

    [[nodiscard]] size_t empty_space() const;
    [[nodiscard]] size_t used_space() const;
    [[nodiscard]] size_t capacity() const;
    [[nodiscard]] size_t seekback_limit() const;

    Optional<size_t> offset_of(StringView needle, Optional<size_t> from = {}, Optional<size_t> until = {}) const;

    void clear();

protected:
    CircularBuffer(ByteBuffer);

    [[nodiscard]] bool is_wrapping_around() const;

    [[nodiscard]] Bytes next_write_span();
    [[nodiscard]] ReadonlyBytes next_read_span(size_t offset = 0) const;
    [[nodiscard]] ReadonlyBytes next_seekback_span(size_t distance) const;

    ByteBuffer m_buffer {};

    size_t m_reading_head {};
    size_t m_used_space {};
    size_t m_seekback_limit {};
};

class SearchableCircularBuffer : public CircularBuffer {
public:
    static ErrorOr<SearchableCircularBuffer> create_empty(size_t size);
    static ErrorOr<SearchableCircularBuffer> create_initialized(ByteBuffer);

    [[nodiscard]] size_t search_limit() const;

    // These functions update the read pointer, so we need to hash any data that we have processed.
    ErrorOr<Bytes> read(Bytes bytes);
    ErrorOr<void> discard(size_t discarded_bytes);
    ErrorOr<size_t> flush_to_stream(Stream& stream);

    struct Match {
        size_t distance;
        size_t length;
    };
    /// This searches the seekback buffer (between read head and limit) for occurrences where it matches the next `length` bytes from the read buffer.
    /// Supplying any hints will only consider those distances, in case existing offsets need to be validated.
    /// Note that, since we only start searching at the read head, the length between read head and write head is excluded from the distance.
    Optional<Match> find_copy_in_seekback(size_t maximum_length, size_t minimum_length = 2);
    Optional<Match> find_copy_in_seekback(ReadonlySpan<size_t> distances, size_t maximum_length, size_t minimum_length = 2) const;

    // The chunk size for which the hash table holds hashes.
    // This is nice for users to know, as picking a minimum match length that is
    // equal or greater than this allows us to completely skip a slow memory search.
    static constexpr size_t HASH_CHUNK_SIZE = 3;

private:
    // Note: This function has a similar purpose as next_seekback_span, but they differ in their reference point.
    //       Seekback operations start counting their distance at the write head, while search operations start counting their distance at the read head.
    [[nodiscard]] ReadonlyBytes next_search_span(size_t distance) const;

    SearchableCircularBuffer(ByteBuffer);

    HashMap<unsigned, size_t> m_hash_location_map;
    HashMap<size_t, size_t> m_location_chain_map;

    ErrorOr<void> insert_location_hash(ReadonlyBytes value, size_t raw_offset);
    ErrorOr<void> hash_last_bytes(size_t count);
};

}
