/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2021, Idan Horowitz <idan.horowitz@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Assertions.h>
#include <AK/BinarySearch.h>
#include <AK/MemoryStream.h>
#include <string.h>

#include <LibCompress/Deflate.h>
#include <LibCompress/Huffman.h>

namespace Compress {

static constexpr u8 deflate_special_code_length_copy = 16;
static constexpr u8 deflate_special_code_length_zeros = 17;
static constexpr u8 deflate_special_code_length_long_zeros = 18;

static constexpr int EndOfBlock = 256;

CanonicalCode const& CanonicalCode::fixed_literal_codes()
{
    static CanonicalCode code;
    static bool initialized = false;

    if (initialized)
        return code;

    code = MUST(CanonicalCode::from_bytes(fixed_literal_bit_lengths));
    initialized = true;

    return code;
}

CanonicalCode const& CanonicalCode::fixed_distance_codes()
{
    static CanonicalCode code;
    static bool initialized = false;

    if (initialized)
        return code;

    code = MUST(CanonicalCode::from_bytes(fixed_distance_bit_lengths));
    initialized = true;

    return code;
}

ErrorOr<CanonicalCode> CanonicalCode::from_bytes(ReadonlyBytes bytes)
{
    CanonicalCode code;

    auto non_zero_symbols = 0;
    auto last_non_zero = -1;
    for (size_t i = 0; i < bytes.size(); i++) {
        if (bytes[i] != 0) {
            non_zero_symbols++;
            last_non_zero = i;
        }
    }

    if (non_zero_symbols == 1) { // special case - only 1 symbol
        code.m_prefix_table[0] = PrefixTableEntry { static_cast<u16>(last_non_zero), 1u };
        code.m_prefix_table[1] = code.m_prefix_table[0];
        code.m_max_prefixed_code_length = 1;

        if (code.m_bit_codes.size() < static_cast<size_t>(last_non_zero + 1)) {
            TRY(code.m_bit_codes.try_resize(last_non_zero + 1));
            TRY(code.m_bit_code_lengths.try_resize(last_non_zero + 1));
        }
        code.m_bit_codes[last_non_zero] = 0;
        code.m_bit_code_lengths[last_non_zero] = 1;

        return code;
    }

    struct PrefixCode {
        u16 symbol_code { 0 };
        u16 symbol_value { 0 };
        u16 code_length { 0 };
    };
    Array<PrefixCode, 1 << CanonicalCode::max_allowed_prefixed_code_length> prefix_codes;
    size_t number_of_prefix_codes = 0;

    auto next_code = 0;
    for (size_t code_length = 1; code_length <= 15; ++code_length) {
        next_code <<= 1;
        auto start_bit = 1 << code_length;

        for (size_t symbol = 0; symbol < bytes.size(); ++symbol) {
            if (bytes[symbol] != code_length)
                continue;

            if (next_code > start_bit)
                return Error::from_string_literal("Failed to decode code lengths");

            if (code_length <= CanonicalCode::max_allowed_prefixed_code_length) {
                if (number_of_prefix_codes >= prefix_codes.size())
                    return Error::from_string_literal("Invalid canonical Huffman code");

                auto& prefix_code = prefix_codes[number_of_prefix_codes++];
                prefix_code.symbol_code = next_code;
                prefix_code.symbol_value = symbol;
                prefix_code.code_length = code_length;

                code.m_max_prefixed_code_length = code_length;
            } else {
                code.m_symbol_codes.append(start_bit | next_code);
                code.m_symbol_values.append(symbol);
            }

            if (code.m_bit_codes.size() < symbol + 1) {
                TRY(code.m_bit_codes.try_resize(symbol + 1));
                TRY(code.m_bit_code_lengths.try_resize(symbol + 1));
            }
            code.m_bit_codes[symbol] = fast_reverse16(start_bit | next_code, code_length); // DEFLATE writes huffman encoded symbols as lsb-first
            code.m_bit_code_lengths[symbol] = code_length;

            next_code++;
        }
    }

    if (next_code != (1 << 15))
        return Error::from_string_literal("Failed to decode code lengths");

    for (auto [symbol_code, symbol_value, code_length] : prefix_codes) {
        if (code_length == 0 || code_length > CanonicalCode::max_allowed_prefixed_code_length)
            break;

        auto shift = code.m_max_prefixed_code_length - code_length;
        symbol_code <<= shift;

        for (size_t j = 0; j < (1u << shift); ++j) {
            auto index = fast_reverse16(symbol_code + j, code.m_max_prefixed_code_length);
            code.m_prefix_table[index] = PrefixTableEntry { symbol_value, code_length };
        }
    }

    return code;
}

ErrorOr<u32> CanonicalCode::read_symbol(LittleEndianInputBitStream& stream) const
{
    auto prefix = TRY(stream.peek_bits<size_t>(m_max_prefixed_code_length));

    if (auto [symbol_value, code_length] = m_prefix_table[prefix]; code_length != 0) {
        stream.discard_previously_peeked_bits(code_length);
        return symbol_value;
    }

    auto code_bits = TRY(stream.read_bits<u16>(m_max_prefixed_code_length));
    code_bits = fast_reverse16(code_bits, m_max_prefixed_code_length);
    code_bits |= 1 << m_max_prefixed_code_length;

    for (size_t i = m_max_prefixed_code_length; i < 16; ++i) {
        size_t index;
        if (binary_search(m_symbol_codes.span(), code_bits, &index))
            return m_symbol_values[index];

        code_bits = code_bits << 1 | TRY(stream.read_bit());
    }

    return Error::from_string_literal("Symbol exceeds maximum symbol number");
}

DeflateDecompressor::CompressedBlock::CompressedBlock(DeflateDecompressor& decompressor, CanonicalCode literal_codes, Optional<CanonicalCode> distance_codes)
    : m_decompressor(decompressor)
    , m_literal_codes(literal_codes)
    , m_distance_codes(distance_codes)
{
}

ErrorOr<bool> DeflateDecompressor::CompressedBlock::try_read_more()
{
    if (m_eof == true)
        return false;

    auto const symbol = TRY(m_literal_codes.read_symbol(*m_decompressor.m_input_stream));

    if (symbol >= 286)
        return Error::from_string_literal("Invalid deflate literal/length symbol");

    if (symbol < EndOfBlock) {
        u8 byte_symbol = symbol;
        m_decompressor.m_output_buffer.write({ &byte_symbol, sizeof(byte_symbol) });
        return true;
    }

    if (symbol == EndOfBlock) {
        m_eof = true;
        return false;
    }

    if (!m_distance_codes.has_value())
        return Error::from_string_literal("Distance codes have not been initialized");

    auto const length = TRY(m_decompressor.decode_length(symbol));
    auto const distance_symbol = TRY(m_distance_codes.value().read_symbol(*m_decompressor.m_input_stream));
    if (distance_symbol >= 30)
        return Error::from_string_literal("Invalid deflate distance symbol");

    auto const distance = TRY(m_decompressor.decode_distance(distance_symbol));

    auto copied_length = TRY(m_decompressor.m_output_buffer.copy_from_seekback(distance, length));

    // TODO: What should we do if the output buffer is full?
    VERIFY(copied_length == length);

    return true;
}

DeflateDecompressor::UncompressedBlock::UncompressedBlock(DeflateDecompressor& decompressor, size_t length)
    : m_decompressor(decompressor)
    , m_bytes_remaining(length)
{
}

ErrorOr<bool> DeflateDecompressor::UncompressedBlock::try_read_more()
{
    if (m_bytes_remaining == 0)
        return false;

    if (m_decompressor.m_input_stream->is_eof())
        return Error::from_string_literal("Input data ends in the middle of an uncompressed DEFLATE block");

    Array<u8, 4096> temporary_buffer;
    auto readable_bytes = temporary_buffer.span().trim(min(m_bytes_remaining, m_decompressor.m_output_buffer.empty_space()));
    auto read_bytes = TRY(m_decompressor.m_input_stream->read_some(readable_bytes));
    auto written_bytes = m_decompressor.m_output_buffer.write(read_bytes);
    VERIFY(read_bytes.size() == written_bytes);

    m_bytes_remaining -= written_bytes;
    return true;
}

ErrorOr<NonnullOwnPtr<DeflateDecompressor>> DeflateDecompressor::construct(MaybeOwned<LittleEndianInputBitStream> stream)
{
    auto output_buffer = TRY(CircularBuffer::create_empty(32 * KiB));
    return TRY(adopt_nonnull_own_or_enomem(new (nothrow) DeflateDecompressor(move(stream), move(output_buffer))));
}

DeflateDecompressor::DeflateDecompressor(MaybeOwned<LittleEndianInputBitStream> stream, CircularBuffer output_buffer)
    : m_input_stream(move(stream))
    , m_output_buffer(move(output_buffer))
{
}

DeflateDecompressor::~DeflateDecompressor()
{
    if (m_state == State::ReadingCompressedBlock)
        m_compressed_block.~CompressedBlock();
    if (m_state == State::ReadingUncompressedBlock)
        m_uncompressed_block.~UncompressedBlock();
}

ErrorOr<Bytes> DeflateDecompressor::read_some(Bytes bytes)
{
    size_t total_read = 0;
    while (total_read < bytes.size()) {
        auto slice = bytes.slice(total_read);

        if (m_state == State::Idle) {
            if (m_read_final_block)
                break;

            m_read_final_block = TRY(m_input_stream->read_bit());
            auto const block_type = TRY(m_input_stream->read_bits(2));

            if (block_type == 0b00) {
                m_input_stream->align_to_byte_boundary();

                u16 length = TRY(m_input_stream->read_value<LittleEndian<u16>>());
                u16 negated_length = TRY(m_input_stream->read_value<LittleEndian<u16>>());

                if ((length ^ 0xffff) != negated_length)
                    return Error::from_string_literal("Calculated negated length does not equal stored negated length");

                m_state = State::ReadingUncompressedBlock;
                new (&m_uncompressed_block) UncompressedBlock(*this, length);

                continue;
            }

            if (block_type == 0b01) {
                m_state = State::ReadingCompressedBlock;
                new (&m_compressed_block) CompressedBlock(*this, CanonicalCode::fixed_literal_codes(), CanonicalCode::fixed_distance_codes());

                continue;
            }

            if (block_type == 0b10) {
                CanonicalCode literal_codes;
                Optional<CanonicalCode> distance_codes;
                TRY(decode_codes(literal_codes, distance_codes));

                m_state = State::ReadingCompressedBlock;
                new (&m_compressed_block) CompressedBlock(*this, literal_codes, distance_codes);

                continue;
            }

            return Error::from_string_literal("Unhandled block type for Idle state");
        }

        if (m_state == State::ReadingCompressedBlock) {
            auto nread = m_output_buffer.read(slice).size();

            while (nread < slice.size() && TRY(m_compressed_block.try_read_more())) {
                nread += m_output_buffer.read(slice.slice(nread)).size();
            }

            total_read += nread;
            if (nread == slice.size())
                break;

            m_compressed_block.~CompressedBlock();
            m_state = State::Idle;

            continue;
        }

        if (m_state == State::ReadingUncompressedBlock) {
            auto nread = m_output_buffer.read(slice).size();

            while (nread < slice.size() && TRY(m_uncompressed_block.try_read_more())) {
                nread += m_output_buffer.read(slice.slice(nread)).size();
            }

            total_read += nread;
            if (nread == slice.size())
                break;

            m_uncompressed_block.~UncompressedBlock();
            m_state = State::Idle;

            continue;
        }

        VERIFY_NOT_REACHED();
    }

    return bytes.slice(0, total_read);
}

bool DeflateDecompressor::is_eof() const { return m_state == State::Idle && m_read_final_block; }

ErrorOr<size_t> DeflateDecompressor::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

bool DeflateDecompressor::is_open() const
{
    return true;
}

void DeflateDecompressor::close()
{
}

ErrorOr<ByteBuffer> DeflateDecompressor::decompress_all(ReadonlyBytes bytes)
{
    FixedMemoryStream memory_stream { bytes };
    LittleEndianInputBitStream bit_stream { MaybeOwned<Stream>(memory_stream) };
    auto deflate_stream = TRY(DeflateDecompressor::construct(MaybeOwned<LittleEndianInputBitStream>(bit_stream)));
    return deflate_stream->read_until_eof(4096);
}

ErrorOr<u32> DeflateDecompressor::decode_length(u32 symbol)
{
    if (symbol <= 264)
        return symbol - 254;

    if (symbol <= 284) {
        auto extra_bits = (symbol - 261) / 4;
        return (((symbol - 265) % 4 + 4) << extra_bits) + 3 + TRY(m_input_stream->read_bits(extra_bits));
    }

    if (symbol == 285)
        return DeflateDecompressor::max_back_reference_length;

    VERIFY_NOT_REACHED();
}

ErrorOr<u32> DeflateDecompressor::decode_distance(u32 symbol)
{
    if (symbol <= 3)
        return symbol + 1;

    if (symbol <= 29) {
        auto extra_bits = (symbol / 2) - 1;
        return ((symbol % 2 + 2) << extra_bits) + 1 + TRY(m_input_stream->read_bits(extra_bits));
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<void> DeflateDecompressor::decode_codes(CanonicalCode& literal_code, Optional<CanonicalCode>& distance_code)
{
    auto literal_code_count = TRY(m_input_stream->read_bits(5)) + 257;
    auto distance_code_count = TRY(m_input_stream->read_bits(5)) + 1;
    auto code_length_count = TRY(m_input_stream->read_bits(4)) + 4;

    // First we have to extract the code lengths of the code that was used to encode the code lengths of
    // the code that was used to encode the block.

    u8 code_lengths_code_lengths[19] = { 0 };
    for (size_t i = 0; i < code_length_count; ++i) {
        code_lengths_code_lengths[code_lengths_code_lengths_order[i]] = TRY(m_input_stream->read_bits(3));
    }

    // Now we can extract the code that was used to encode the code lengths of the code that was used to
    // encode the block.
    auto const code_length_code = TRY(CanonicalCode::from_bytes({ code_lengths_code_lengths, sizeof(code_lengths_code_lengths) }));

    // Next we extract the code lengths of the code that was used to encode the block.
    Vector<u8, 286> code_lengths;
    while (code_lengths.size() < literal_code_count + distance_code_count) {
        auto symbol = TRY(code_length_code.read_symbol(*m_input_stream));

        if (symbol < deflate_special_code_length_copy) {
            code_lengths.append(static_cast<u8>(symbol));
        } else if (symbol == deflate_special_code_length_copy) {
            if (code_lengths.is_empty())
                return Error::from_string_literal("Found no codes to copy before a copy block");
            auto nrepeat = 3 + TRY(m_input_stream->read_bits(2));
            for (size_t j = 0; j < nrepeat; ++j)
                code_lengths.append(code_lengths.last());
        } else if (symbol == deflate_special_code_length_zeros) {
            auto nrepeat = 3 + TRY(m_input_stream->read_bits(3));
            for (size_t j = 0; j < nrepeat; ++j)
                code_lengths.append(0);
        } else {
            VERIFY(symbol == deflate_special_code_length_long_zeros);
            auto nrepeat = 11 + TRY(m_input_stream->read_bits(7));
            for (size_t j = 0; j < nrepeat; ++j)
                code_lengths.append(0);
        }
    }

    if (code_lengths.size() != literal_code_count + distance_code_count)
        return Error::from_string_literal("Number of code lengths does not match the sum of codes");

    // Now we extract the code that was used to encode literals and lengths in the block.
    literal_code = TRY(CanonicalCode::from_bytes(code_lengths.span().trim(literal_code_count)));

    // Now we extract the code that was used to encode distances in the block.

    if (distance_code_count == 1) {
        auto length = code_lengths[literal_code_count];

        if (length == 0)
            return {};
        else if (length != 1)
            return Error::from_string_literal("Length for a single distance code is longer than 1");
    }

    distance_code = TRY(CanonicalCode::from_bytes(code_lengths.span().slice(literal_code_count)));

    return {};
}

ErrorOr<NonnullOwnPtr<DeflateCompressor>> DeflateCompressor::construct(MaybeOwned<Stream> stream, CompressionLevel compression_level)
{
    auto bit_stream = TRY(try_make<LittleEndianOutputBitStream>(move(stream)));
    auto deflate_compressor = TRY(adopt_nonnull_own_or_enomem(new (nothrow) DeflateCompressor(move(bit_stream), compression_level)));
    return deflate_compressor;
}

DeflateCompressor::DeflateCompressor(NonnullOwnPtr<LittleEndianOutputBitStream> stream, CompressionLevel compression_level)
    : m_compression_level(compression_level)
    , m_compression_constants(compression_constants[static_cast<int>(m_compression_level)])
    , m_output_stream(move(stream))
{
    m_symbol_frequencies.fill(0);
    m_distance_frequencies.fill(0);
}

DeflateCompressor::~DeflateCompressor() = default;

ErrorOr<Bytes> DeflateCompressor::read_some(Bytes)
{
    return Error::from_errno(EBADF);
}

ErrorOr<size_t> DeflateCompressor::write_some(ReadonlyBytes bytes)
{
    VERIFY(!m_finished);

    size_t total_written = 0;
    while (!bytes.is_empty()) {
        auto n_written = bytes.copy_trimmed_to(pending_block().slice(m_pending_block_size));
        m_pending_block_size += n_written;

        if (m_pending_block_size == block_size)
            TRY(flush());

        bytes = bytes.slice(n_written);
        total_written += n_written;
    }
    return total_written;
}

bool DeflateCompressor::is_eof() const
{
    return true;
}

bool DeflateCompressor::is_open() const
{
    return m_output_stream->is_open();
}

void DeflateCompressor::close()
{
}

// Knuth's multiplicative hash on 4 bytes
u16 DeflateCompressor::hash_sequence(u8 const* bytes)
{
    constexpr u32 const knuth_constant = 2654435761; // shares no common factors with 2^32
    return ((bytes[0] | bytes[1] << 8 | bytes[2] << 16 | bytes[3] << 24) * knuth_constant) >> (32 - hash_bits);
}

size_t DeflateCompressor::compare_match_candidate(size_t start, size_t candidate, size_t previous_match_length, size_t maximum_match_length)
{
    VERIFY(previous_match_length < maximum_match_length);

    // We firstly check that the match is at least (prev_match_length + 1) long, we check backwards as there's a higher chance the end mismatches
    for (ssize_t i = previous_match_length; i >= 0; i--) {
        if (m_rolling_window[start + i] != m_rolling_window[candidate + i])
            return 0;
    }

    // Find the actual length
    auto match_length = previous_match_length + 1;
    while (match_length < maximum_match_length && m_rolling_window[start + match_length] == m_rolling_window[candidate + match_length]) {
        match_length++;
    }

    VERIFY(match_length > previous_match_length);
    VERIFY(match_length <= maximum_match_length);
    return match_length;
}

size_t DeflateCompressor::find_back_match(size_t start, u16 hash, size_t previous_match_length, size_t maximum_match_length, size_t& match_position)
{
    auto max_chain_length = m_compression_constants.max_chain;
    if (previous_match_length == 0)
        previous_match_length = min_match_length - 1; // we only care about matches that are at least min_match_length long
    if (previous_match_length >= maximum_match_length)
        return 0; // we can't improve a maximum length match
    if (previous_match_length >= m_compression_constants.max_lazy_length)
        return 0; // the previous match is already pretty, we shouldn't waste another full search
    if (previous_match_length >= m_compression_constants.good_match_length)
        max_chain_length /= 4; // we already have a pretty good much, so do a shorter search

    auto candidate = m_hash_head[hash];
    auto match_found = false;
    while (max_chain_length--) {
        if (candidate == empty_slot)
            break; // no remaining candidates

        VERIFY(candidate < start);
        if (start - candidate > window_size)
            break; // outside the window

        auto match_length = compare_match_candidate(start, candidate, previous_match_length, maximum_match_length);

        if (match_length != 0) {
            match_found = true;
            match_position = candidate;
            previous_match_length = match_length;

            if (match_length == maximum_match_length)
                return match_length; // bail if we got the maximum possible length
        }

        candidate = m_hash_prev[candidate % window_size];
    }
    if (!match_found)
        return 0;                 // we didn't find any matches
    return previous_match_length; // we found matches, but they were at most previous_match_length long
}

ALWAYS_INLINE u8 DeflateCompressor::distance_to_base(u16 distance)
{
    return (distance <= 256) ? distance_to_base_lo[distance - 1] : distance_to_base_hi[(distance - 1) >> 7];
}

void DeflateCompressor::lz77_compress_block()
{
    for (auto& slot : m_hash_head) { // initialize chained hash table
        slot = empty_slot;
    }

    auto insert_hash = [&](auto pos, auto hash) {
        auto window_pos = pos % window_size;
        m_hash_prev[window_pos] = m_hash_head[hash];
        m_hash_head[hash] = window_pos;
    };

    auto emit_literal = [&](auto literal) {
        VERIFY(m_pending_symbol_size <= block_size + 1);
        auto index = m_pending_symbol_size++;
        m_symbol_buffer[index].distance = 0;
        m_symbol_buffer[index].literal = literal;
        m_symbol_frequencies[literal]++;
    };

    auto emit_back_reference = [&](auto distance, auto length) {
        VERIFY(m_pending_symbol_size <= block_size + 1);
        auto index = m_pending_symbol_size++;
        m_symbol_buffer[index].distance = distance;
        m_symbol_buffer[index].length = length;
        m_symbol_frequencies[length_to_symbol[length]]++;
        m_distance_frequencies[distance_to_base(distance)]++;
    };

    size_t previous_match_length = 0;
    size_t previous_match_position = 0;

    VERIFY(m_compression_constants.great_match_length <= max_match_length);

    // our block starts at block_size and is m_pending_block_size in length
    auto block_end = block_size + m_pending_block_size;
    size_t current_position;
    for (current_position = block_size; current_position < block_end - min_match_length + 1; current_position++) {
        auto hash = hash_sequence(&m_rolling_window[current_position]);
        size_t match_position;
        auto match_length = find_back_match(current_position, hash, previous_match_length,
            min(m_compression_constants.great_match_length, block_end - current_position), match_position);

        insert_hash(current_position, hash);

        // if the previous match is as good as the new match, just use it
        if (previous_match_length != 0 && previous_match_length >= match_length) {
            emit_back_reference((current_position - 1) - previous_match_position, previous_match_length);

            // skip all the bytes that are included in this match
            for (size_t j = current_position + 1; j < min(current_position - 1 + previous_match_length, block_end - min_match_length + 1); j++) {
                insert_hash(j, hash_sequence(&m_rolling_window[j]));
            }
            current_position = (current_position - 1) + previous_match_length - 1;
            previous_match_length = 0;
            continue;
        }

        if (match_length == 0) {
            VERIFY(previous_match_length == 0);
            emit_literal(m_rolling_window[current_position]);
            continue;
        }

        // if this is a lazy match, and the new match is better than the old one, output previous as literal
        if (previous_match_length != 0) {
            emit_literal(m_rolling_window[current_position - 1]);
        }

        previous_match_length = match_length;
        previous_match_position = match_position;
    }

    // clean up leftover lazy match
    if (previous_match_length != 0) {
        emit_back_reference((current_position - 1) - previous_match_position, previous_match_length);
        current_position = (current_position - 1) + previous_match_length;
    }

    // output remaining literals
    while (current_position < block_end) {
        emit_literal(m_rolling_window[current_position++]);
    }
}

size_t DeflateCompressor::huffman_block_length(Array<u8, max_huffman_literals> const& literal_bit_lengths, Array<u8, max_huffman_distances> const& distance_bit_lengths)
{
    size_t length = 0;

    for (size_t i = 0; i < 286; i++) {
        auto frequency = m_symbol_frequencies[i];
        length += literal_bit_lengths[i] * frequency;

        if (i >= 257) // back reference length symbols
            length += packed_length_symbols[i - 257].extra_bits * frequency;
    }

    for (size_t i = 0; i < 30; i++) {
        auto frequency = m_distance_frequencies[i];
        length += distance_bit_lengths[i] * frequency;
        length += packed_distances[i].extra_bits * frequency;
    }

    return length;
}

size_t DeflateCompressor::uncompressed_block_length()
{
    auto padding = 8 - ((m_output_stream->bit_offset() + 3) % 8);
    // 3 bit block header + align to byte + 2 * 16 bit length fields + block contents
    return 3 + padding + (2 * 16) + m_pending_block_size * 8;
}

size_t DeflateCompressor::fixed_block_length()
{
    // block header + fixed huffman encoded block contents
    return 3 + huffman_block_length(fixed_literal_bit_lengths, fixed_distance_bit_lengths);
}

size_t DeflateCompressor::dynamic_block_length(Array<u8, max_huffman_literals> const& literal_bit_lengths, Array<u8, max_huffman_distances> const& distance_bit_lengths, Array<u8, 19> const& code_lengths_bit_lengths, Array<u16, 19> const& code_lengths_frequencies, size_t code_lengths_count)
{
    // block header + literal code count + distance code count + code length count
    auto length = 3 + 5 + 5 + 4;

    // 3 bits per code_length
    length += 3 * code_lengths_count;

    for (size_t i = 0; i < code_lengths_frequencies.size(); i++) {
        auto frequency = code_lengths_frequencies[i];
        length += code_lengths_bit_lengths[i] * frequency;

        if (i == deflate_special_code_length_copy) {
            length += 2 * frequency;
        } else if (i == deflate_special_code_length_zeros) {
            length += 3 * frequency;
        } else if (i == deflate_special_code_length_long_zeros) {
            length += 7 * frequency;
        }
    }

    return length + huffman_block_length(literal_bit_lengths, distance_bit_lengths);
}

ErrorOr<void> DeflateCompressor::write_huffman(CanonicalCode const& literal_code, Optional<CanonicalCode> const& distance_code)
{
    auto has_distances = distance_code.has_value();
    for (size_t i = 0; i < m_pending_symbol_size; i++) {
        if (m_symbol_buffer[i].distance == 0) {
            TRY(literal_code.write_symbol(*m_output_stream, m_symbol_buffer[i].literal));
            continue;
        }
        VERIFY(has_distances);
        auto symbol = length_to_symbol[m_symbol_buffer[i].length];
        TRY(literal_code.write_symbol(*m_output_stream, symbol));
        // Emit extra bits if needed
        TRY(m_output_stream->write_bits<u16>(m_symbol_buffer[i].length - packed_length_symbols[symbol - 257].base_length, packed_length_symbols[symbol - 257].extra_bits));

        auto base_distance = distance_to_base(m_symbol_buffer[i].distance);
        TRY(distance_code.value().write_symbol(*m_output_stream, base_distance));
        // Emit extra bits if needed
        TRY(m_output_stream->write_bits<u16>(m_symbol_buffer[i].distance - packed_distances[base_distance].base_distance, packed_distances[base_distance].extra_bits));
    }
    return {};
}

size_t DeflateCompressor::encode_huffman_lengths(ReadonlyBytes lengths, Array<code_length_symbol, max_huffman_literals + max_huffman_distances>& encoded_lengths)
{
    size_t encoded_count = 0;
    size_t i = 0;
    while (i < lengths.size()) {
        if (lengths[i] == 0) {
            auto zero_count = 0;
            for (size_t j = i; j < min(lengths.size(), i + 138) && lengths[j] == 0; j++)
                zero_count++;

            if (zero_count < 3) { // below minimum repeated zero count
                encoded_lengths[encoded_count++].symbol = 0;
                i++;
                continue;
            }

            if (zero_count <= 10) {
                encoded_lengths[encoded_count].symbol = deflate_special_code_length_zeros;
                encoded_lengths[encoded_count++].count = zero_count;
            } else {
                encoded_lengths[encoded_count].symbol = deflate_special_code_length_long_zeros;
                encoded_lengths[encoded_count++].count = zero_count;
            }
            i += zero_count;
            continue;
        }

        encoded_lengths[encoded_count++].symbol = lengths[i++];

        auto copy_count = 0;
        for (size_t j = i; j < min(lengths.size(), i + 6) && lengths[j] == lengths[i - 1]; j++)
            copy_count++;

        if (copy_count >= 3) {
            encoded_lengths[encoded_count].symbol = deflate_special_code_length_copy;
            encoded_lengths[encoded_count++].count = copy_count;
            i += copy_count;
            continue;
        }
    }
    return encoded_count;
}

size_t DeflateCompressor::encode_block_lengths(Array<u8, max_huffman_literals> const& literal_bit_lengths, Array<u8, max_huffman_distances> const& distance_bit_lengths, Array<code_length_symbol, max_huffman_literals + max_huffman_distances>& encoded_lengths, size_t& literal_code_count, size_t& distance_code_count)
{
    literal_code_count = max_huffman_literals;
    distance_code_count = max_huffman_distances;

    VERIFY(literal_bit_lengths[EndOfBlock] != 0); // Make sure at least the EndOfBlock marker is present
    while (literal_bit_lengths[literal_code_count - 1] == 0)
        literal_code_count--;

    // Drop trailing zero lengths, keeping at least one
    while (distance_bit_lengths[distance_code_count - 1] == 0 && distance_code_count > 1)
        distance_code_count--;

    Array<u8, max_huffman_literals + max_huffman_distances> all_lengths {};
    for (size_t i = 0; i < literal_code_count; i++)
        all_lengths[i] = literal_bit_lengths[i];
    for (size_t i = 0; i < distance_code_count; i++)
        all_lengths[literal_code_count + i] = distance_bit_lengths[i];

    return encode_huffman_lengths(all_lengths.span().trim(literal_code_count + distance_code_count), encoded_lengths);
}

ErrorOr<void> DeflateCompressor::write_dynamic_huffman(CanonicalCode const& literal_code, size_t literal_code_count, Optional<CanonicalCode> const& distance_code, size_t distance_code_count, Array<u8, 19> const& code_lengths_bit_lengths, size_t code_length_count, Array<code_length_symbol, max_huffman_literals + max_huffman_distances> const& encoded_lengths, size_t encoded_lengths_count)
{
    TRY(m_output_stream->write_bits(literal_code_count - 257, 5));
    TRY(m_output_stream->write_bits(distance_code_count - 1, 5));
    TRY(m_output_stream->write_bits(code_length_count - 4, 4));

    for (size_t i = 0; i < code_length_count; i++) {
        TRY(m_output_stream->write_bits(code_lengths_bit_lengths[code_lengths_code_lengths_order[i]], 3));
    }

    auto code_lengths_code = MUST(CanonicalCode::from_bytes(code_lengths_bit_lengths));
    for (size_t i = 0; i < encoded_lengths_count; i++) {
        auto encoded_length = encoded_lengths[i];
        TRY(code_lengths_code.write_symbol(*m_output_stream, encoded_length.symbol));
        if (encoded_length.symbol == deflate_special_code_length_copy) {
            TRY(m_output_stream->write_bits<u8>(encoded_length.count - 3, 2));
        } else if (encoded_length.symbol == deflate_special_code_length_zeros) {
            TRY(m_output_stream->write_bits<u8>(encoded_length.count - 3, 3));
        } else if (encoded_length.symbol == deflate_special_code_length_long_zeros) {
            TRY(m_output_stream->write_bits<u8>(encoded_length.count - 11, 7));
        }
    }

    TRY(write_huffman(literal_code, distance_code));
    return {};
}

ErrorOr<void> DeflateCompressor::flush()
{
    TRY(m_output_stream->write_bits(m_finished, 1));

    // if this is just an empty block to signify the end of the deflate stream use the smallest block possible (10 bits total)
    if (m_pending_block_size == 0) {
        VERIFY(m_finished);                              // we shouldn't be writing empty blocks unless this is the final one
        TRY(m_output_stream->write_bits(0b01u, 2));      // fixed huffman codes
        TRY(m_output_stream->write_bits(0b0000000u, 7)); // end of block symbol
        TRY(m_output_stream->align_to_byte_boundary());
        return {};
    }

    auto write_uncompressed = [&]() -> ErrorOr<void> {
        TRY(m_output_stream->write_bits(0b00u, 2)); // no compression
        TRY(m_output_stream->align_to_byte_boundary());
        TRY(m_output_stream->write_value<LittleEndian<u16>>(m_pending_block_size));
        TRY(m_output_stream->write_value<LittleEndian<u16>>(~m_pending_block_size));
        TRY(m_output_stream->write_until_depleted(pending_block().slice(0, m_pending_block_size)));
        return {};
    };

    if (m_compression_level == CompressionLevel::STORE) { // disabled compression fast path
        TRY(write_uncompressed());
        m_pending_block_size = 0;
        return {};
    }

    // The following implementation of lz77 compression and huffman encoding is based on the reference implementation by Hans Wennborg https://www.hanshq.net/zip.html

    // this reads from the pending block and writes to m_symbol_buffer
    lz77_compress_block();

    // insert EndOfBlock marker to the symbol buffer
    m_symbol_buffer[m_pending_symbol_size].distance = 0;
    m_symbol_buffer[m_pending_symbol_size++].literal = EndOfBlock;
    m_symbol_frequencies[EndOfBlock]++;

    // generate optimal dynamic huffman code lengths
    Array<u8, max_huffman_literals> dynamic_literal_bit_lengths {};
    Array<u8, max_huffman_distances> dynamic_distance_bit_lengths {};
    generate_huffman_lengths(dynamic_literal_bit_lengths, m_symbol_frequencies, 15); // deflate data huffman can use up to 15 bits per symbol
    generate_huffman_lengths(dynamic_distance_bit_lengths, m_distance_frequencies, 15);

    // encode literal and distance lengths together in deflate format
    Array<code_length_symbol, max_huffman_literals + max_huffman_distances> encoded_lengths {};
    size_t literal_code_count;
    size_t distance_code_count;
    auto encoded_lengths_count = encode_block_lengths(dynamic_literal_bit_lengths, dynamic_distance_bit_lengths, encoded_lengths, literal_code_count, distance_code_count);

    // count code length frequencies
    Array<u16, 19> code_lengths_frequencies { 0 };
    for (size_t i = 0; i < encoded_lengths_count; i++) {
        code_lengths_frequencies[encoded_lengths[i].symbol]++;
    }
    // generate optimal huffman code lengths code lengths
    Array<u8, 19> code_lengths_bit_lengths {};
    generate_huffman_lengths(code_lengths_bit_lengths, code_lengths_frequencies, 7); // deflate code length huffman can use up to 7 bits per symbol
    // calculate actual code length code lengths count (without trailing zeros)
    auto code_lengths_count = code_lengths_bit_lengths.size();
    while (code_lengths_bit_lengths[code_lengths_code_lengths_order[code_lengths_count - 1]] == 0)
        code_lengths_count--;

    auto uncompressed_size = uncompressed_block_length();
    auto fixed_huffman_size = fixed_block_length();
    auto dynamic_huffman_size = dynamic_block_length(dynamic_literal_bit_lengths, dynamic_distance_bit_lengths, code_lengths_bit_lengths, code_lengths_frequencies, code_lengths_count);

    // If the compression somehow didn't reduce the size enough, just write out the block uncompressed as it allows for much faster decompression
    if (uncompressed_size <= min(fixed_huffman_size, dynamic_huffman_size)) {
        TRY(write_uncompressed());
    } else if (fixed_huffman_size <= dynamic_huffman_size) {
        // If the fixed and dynamic huffman codes come out the same size, prefer the fixed version, as it takes less time to decode fixed huffman codes.
        TRY(m_output_stream->write_bits(0b01u, 2));
        TRY(write_huffman(CanonicalCode::fixed_literal_codes(), CanonicalCode::fixed_distance_codes()));
    } else {
        // dynamic huffman codes
        TRY(m_output_stream->write_bits(0b10u, 2));
        auto literal_code = MUST(CanonicalCode::from_bytes(dynamic_literal_bit_lengths));
        auto distance_code_or_error = CanonicalCode::from_bytes(dynamic_distance_bit_lengths);
        Optional<CanonicalCode> distance_code;
        if (!distance_code_or_error.is_error())
            distance_code = distance_code_or_error.release_value();
        TRY(write_dynamic_huffman(literal_code, literal_code_count, distance_code, distance_code_count, code_lengths_bit_lengths, code_lengths_count, encoded_lengths, encoded_lengths_count));
    }
    if (m_finished)
        TRY(m_output_stream->align_to_byte_boundary());

    // reset all block specific members
    m_pending_block_size = 0;
    m_pending_symbol_size = 0;
    m_symbol_frequencies.fill(0);
    m_distance_frequencies.fill(0);
    // On the final block this copy will potentially produce an invalid search window, but since its the final block we dont care
    pending_block().copy_trimmed_to({ m_rolling_window, block_size });

    return {};
}

ErrorOr<void> DeflateCompressor::final_flush()
{
    VERIFY(!m_finished);
    m_finished = true;
    TRY(flush());
    TRY(m_output_stream->flush_buffer_to_stream());
    return {};
}

ErrorOr<ByteBuffer> DeflateCompressor::compress_all(ReadonlyBytes bytes, CompressionLevel compression_level)
{
    auto output_stream = TRY(try_make<AllocatingMemoryStream>());
    auto deflate_stream = TRY(DeflateCompressor::construct(MaybeOwned<Stream>(*output_stream), compression_level));

    TRY(deflate_stream->write_until_depleted(bytes));
    TRY(deflate_stream->final_flush());

    auto buffer = TRY(ByteBuffer::create_uninitialized(output_stream->used_buffer_size()));
    TRY(output_stream->read_until_filled(buffer));

    return buffer;
}

}
