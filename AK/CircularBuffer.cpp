/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CircularBuffer.h>
#include <AK/MemMem.h>
#include <AK/Stream.h>

namespace AK {

CircularBuffer::CircularBuffer(ByteBuffer buffer)
    : m_buffer(move(buffer))
{
}

ErrorOr<CircularBuffer> CircularBuffer::create_empty(size_t size)
{
    auto temporary_buffer = TRY(ByteBuffer::create_uninitialized(size));

    CircularBuffer circular_buffer { move(temporary_buffer) };

    return circular_buffer;
}

ErrorOr<CircularBuffer> CircularBuffer::create_initialized(ByteBuffer buffer)
{
    CircularBuffer circular_buffer { move(buffer) };

    circular_buffer.m_used_space = circular_buffer.m_buffer.size();

    return circular_buffer;
}

size_t CircularBuffer::empty_space() const
{
    return capacity() - m_used_space;
}

size_t CircularBuffer::used_space() const
{
    return m_used_space;
}

size_t CircularBuffer::capacity() const
{
    return m_buffer.size();
}

size_t CircularBuffer::seekback_limit() const
{
    return m_seekback_limit;
}

size_t SearchableCircularBuffer::search_limit() const
{
    return m_seekback_limit - m_used_space;
}

bool CircularBuffer::is_wrapping_around() const
{
    return capacity() <= m_reading_head + m_used_space;
}

Optional<size_t> CircularBuffer::offset_of(StringView needle, Optional<size_t> from, Optional<size_t> until) const
{
    auto const read_from = from.value_or(0);
    auto const read_until = until.value_or(m_used_space);
    VERIFY(read_from <= read_until);

    Array<ReadonlyBytes, 2> spans {};
    spans[0] = next_read_span();
    auto const original_span_0_size = spans[0].size();

    if (read_from > 0)
        spans[0] = spans[0].slice(min(spans[0].size(), read_from));

    if (spans[0].size() + read_from > read_until)
        spans[0] = spans[0].trim(read_until - read_from);
    else if (is_wrapping_around())
        spans[1] = m_buffer.span().slice(max(original_span_0_size, read_from) - original_span_0_size, min(read_until, m_used_space) - original_span_0_size);

    auto maybe_found = AK::memmem(spans.begin(), spans.end(), needle.bytes());
    if (maybe_found.has_value())
        *maybe_found += read_from;

    return maybe_found;
}

void CircularBuffer::clear()
{
    m_reading_head = 0;
    m_used_space = 0;
    m_seekback_limit = 0;
}

Bytes CircularBuffer::next_write_span()
{
    if (is_wrapping_around())
        return m_buffer.span().slice(m_reading_head + m_used_space - capacity(), capacity() - m_used_space);
    return m_buffer.span().slice(m_reading_head + m_used_space, capacity() - (m_reading_head + m_used_space));
}

ReadonlyBytes CircularBuffer::next_read_span(size_t offset) const
{
    auto reading_head = m_reading_head;
    auto used_space = m_used_space;

    if (offset > 0) {
        if (offset >= used_space)
            return Bytes {};

        reading_head = (reading_head + offset) % capacity();
        used_space -= offset;
    }

    return m_buffer.span().slice(reading_head, min(capacity() - reading_head, used_space));
}

ReadonlyBytes CircularBuffer::next_seekback_span(size_t distance) const
{
    VERIFY(m_seekback_limit <= capacity());
    VERIFY(distance <= m_seekback_limit);

    // Note: We are adding the capacity once here to ensure that we can wrap around the negative space by using modulo.
    auto read_offset = (capacity() + m_reading_head + m_used_space - distance) % capacity();

    return m_buffer.span().slice(read_offset, min(capacity() - read_offset, distance));
}

ReadonlyBytes SearchableCircularBuffer::next_search_span(size_t distance) const
{
    VERIFY(search_limit() <= capacity());
    VERIFY(distance <= search_limit());

    // Note: We are adding the capacity once here to ensure that we can wrap around the negative space by using modulo.
    auto read_offset = (capacity() + m_reading_head - distance) % capacity();

    return m_buffer.span().slice(read_offset, min(capacity() - read_offset, distance));
}

size_t CircularBuffer::write(ReadonlyBytes bytes)
{
    auto remaining = bytes.size();

    while (remaining > 0) {
        auto const next_span = next_write_span();
        if (next_span.size() == 0)
            break;

        auto const written_bytes = bytes.slice(bytes.size() - remaining).copy_trimmed_to(next_span);

        m_used_space += written_bytes;

        m_seekback_limit += written_bytes;
        if (m_seekback_limit > capacity())
            m_seekback_limit = capacity();

        remaining -= written_bytes;
    }

    return bytes.size() - remaining;
}

Bytes CircularBuffer::read(Bytes bytes)
{
    auto remaining = bytes.size();

    while (remaining > 0) {
        auto const next_span = next_read_span();
        if (next_span.size() == 0)
            break;

        auto written_bytes = next_span.copy_trimmed_to(bytes.slice(bytes.size() - remaining));

        m_used_space -= written_bytes;
        m_reading_head += written_bytes;

        if (m_reading_head >= capacity())
            m_reading_head -= capacity();

        remaining -= written_bytes;
    }

    return bytes.trim(bytes.size() - remaining);
}

ErrorOr<Bytes> CircularBuffer::read_with_seekback(Bytes bytes, size_t distance) const
{
    if (distance > m_seekback_limit)
        return Error::from_string_literal("Tried a seekback read beyond the seekback limit");

    auto remaining = bytes.size();

    while (remaining > 0) {
        auto const next_span = next_seekback_span(distance);
        if (next_span.size() == 0)
            break;

        auto written_bytes = next_span.copy_trimmed_to(bytes.slice(bytes.size() - remaining));

        distance -= written_bytes;
        remaining -= written_bytes;
    }

    return bytes.trim(bytes.size() - remaining);
}

ErrorOr<void> CircularBuffer::discard(size_t discarding_size)
{
    if (m_used_space < discarding_size)
        return Error::from_string_literal("Can not discard more data than what the buffer contains");
    m_used_space -= discarding_size;
    m_reading_head = (m_reading_head + discarding_size) % capacity();

    return {};
}

ErrorOr<size_t> CircularBuffer::fill_from_stream(Stream& stream)
{
    auto next_span = next_write_span();
    if (next_span.size() == 0)
        return 0;

    auto bytes = TRY(stream.read_some(next_span));
    m_used_space += bytes.size();

    m_seekback_limit += bytes.size();
    if (m_seekback_limit > capacity())
        m_seekback_limit = capacity();

    return bytes.size();
}

ErrorOr<size_t> CircularBuffer::flush_to_stream(Stream& stream)
{
    auto next_span = next_read_span();
    if (next_span.size() == 0)
        return 0;

    auto written_bytes = TRY(stream.write_some(next_span));

    m_used_space -= written_bytes;
    m_reading_head += written_bytes;

    if (m_reading_head >= capacity())
        m_reading_head -= capacity();

    return written_bytes;
}

ErrorOr<size_t> CircularBuffer::copy_from_seekback(size_t distance, size_t length)
{
    if (distance > m_seekback_limit)
        return Error::from_string_literal("Tried a seekback copy beyond the seekback limit");

    auto remaining_length = length;
    while (remaining_length > 0) {
        if (empty_space() == 0)
            break;

        auto next_span = next_seekback_span(distance);
        if (next_span.size() == 0)
            break;

        auto length_written = write(next_span.trim(remaining_length));
        remaining_length -= length_written;

        // If we copied right from the end of the seekback area (i.e. our length is larger than the distance)
        // and the last copy was one complete "chunk", we can now double the distance to copy twice as much data in one go.
        if (remaining_length > distance && length_written == distance)
            distance *= 2;
    }

    return length - remaining_length;
}

SearchableCircularBuffer::SearchableCircularBuffer(ByteBuffer buffer)
    : CircularBuffer(move(buffer))
{
}

ErrorOr<SearchableCircularBuffer> SearchableCircularBuffer::create_empty(size_t size)
{
    auto temporary_buffer = TRY(ByteBuffer::create_uninitialized(size));

    SearchableCircularBuffer circular_buffer { move(temporary_buffer) };

    return circular_buffer;
}

ErrorOr<SearchableCircularBuffer> SearchableCircularBuffer::create_initialized(ByteBuffer buffer)
{
    SearchableCircularBuffer circular_buffer { move(buffer) };

    circular_buffer.m_used_space = circular_buffer.m_buffer.size();

    for (size_t i = 0; i + HASH_CHUNK_SIZE <= circular_buffer.m_buffer.size(); i++)
        TRY(circular_buffer.insert_location_hash(circular_buffer.m_buffer.span().slice(i, HASH_CHUNK_SIZE), i));

    return circular_buffer;
}

ErrorOr<Bytes> SearchableCircularBuffer::read(Bytes bytes)
{
    auto read_bytes_span = CircularBuffer::read(bytes);
    TRY(hash_last_bytes(read_bytes_span.size()));
    return read_bytes_span;
}

ErrorOr<void> SearchableCircularBuffer::discard(size_t discarded_bytes)
{
    TRY(CircularBuffer::discard(discarded_bytes));
    TRY(hash_last_bytes(discarded_bytes));
    return {};
}

ErrorOr<size_t> SearchableCircularBuffer::flush_to_stream(Stream& stream)
{
    auto flushed_byte_count = TRY(CircularBuffer::flush_to_stream(stream));
    TRY(hash_last_bytes(flushed_byte_count));
    return flushed_byte_count;
}

Optional<SearchableCircularBuffer::Match> SearchableCircularBuffer::find_copy_in_seekback(size_t maximum_length, size_t minimum_length)
{
    VERIFY(minimum_length > 0);

    // Clip the maximum length to the amount of data that we actually store.
    if (maximum_length > m_used_space)
        maximum_length = m_used_space;

    if (maximum_length < minimum_length)
        return {};

    Optional<Match> best_match;

    Array<u8, HASH_CHUNK_SIZE> needle_storage;
    auto needle = needle_storage.span().trim(min(HASH_CHUNK_SIZE, maximum_length));

    {
        auto needle_read_bytes = MUST(read_with_seekback(needle, used_space()));
        VERIFY(needle_read_bytes.size() == needle.size());
    }

    // Try an efficient hash-based search first.
    if (needle.size() >= HASH_CHUNK_SIZE) {
        auto needle_hash = StringView { needle }.hash();

        auto maybe_starting_offset = m_hash_location_map.get(needle_hash);

        if (maybe_starting_offset.has_value()) {
            Optional<size_t> previous_buffer_offset;
            auto current_buffer_offset = maybe_starting_offset.value();

            while (true) {
                auto current_search_offset = (capacity() + m_reading_head - current_buffer_offset) % capacity();

                // Validate the hash. In case it is invalid, we can discard the rest of the chain, as the data (and everything older) got updated.
                Array<u8, HASH_CHUNK_SIZE> hash_chunk_at_offset;
                auto hash_chunk_at_offset_span = MUST(read_with_seekback(hash_chunk_at_offset, current_search_offset + used_space()));
                VERIFY(hash_chunk_at_offset_span.size() == HASH_CHUNK_SIZE);
                auto found_chunk_hash = StringView { hash_chunk_at_offset }.hash();
                if (needle_hash != found_chunk_hash) {
                    if (!previous_buffer_offset.has_value())
                        m_hash_location_map.remove(needle_hash);
                    else
                        m_location_chain_map.remove(*previous_buffer_offset);
                    break;
                }

                // Validate the match through the set-distance-based implementation.
                auto maybe_new_match = find_copy_in_seekback(Array { current_search_offset }, maximum_length, HASH_CHUNK_SIZE);

                // If we found a match, record it.
                // If we haven't found a match, we simply got a hash collision, so skip.
                if (maybe_new_match.has_value()) {
                    auto new_match = maybe_new_match.release_value();

                    if (!best_match.has_value() || best_match->length < new_match.length) {
                        best_match = new_match;

                        // If we already found a result with the best possible length, then stop searching.
                        if (best_match->length >= maximum_length)
                            break;
                    }
                }

                // Get the next location with the same hash from the location chain.
                auto maybe_next_buffer_offset = m_location_chain_map.get(current_buffer_offset);

                // End of the chain, nothing more to check.
                if (!maybe_next_buffer_offset.has_value())
                    break;

                previous_buffer_offset = current_buffer_offset;
                current_buffer_offset = maybe_next_buffer_offset.release_value();
            }

            // If we found a match, return it now.
            if (best_match.has_value())
                return best_match;
        }
    }

    // Try a plain memory search for smaller values.
    // Note: This overlaps with the hash search for chunks of size HASH_CHUNK_SIZE for the purpose of validation.
    if (minimum_length <= HASH_CHUNK_SIZE) {
        size_t haystack_offset_from_start = 0;
        Vector<ReadonlyBytes, 2> haystack;
        haystack.append(next_search_span(search_limit()));
        if (haystack[0].size() < search_limit())
            haystack.append(next_search_span(search_limit() - haystack[0].size()));

        // TODO: `memmem` searches the memory in "natural" order, which means that it finds matches with a greater distance first.
        //       Hash-based searching finds the shortest distances first, which is most likely better for encoding and memory efficiency.
        //       Look into creating a `memmem_reverse`, which starts searching from the end.
        auto memmem_match = AK::memmem(haystack.begin(), haystack.end(), needle);
        while (memmem_match.has_value()) {
            auto match_offset = memmem_match.release_value();
            auto corrected_match_distance = search_limit() - haystack_offset_from_start - match_offset;

            // Validate the match through the set-distance-based implementation and extend it to the largest size possible.
            auto maybe_new_match = find_copy_in_seekback(Array { corrected_match_distance }, min(maximum_length, HASH_CHUNK_SIZE), minimum_length);

            // If we weren't able to validate the match at all, either our memmem search returned garbage or our validation function is incorrect. Investigate.
            VERIFY(maybe_new_match.has_value());

            auto new_match = maybe_new_match.release_value();

            if (!best_match.has_value() || best_match->length < new_match.length) {
                best_match = new_match;

                // If we already found a result with the best possible length, then stop searching.
                if (best_match->length >= maximum_length)
                    break;
            }

            auto size_to_discard = match_offset + 1;

            // Trim away the already processed bytes from the haystack.
            haystack_offset_from_start += size_to_discard;
            while (size_to_discard > 0) {
                if (haystack[0].size() < size_to_discard) {
                    size_to_discard -= haystack[0].size();
                    haystack.remove(0);
                } else {
                    haystack[0] = haystack[0].slice(size_to_discard);
                    break;
                }
            }

            if (haystack.size() == 0)
                break;

            // Try and find the next match.
            memmem_match = AK::memmem(haystack.begin(), haystack.end(), needle);
        }

        // If we found a match of size HASH_CHUNK_SIZE, we should have already found that using the hash search. Investigate.
        VERIFY(!best_match.has_value() || best_match->length < HASH_CHUNK_SIZE);
    }

    return best_match;
}

Optional<SearchableCircularBuffer::Match> SearchableCircularBuffer::find_copy_in_seekback(ReadonlySpan<size_t> distances, size_t maximum_length, size_t minimum_length) const
{
    VERIFY(minimum_length > 0);

    // Clip the maximum length to the amount of data that we actually store.
    if (maximum_length > m_used_space)
        maximum_length = m_used_space;

    if (maximum_length < minimum_length)
        return Optional<Match> {};

    Optional<Match> best_match;

    for (auto distance : distances) {
        // Discard distances outside the valid range.
        if (distance > search_limit() || distance <= 0)
            continue;

        // TODO: This does not yet support looping repetitions.
        if (distance < minimum_length)
            continue;

        auto current_match_length = 0ul;

        while (current_match_length < maximum_length) {
            auto haystack = next_search_span(distance - current_match_length).trim(maximum_length - current_match_length);
            auto needle = next_read_span(current_match_length).trim(maximum_length - current_match_length);

            auto submatch_length = haystack.matching_prefix_length(needle);

            if (submatch_length == 0)
                break;

            current_match_length += submatch_length;
        }

        // Discard matches that don't reach the minimum length.
        if (current_match_length < minimum_length)
            continue;

        if (!best_match.has_value() || best_match->length < current_match_length)
            best_match = Match { distance, current_match_length };
    }

    return best_match;
}

ErrorOr<void> SearchableCircularBuffer::insert_location_hash(ReadonlyBytes value, size_t raw_offset)
{
    VERIFY(value.size() == HASH_CHUNK_SIZE);

    auto value_hash = StringView { value }.hash();

    // Discard any old entries for this offset first. This should eliminate accidental loops by breaking the chain.
    // The actual cleanup is done on access, since we can only remove invalid references when actually walking the chain.
    m_location_chain_map.remove(raw_offset);

    // Check if we have any existing entries for this hash.
    // If so, we need to add it to the location chain map instead, as we will soon replace the entry in the hash location map.
    auto existing_entry = m_hash_location_map.get(value_hash);

    if (existing_entry.has_value())
        TRY(m_location_chain_map.try_set(raw_offset, existing_entry.value()));

    TRY(m_hash_location_map.try_set(value_hash, raw_offset));

    return {};
}

ErrorOr<void> SearchableCircularBuffer::hash_last_bytes(size_t count)
{
    // Stop early if we don't have enough data overall to hash a full chunk.
    if (search_limit() < HASH_CHUNK_SIZE)
        return {};

    auto remaining_recalculations = count;
    while (remaining_recalculations > 0) {
        // Note: We offset everything by HASH_CHUNK_SIZE because we have up to HASH_CHUNK_SIZE - 1 bytes that we couldn't hash before (as we had missing data).
        //       The number of recalculations stays the same, since we now have up to HASH_CHUNK_SIZE - 1 bytes that we can't hash now.
        auto recalculation_span = next_search_span(min(remaining_recalculations + HASH_CHUNK_SIZE - 1, search_limit()));

        // If the span is smaller than a hash chunk, we need to manually craft some consecutive data to do the hashing.
        if (recalculation_span.size() < HASH_CHUNK_SIZE) {
            auto auxiliary_span = next_seekback_span(remaining_recalculations);

            // Ensure that our math is correct and that both spans are "adjacent".
            VERIFY(recalculation_span.data() + recalculation_span.size() == m_buffer.data() + m_buffer.size());
            VERIFY(auxiliary_span.data() == m_buffer.data());

            while (recalculation_span.size() > 0 && recalculation_span.size() + auxiliary_span.size() >= HASH_CHUNK_SIZE) {
                Array<u8, HASH_CHUNK_SIZE> temporary_hash_chunk;

                auto copied_from_recalculation_span = recalculation_span.copy_to(temporary_hash_chunk);
                VERIFY(copied_from_recalculation_span == recalculation_span.size());

                auto copied_from_auxiliary_span = auxiliary_span.copy_to(temporary_hash_chunk.span().slice(copied_from_recalculation_span));
                VERIFY(copied_from_recalculation_span + copied_from_auxiliary_span == HASH_CHUNK_SIZE);

                TRY(insert_location_hash(temporary_hash_chunk, recalculation_span.data() - m_buffer.data()));

                recalculation_span = recalculation_span.slice(1);
                remaining_recalculations--;
            }

            continue;
        }

        for (size_t i = 0; i + HASH_CHUNK_SIZE <= recalculation_span.size(); i++) {
            auto value = recalculation_span.slice(i, HASH_CHUNK_SIZE);
            auto raw_offset = value.data() - m_buffer.data();
            TRY(insert_location_hash(value, raw_offset));
            remaining_recalculations--;
        }
    }

    return {};
}

}
