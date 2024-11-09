/*
 * Copyright (c) 2023, Tim Schumacher <timschumi@gmx.de>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/IntegralMath.h>
#include <LibCompress/Lzma.h>

namespace Compress {

u32 LzmaHeader::dictionary_size() const
{
    // "If the value of dictionary size in properties is smaller than (1 << 12),
    //  the LZMA decoder must set the dictionary size variable to (1 << 12)."
    constexpr u32 minimum_dictionary_size = (1 << 12);
    if (unchecked_dictionary_size < minimum_dictionary_size)
        return minimum_dictionary_size;

    return unchecked_dictionary_size;
}

Optional<u64> LzmaHeader::uncompressed_size() const
{
    // We are making a copy of the packed field here because we would otherwise
    // pass an unaligned reference to the constructor of Optional, which is
    // undefined behavior.
    auto uncompressed_size = encoded_uncompressed_size;

    // "If "Uncompressed size" field contains ones in all 64 bits, it means that
    //  uncompressed size is unknown and there is the "end marker" in stream,
    //  that indicates the end of decoding point."
    if (uncompressed_size == placeholder_for_unknown_uncompressed_size)
        return {};

    // "In opposite case, if the value from "Uncompressed size" field is not
    //  equal to ((2^64) - 1), the LZMA stream decoding must be finished after
    //  specified number of bytes (Uncompressed size) is decoded. And if there
    //  is the "end marker", the LZMA decoder must read that marker also."
    return uncompressed_size;
}

ErrorOr<LzmaModelProperties> LzmaHeader::decode_model_properties(u8 input_bits)
{
    // "Decodes the following values from the encoded model properties field:
    //
    //     name  Range          Description
    //       lc  [0, 8]         the number of "literal context" bits
    //       lp  [0, 4]         the number of "literal pos" bits
    //       pb  [0, 4]         the number of "pos" bits
    //
    //  Encoded using `((pb * 5 + lp) * 9 + lc)`."

    if (input_bits >= (9 * 5 * 5))
        return Error::from_string_literal("Encoded model properties value is larger than the highest possible value");

    u8 literal_context_bits = input_bits % 9;
    input_bits /= 9;
    VERIFY(literal_context_bits >= 0 && literal_context_bits <= 8);

    u8 literal_position_bits = input_bits % 5;
    input_bits /= 5;
    VERIFY(literal_position_bits >= 0 && literal_position_bits <= 4);

    u8 position_bits = input_bits;
    VERIFY(position_bits >= 0 && position_bits <= 4);

    return LzmaModelProperties {
        .literal_context_bits = literal_context_bits,
        .literal_position_bits = literal_position_bits,
        .position_bits = position_bits,
    };
}

ErrorOr<u8> LzmaHeader::encode_model_properties(LzmaModelProperties const& model_properties)
{
    if (model_properties.literal_context_bits > 8)
        return Error::from_string_literal("LZMA literal context bits are too large to encode");

    if (model_properties.literal_position_bits > 4)
        return Error::from_string_literal("LZMA literal position bits are too large to encode");

    if (model_properties.position_bits > 4)
        return Error::from_string_literal("LZMA position bits are too large to encode");

    return (model_properties.position_bits * 5 + model_properties.literal_position_bits) * 9 + model_properties.literal_context_bits;
}

ErrorOr<LzmaDecompressorOptions> LzmaHeader::as_decompressor_options() const
{
    auto model_properties = TRY(decode_model_properties(encoded_model_properties));

    return Compress::LzmaDecompressorOptions {
        .literal_context_bits = model_properties.literal_context_bits,
        .literal_position_bits = model_properties.literal_position_bits,
        .position_bits = model_properties.position_bits,
        .dictionary_size = dictionary_size(),
        .uncompressed_size = uncompressed_size(),
        .reject_end_of_stream_marker = false,
    };
}

ErrorOr<LzmaHeader> LzmaHeader::from_compressor_options(LzmaCompressorOptions const& options)
{
    auto encoded_model_properties = TRY(encode_model_properties({
        .literal_context_bits = options.literal_context_bits,
        .literal_position_bits = options.literal_position_bits,
        .position_bits = options.position_bits,
    }));

    return LzmaHeader {
        .encoded_model_properties = encoded_model_properties,
        .unchecked_dictionary_size = options.dictionary_size,
        .encoded_uncompressed_size = options.uncompressed_size.value_or(placeholder_for_unknown_uncompressed_size),
    };
}

void LzmaState::initialize_to_default_probability(Span<Probability> span)
{
    for (auto& entry : span)
        entry = default_probability;
}

ErrorOr<NonnullOwnPtr<LzmaDecompressor>> LzmaDecompressor::create_from_container(MaybeOwned<Stream> stream, Optional<MaybeOwned<CircularBuffer>> dictionary)
{
    auto header = TRY(stream->read_value<LzmaHeader>());

    return TRY(LzmaDecompressor::create_from_raw_stream(move(stream), TRY(header.as_decompressor_options()), move(dictionary)));
}

ErrorOr<NonnullOwnPtr<LzmaDecompressor>> LzmaDecompressor::create_from_raw_stream(MaybeOwned<Stream> stream, LzmaDecompressorOptions const& options, Optional<MaybeOwned<CircularBuffer>> dictionary)
{
    if (!dictionary.has_value()) {
        auto new_dictionary = TRY(CircularBuffer::create_empty(options.dictionary_size));
        dictionary = TRY(try_make<CircularBuffer>(move(new_dictionary)));
    }

    VERIFY((*dictionary)->capacity() >= options.dictionary_size);

    // "The LZMA Decoder uses (1 << (lc + lp)) tables with CProb values, where each table contains 0x300 CProb values."
    auto literal_probabilities = TRY(FixedArray<Probability>::create(literal_probability_table_size * (1 << (options.literal_context_bits + options.literal_position_bits))));

    auto decompressor = TRY(adopt_nonnull_own_or_enomem(new (nothrow) LzmaDecompressor(move(stream), options, dictionary.release_value(), move(literal_probabilities))));

    TRY(decompressor->initialize_range_decoder());

    return decompressor;
}

LzmaState::LzmaState(FixedArray<Probability> literal_probabilities)
    : m_literal_probabilities(move(literal_probabilities))
{
    initialize_to_default_probability(m_literal_probabilities.span());

    for (auto& array : m_length_to_position_states)
        initialize_to_default_probability(array);

    for (auto& array : m_binary_tree_distance_probabilities)
        initialize_to_default_probability(array);

    initialize_to_default_probability(m_alignment_bit_probabilities);

    initialize_to_default_probability(m_is_match_probabilities);
    initialize_to_default_probability(m_is_rep_probabilities);
    initialize_to_default_probability(m_is_rep_g0_probabilities);
    initialize_to_default_probability(m_is_rep_g1_probabilities);
    initialize_to_default_probability(m_is_rep_g2_probabilities);
    initialize_to_default_probability(m_is_rep0_long_probabilities);
}

LzmaDecompressor::LzmaDecompressor(MaybeOwned<Stream> stream, LzmaDecompressorOptions options, MaybeOwned<CircularBuffer> dictionary, FixedArray<Probability> literal_probabilities)
    : LzmaState(move(literal_probabilities))
    , m_stream(move(stream))
    , m_options(move(options))
    , m_dictionary(move(dictionary))
{
}

bool LzmaDecompressor::is_range_decoder_in_clean_state() const
{
    return m_range_decoder_code == 0;
}

bool LzmaDecompressor::has_reached_expected_data_size() const
{
    if (!m_options.uncompressed_size.has_value())
        return false;

    return m_total_processed_bytes >= m_options.uncompressed_size.value();
}

ErrorOr<void> LzmaDecompressor::initialize_range_decoder()
{
    // "The LZMA Encoder always writes ZERO in initial byte of compressed stream.
    //  That scheme allows to simplify the code of the Range Encoder in the
    //  LZMA Encoder. If initial byte is not equal to ZERO, the LZMA Decoder must
    //  stop decoding and report error."
    {
        auto byte = TRY(m_stream->read_value<u8>());
        if (byte != 0)
            return Error::from_string_literal("Initial byte of data stream is not zero");
    }

    // Read the initial bytes into the range decoder.
    m_range_decoder_code = 0;
    for (size_t i = 0; i < 4; i++) {
        auto byte = TRY(m_stream->read_value<u8>());
        m_range_decoder_code = m_range_decoder_code << 8 | byte;
    }

    m_range_decoder_range = 0xFFFFFFFF;

    return {};
}

ErrorOr<void> LzmaDecompressor::append_input_stream(MaybeOwned<Stream> stream, Optional<u64> uncompressed_size)
{
    m_stream = move(stream);

    TRY(initialize_range_decoder());

    if (m_options.uncompressed_size.has_value() != uncompressed_size.has_value())
        return Error::from_string_literal("Appending LZMA streams with mismatching uncompressed size status");

    if (uncompressed_size.has_value())
        *m_options.uncompressed_size += *uncompressed_size;

    return {};
}

ErrorOr<void> LzmaDecompressor::normalize_range_decoder()
{
    // "The Normalize() function keeps the "Range" value in described range."

    if (m_range_decoder_range >= minimum_range_value)
        return {};

    m_range_decoder_range <<= 8;
    m_range_decoder_code <<= 8;

    m_range_decoder_code |= TRY(m_stream->read_value<u8>());

    VERIFY(m_range_decoder_range >= minimum_range_value);

    return {};
}

ErrorOr<void> LzmaCompressor::shift_range_encoder()
{
    if ((m_range_encoder_code >> 32) == 0x01) {
        // If there is an overflow, we can finalize the chain we were previously building.
        // This includes incrementing both the cached byte and all the 0xFF bytes that we generate.
        VERIFY(m_range_encoder_cached_byte != 0xFF);
        TRY(m_stream->write_value<u8>(m_range_encoder_cached_byte + 1));
        for (size_t i = 0; i < m_range_encoder_ff_chain_length; i++)
            TRY(m_stream->write_value<u8>(0x00));
        m_range_encoder_ff_chain_length = 0;
        m_range_encoder_cached_byte = (m_range_encoder_code >> 24);
    } else if ((m_range_encoder_code >> 24) == 0xFF) {
        // If the byte to flush is 0xFF, it can potentially propagate an overflow and needs to be added to the chain.
        m_range_encoder_ff_chain_length++;
    } else {
        // If the byte to flush isn't 0xFF, any future overflows will not be propagated beyond this point,
        // so we can be sure that the built chain doesn't change anymore.
        TRY(m_stream->write_value<u8>(m_range_encoder_cached_byte));
        for (size_t i = 0; i < m_range_encoder_ff_chain_length; i++)
            TRY(m_stream->write_value<u8>(0xFF));
        m_range_encoder_ff_chain_length = 0;
        m_range_encoder_cached_byte = (m_range_encoder_code >> 24);
    }

    // In all three cases we now recorded the highest byte in some way, so we can shift it away and shift in a null byte as the lowest byte.
    m_range_encoder_range <<= 8;
    m_range_encoder_code <<= 8;

    // Since we are working with a 64-bit code, we need to limit it to 32 bits artificially.
    m_range_encoder_code &= 0xFFFFFFFF;

    return {};
}

ErrorOr<void> LzmaCompressor::normalize_range_encoder()
{
    u64 const maximum_range_value = m_range_encoder_code + m_range_encoder_range;

    // Logically, we should only ever build up an overflow that is smaller than or equal to 0x01.
    VERIFY((maximum_range_value >> 32) <= 0x01);

    if (m_range_encoder_range >= minimum_range_value)
        return {};

    TRY(shift_range_encoder());

    VERIFY(m_range_encoder_range >= minimum_range_value);

    return {};
}

ErrorOr<u8> LzmaDecompressor::decode_direct_bit()
{
    dbgln_if(LZMA_DEBUG, "Decoding direct bit {} with code = {:#x}, range = {:#x}", 1 - ((m_range_decoder_code - (m_range_decoder_range >> 1)) >> 31), m_range_decoder_code, m_range_decoder_range);

    m_range_decoder_range >>= 1;
    m_range_decoder_code -= m_range_decoder_range;

    u32 temp = 0 - (m_range_decoder_code >> 31);

    m_range_decoder_code += m_range_decoder_range & temp;

    if (m_range_decoder_code == m_range_decoder_range)
        return Error::from_string_literal("Reached an invalid state while decoding LZMA stream");

    TRY(normalize_range_decoder());

    return temp + 1;
}

ErrorOr<void> LzmaCompressor::encode_direct_bit(u8 value)
{
    dbgln_if(LZMA_DEBUG, "Encoding direct bit {} with code = {:#x}, range = {:#x}", value, m_range_encoder_code, m_range_encoder_range);

    m_range_encoder_range >>= 1;

    if (value != 0)
        m_range_encoder_code += m_range_encoder_range;

    TRY(normalize_range_encoder());

    return {};
}

ErrorOr<u8> LzmaDecompressor::decode_bit_with_probability(Probability& probability)
{
    // "The LZMA decoder provides the pointer to CProb variable that contains
    //  information about estimated probability for symbol 0 and the Range Decoder
    //  updates that CProb variable after decoding."

    u32 bound = (m_range_decoder_range >> probability_bit_count) * probability;

    dbgln_if(LZMA_DEBUG, "Decoding bit {} with probability = {:#x}, bound = {:#x}, code = {:#x}, range = {:#x}", m_range_decoder_code < bound ? 0 : 1, probability, bound, m_range_decoder_code, m_range_decoder_range);

    if (m_range_decoder_code < bound) {
        probability += ((1 << probability_bit_count) - probability) >> probability_shift_width;
        m_range_decoder_range = bound;
        TRY(normalize_range_decoder());
        return 0;
    } else {
        probability -= probability >> probability_shift_width;
        m_range_decoder_code -= bound;
        m_range_decoder_range -= bound;
        TRY(normalize_range_decoder());
        return 1;
    }
}

ErrorOr<void> LzmaCompressor::encode_bit_with_probability(Probability& probability, u8 value)
{
    u32 bound = (m_range_encoder_range >> probability_bit_count) * probability;

    dbgln_if(LZMA_DEBUG, "Encoding bit {} with probability = {:#x}, bound = {:#x}, code = {:#x}, range = {:#x}", value, probability, bound, m_range_encoder_code, m_range_encoder_range);

    if (value == 0) {
        probability += ((1 << probability_bit_count) - probability) >> probability_shift_width;
        m_range_encoder_range = bound;
    } else {
        probability -= probability >> probability_shift_width;
        m_range_encoder_code += bound;
        m_range_encoder_range -= bound;
    }

    TRY(normalize_range_encoder());
    return {};
}

ErrorOr<u16> LzmaDecompressor::decode_symbol_using_bit_tree(size_t bit_count, Span<Probability> probability_tree)
{
    VERIFY(bit_count <= sizeof(u16) * 8);
    VERIFY(probability_tree.size() >= 1ul << bit_count);

    // This has been modified from the reference implementation to unlink the result and the tree index,
    // which should allow for better readability.

    u16 result = 0;
    size_t tree_index = 1;

    for (size_t i = 0; i < bit_count; i++) {
        u16 next_bit = TRY(decode_bit_with_probability(probability_tree[tree_index]));
        result = (result << 1) | next_bit;
        tree_index = (tree_index << 1) | next_bit;
    }

    dbgln_if(LZMA_DEBUG, "Decoded value {:#x} with {} bits using bit tree", result, bit_count);

    return result;
}

ErrorOr<void> LzmaCompressor::encode_symbol_using_bit_tree(size_t bit_count, Span<Probability> probability_tree, u16 value)
{
    VERIFY(bit_count <= sizeof(u16) * 8);
    VERIFY(probability_tree.size() >= 1ul << bit_count);
    VERIFY(value <= (1 << bit_count) - 1);

    auto original_value = value;

    // Shift value to make the first sent byte the most significant bit. This makes the shifting logic a lot easier to read.
    value <<= sizeof(u16) * 8 - bit_count;

    size_t tree_index = 1;

    for (size_t i = 0; i < bit_count; i++) {
        u8 const next_bit = (value & 0x8000) >> (sizeof(u16) * 8 - 1);
        value <<= 1;
        TRY(encode_bit_with_probability(probability_tree[tree_index], next_bit));
        tree_index = (tree_index << 1) | next_bit;
    }

    dbgln_if(LZMA_DEBUG, "Encoded value {:#x} with {} bits using bit tree", original_value, bit_count);

    return {};
}

ErrorOr<u16> LzmaDecompressor::decode_symbol_using_reverse_bit_tree(size_t bit_count, Span<Probability> probability_tree)
{
    VERIFY(bit_count <= sizeof(u16) * 8);
    VERIFY(probability_tree.size() >= 1ul << bit_count);

    u16 result = 0;
    size_t tree_index = 1;

    for (size_t i = 0; i < bit_count; i++) {
        u16 next_bit = TRY(decode_bit_with_probability(probability_tree[tree_index]));
        result |= next_bit << i;
        tree_index = (tree_index << 1) | next_bit;
    }

    dbgln_if(LZMA_DEBUG, "Decoded value {:#x} with {} bits using reverse bit tree", result, bit_count);

    return result;
}

ErrorOr<void> LzmaCompressor::encode_symbol_using_reverse_bit_tree(size_t bit_count, Span<Probability> probability_tree, u16 value)
{
    VERIFY(bit_count <= sizeof(u16) * 8);
    VERIFY(probability_tree.size() >= 1ul << bit_count);
    VERIFY(value <= (1 << bit_count) - 1);

    auto original_value = value;

    size_t tree_index = 1;

    for (size_t i = 0; i < bit_count; i++) {
        u8 const next_bit = value & 1;
        value >>= 1;
        TRY(encode_bit_with_probability(probability_tree[tree_index], next_bit));
        tree_index = (tree_index << 1) | next_bit;
    }

    dbgln_if(LZMA_DEBUG, "Encoded value {:#x} with {} bits using reverse bit tree", original_value, bit_count);

    return {};
}

ErrorOr<void> LzmaDecompressor::decode_literal_to_output_buffer()
{
    u8 previous_byte = 0;
    if (m_dictionary->seekback_limit() > 0) {
        auto read_bytes = MUST(m_dictionary->read_with_seekback({ &previous_byte, sizeof(previous_byte) }, 1));
        VERIFY(read_bytes.size() == sizeof(previous_byte));
    }

    // "To select the table for decoding it uses the context that consists of
    //  (lc) high bits from previous literal and (lp) low bits from value that
    //  represents current position in outputStream."
    u16 literal_state_bits_from_position = m_total_processed_bytes & ((1 << m_options.literal_position_bits) - 1);
    u16 literal_state_bits_from_output = previous_byte >> (8 - m_options.literal_context_bits);
    u16 literal_state = literal_state_bits_from_position << m_options.literal_context_bits | literal_state_bits_from_output;

    Span<Probability> selected_probability_table = m_literal_probabilities.span().slice(literal_probability_table_size * literal_state, literal_probability_table_size);

    // The result is defined as u16 here and initialized to 1, but we will cut off the top bits before queueing them into the output buffer.
    // The top bit is only used to track how much we have decoded already, and to select the correct probability table.
    u16 result = 1;

    // "If (State > 7), the Literal Decoder also uses "matchByte" that represents
    //  the byte in OutputStream at position the is the DISTANCE bytes before
    //  current position, where the DISTANCE is the distance in DISTANCE-LENGTH pair
    //  of latest decoded match."
    // Note: The specification says `(State > 7)`, but the reference implementation does `(State >= 7)`, which is a mismatch.
    //       Testing `(State > 7)` with actual test files yields errors, so the reference implementation appears to be the correct one.
    if (m_state >= 7) {
        u8 matched_byte = 0;
        auto read_bytes = TRY(m_dictionary->read_with_seekback({ &matched_byte, sizeof(matched_byte) }, current_repetition_offset()));
        VERIFY(read_bytes.size() == sizeof(matched_byte));

        dbgln_if(LZMA_DEBUG, "Decoding literal using match byte {:#x}", matched_byte);

        do {
            u8 match_bit = (matched_byte >> 7) & 1;
            matched_byte <<= 1;

            u8 decoded_bit = TRY(decode_bit_with_probability(selected_probability_table[((1 + match_bit) << 8) + result]));
            result = result << 1 | decoded_bit;

            if (match_bit != decoded_bit)
                break;
        } while (result < 0x100);
    }

    while (result < 0x100)
        result = (result << 1) | TRY(decode_bit_with_probability(selected_probability_table[result]));

    u8 actual_result = result - 0x100;

    size_t written_bytes = m_dictionary->write({ &actual_result, sizeof(actual_result) });
    VERIFY(written_bytes == sizeof(actual_result));
    m_total_processed_bytes += sizeof(actual_result);

    dbgln_if(LZMA_DEBUG, "Decoded literal {:#x} in state {} using literal state {:#x} (previous byte is {:#x})", actual_result, m_state, literal_state, previous_byte);

    return {};
}

ErrorOr<void> LzmaCompressor::encode_literal(u8 literal)
{
    // This function largely mirrors `decode_literal_to_output_buffer`, so specification comments have been omitted.

    TRY(encode_match_type(MatchType::Literal));

    // Note: We have already read the next byte from the input buffer, so it's now in the seekback buffer, shifting all seekback offsets by one.
    u8 previous_byte = 0;
    if (m_dictionary->seekback_limit() - m_dictionary->used_space() > 1) {
        auto read_bytes = MUST(m_dictionary->read_with_seekback({ &previous_byte, sizeof(previous_byte) }, 2 + m_dictionary->used_space()));
        VERIFY(read_bytes.size() == sizeof(previous_byte));
    }
    u16 const literal_state_bits_from_position = m_total_processed_bytes & ((1 << m_options.literal_position_bits) - 1);
    u16 const literal_state_bits_from_output = previous_byte >> (8 - m_options.literal_context_bits);
    u16 const literal_state = literal_state_bits_from_position << m_options.literal_context_bits | literal_state_bits_from_output;

    Span<Probability> selected_probability_table = m_literal_probabilities.span().slice(literal_probability_table_size * literal_state, literal_probability_table_size);

    auto original_literal = literal;
    u16 result = 1;

    if (m_state >= 7) {
        u8 matched_byte = 0;
        auto read_bytes = TRY(m_dictionary->read_with_seekback({ &matched_byte, sizeof(matched_byte) }, current_repetition_offset() + m_dictionary->used_space() + 1));
        VERIFY(read_bytes.size() == sizeof(matched_byte));

        dbgln_if(LZMA_DEBUG, "Encoding literal using match byte {:#x}", matched_byte);

        do {
            u8 const match_bit = (matched_byte >> 7) & 1;
            matched_byte <<= 1;

            u8 const encoded_bit = (literal & 0x80) >> 7;
            literal <<= 1;

            TRY(encode_bit_with_probability(selected_probability_table[((1 + match_bit) << 8) + result], encoded_bit));
            result = result << 1 | encoded_bit;

            if (match_bit != encoded_bit)
                break;
        } while (result < 0x100);
    }

    while (result < 0x100) {
        u8 const encoded_bit = (literal & 0x80) >> 7;
        literal <<= 1;

        TRY(encode_bit_with_probability(selected_probability_table[result], encoded_bit));

        result = (result << 1) | encoded_bit;
    }

    m_total_processed_bytes += sizeof(literal);

    dbgln_if(LZMA_DEBUG, "Encoded literal {:#x} in state {} using literal state {:#x} (previous byte is {:#x})", original_literal, m_state, literal_state, previous_byte);

    update_state_after_literal();

    return {};
}

ErrorOr<void> LzmaCompressor::encode_existing_match(size_t real_distance, size_t real_length)
{
    VERIFY(real_distance >= normalized_to_real_match_distance_offset);
    u32 const normalized_distance = real_distance - normalized_to_real_match_distance_offset;

    VERIFY(real_length >= normalized_to_real_match_length_offset);
    u16 const normalized_length = real_length - normalized_to_real_match_length_offset;

    if (normalized_distance == m_rep0) {
        TRY(encode_match_type(MatchType::RepMatch0));
    } else if (normalized_distance == m_rep1) {
        TRY(encode_match_type(MatchType::RepMatch1));

        u32 const distance = m_rep1;
        m_rep1 = m_rep0;
        m_rep0 = distance;
    } else if (normalized_distance == m_rep2) {
        TRY(encode_match_type(MatchType::RepMatch2));

        u32 const distance = m_rep2;
        m_rep2 = m_rep1;
        m_rep1 = m_rep0;
        m_rep0 = distance;
    } else if (normalized_distance == m_rep3) {
        TRY(encode_match_type(MatchType::RepMatch3));

        u32 const distance = m_rep3;
        m_rep3 = m_rep2;
        m_rep2 = m_rep1;
        m_rep1 = m_rep0;
        m_rep0 = distance;
    } else {
        VERIFY_NOT_REACHED();
    }

    TRY(encode_normalized_match_length(m_rep_length_coder, normalized_length));
    update_state_after_rep();
    MUST(m_dictionary->discard(real_length));
    m_total_processed_bytes += real_length;

    return {};
}

ErrorOr<void> LzmaCompressor::encode_new_match(size_t real_distance, size_t real_length)
{
    VERIFY(real_distance >= normalized_to_real_match_distance_offset);
    u32 const normalized_distance = real_distance - normalized_to_real_match_distance_offset;

    VERIFY(real_length >= normalized_to_real_match_length_offset);
    u16 const normalized_length = real_length - normalized_to_real_match_length_offset;

    TRY(encode_normalized_simple_match(normalized_distance, normalized_length));

    MUST(m_dictionary->discard(real_length));
    m_total_processed_bytes += real_length;

    return {};
}

ErrorOr<void> LzmaCompressor::encode_normalized_simple_match(u32 normalized_distance, u16 normalized_length)
{
    TRY(encode_match_type(MatchType::SimpleMatch));

    m_rep3 = m_rep2;
    m_rep2 = m_rep1;
    m_rep1 = m_rep0;

    TRY(encode_normalized_match_length(m_length_coder, normalized_length));

    update_state_after_match();

    TRY(encode_normalized_match_distance(normalized_length, normalized_distance));
    m_rep0 = normalized_distance;

    return {};
}

LzmaState::LzmaLengthCoderState::LzmaLengthCoderState()
{
    for (auto& array : m_low_length_probabilities)
        initialize_to_default_probability(array);

    for (auto& array : m_medium_length_probabilities)
        initialize_to_default_probability(array);

    initialize_to_default_probability(m_high_length_probabilities);
}

ErrorOr<u16> LzmaDecompressor::decode_normalized_match_length(LzmaLengthCoderState& length_decoder_state)
{
    // "LZMA uses "posState" value as context to select the binary tree
    //  from LowCoder and MidCoder binary tree arrays:"
    u16 position_state = m_total_processed_bytes & ((1 << m_options.position_bits) - 1);

    // "The following scheme is used for the match length encoding:
    //
    //   Binary encoding    Binary Tree structure    Zero-based match length
    //   sequence                                    (binary + decimal):
    //
    //   0 xxx              LowCoder[posState]       xxx
    if (TRY(decode_bit_with_probability(length_decoder_state.m_first_choice_probability)) == 0)
        return TRY(decode_symbol_using_bit_tree(3, length_decoder_state.m_low_length_probabilities[position_state].span()));

    //   1 0 yyy            MidCoder[posState]       yyy + 8
    if (TRY(decode_bit_with_probability(length_decoder_state.m_second_choice_probability)) == 0)
        return TRY(decode_symbol_using_bit_tree(3, length_decoder_state.m_medium_length_probabilities[position_state].span())) + 8;

    //   1 1 zzzzzzzz       HighCoder                zzzzzzzz + 16"
    return TRY(decode_symbol_using_bit_tree(8, length_decoder_state.m_high_length_probabilities.span())) + 16;
}

ErrorOr<void> LzmaCompressor::encode_normalized_match_length(LzmaLengthCoderState& length_coder_state, u16 normalized_length)
{
    u16 const position_state = m_total_processed_bytes & ((1 << m_options.position_bits) - 1);

    if (normalized_length < 8) {
        TRY(encode_bit_with_probability(length_coder_state.m_first_choice_probability, 0));
        TRY(encode_symbol_using_bit_tree(3, length_coder_state.m_low_length_probabilities[position_state].span(), normalized_length));
        return {};
    }

    TRY(encode_bit_with_probability(length_coder_state.m_first_choice_probability, 1));

    if (normalized_length < 16) {
        TRY(encode_bit_with_probability(length_coder_state.m_second_choice_probability, 0));
        TRY(encode_symbol_using_bit_tree(3, length_coder_state.m_medium_length_probabilities[position_state].span(), normalized_length - 8));
        return {};
    }

    TRY(encode_bit_with_probability(length_coder_state.m_second_choice_probability, 1));
    TRY(encode_symbol_using_bit_tree(8, length_coder_state.m_high_length_probabilities.span(), normalized_length - 16));
    return {};
}

ErrorOr<u32> LzmaDecompressor::decode_normalized_match_distance(u16 normalized_match_length)
{
    // "LZMA uses normalized match length (zero-based length)
    //  to calculate the context state "lenState" do decode the distance value."
    u16 length_state = min(normalized_match_length, number_of_length_to_position_states - 1);

    // "At first stage the distance decoder decodes 6-bit "posSlot" value with bit
    //  tree decoder from PosSlotDecoder array."
    u16 position_slot = TRY(decode_symbol_using_bit_tree(6, m_length_to_position_states[length_state].span()));

    // "The encoding scheme for distance value is shown in the following table:
    //
    //  posSlot (decimal) /
    //       zero-based distance (binary)
    //  0    0
    //  1    1
    //  2    10
    //  3    11
    //
    //  4    10 x
    //  5    11 x
    //  6    10 xx
    //  7    11 xx
    //  8    10 xxx
    //  9    11 xxx
    //  10    10 xxxx
    //  11    11 xxxx
    //  12    10 xxxxx
    //  13    11 xxxxx
    //
    //  14    10 yy zzzz
    //  15    11 yy zzzz
    //  16    10 yyy zzzz
    //  17    11 yyy zzzz
    //  ...
    //  62    10 yyyyyyyyyyyyyyyyyyyyyyyyyy zzzz
    //  63    11 yyyyyyyyyyyyyyyyyyyyyyyyyy zzzz
    //
    //  where
    //   "x ... x" means the sequence of binary symbols encoded with binary tree and
    //       "Reverse" scheme. It uses separated binary tree for each posSlot from 4 to 13.
    //   "y" means direct bit encoded with range coder.
    //   "zzzz" means the sequence of four binary symbols encoded with binary
    //       tree with "Reverse" scheme, where one common binary tree "AlignDecoder"
    //       is used for all posSlot values."

    // "If (posSlot < 4), the "dist" value is equal to posSlot value."
    if (position_slot < first_position_slot_with_binary_tree_bits)
        return position_slot;

    // From here on, the first bit of the distance is always set and the second bit is set if the last bit of the position slot is set.
    u32 distance_prefix = ((1 << 1) | ((position_slot & 1) << 0));

    // "If (posSlot >= 4), the decoder uses "posSlot" value to calculate the value of
    //   the high bits of "dist" value and the number of the low bits.
    //   If (4 <= posSlot < kEndPosModelIndex), the decoder uses bit tree decoders.
    //     (one separated bit tree decoder per one posSlot value) and "Reverse" scheme."
    if (position_slot < first_position_slot_with_direct_encoded_bits) {
        size_t number_of_bits_to_decode = (position_slot / 2) - 1;
        auto& selected_probability_tree = m_binary_tree_distance_probabilities[position_slot - first_position_slot_with_binary_tree_bits];
        return (distance_prefix << number_of_bits_to_decode) | TRY(decode_symbol_using_reverse_bit_tree(number_of_bits_to_decode, selected_probability_tree));
    }

    // "  if (posSlot >= kEndPosModelIndex), the middle bits are decoded as direct
    //     bits from RangeDecoder and the low 4 bits are decoded with a bit tree
    //     decoder "AlignDecoder" with "Reverse" scheme."
    size_t number_of_direct_bits_to_decode = ((position_slot - first_position_slot_with_direct_encoded_bits) / 2) + 2;
    for (size_t i = 0; i < number_of_direct_bits_to_decode; i++) {
        distance_prefix = (distance_prefix << 1) | TRY(decode_direct_bit());
    }
    return (distance_prefix << number_of_alignment_bits) | TRY(decode_symbol_using_reverse_bit_tree(number_of_alignment_bits, m_alignment_bit_probabilities));
}

ErrorOr<void> LzmaCompressor::encode_normalized_match_distance(u16 normalized_match_length, u32 normalized_match_distance)
{
    u16 const length_state = min(normalized_match_length, number_of_length_to_position_states - 1);

    if (normalized_match_distance < first_position_slot_with_binary_tree_bits) {
        // The normalized distance gets encoded as the position slot.
        TRY(encode_symbol_using_bit_tree(6, m_length_to_position_states[length_state].span(), normalized_match_distance));
        return {};
    }

    // Note: This has been deduced, there is no immediate relation to the decoding function.
    u16 const distance_log2 = AK::log2(normalized_match_distance);
    u16 number_of_distance_bits = count_required_bits(normalized_match_distance);
    u16 const position_slot = (distance_log2 << 1) + ((normalized_match_distance >> (distance_log2 - 1)) & 1);

    TRY(encode_symbol_using_bit_tree(6, m_length_to_position_states[length_state].span(), position_slot));

    // Mask off the top two bits of the value, those are already encoded by the position slot.
    normalized_match_distance &= (1 << (number_of_distance_bits - 2)) - 1;
    number_of_distance_bits -= 2;

    if (position_slot < first_position_slot_with_direct_encoded_bits) {
        // The value gets encoded using only a reverse bit tree coder.
        auto& selected_probability_tree = m_binary_tree_distance_probabilities[position_slot - first_position_slot_with_binary_tree_bits];
        TRY(encode_symbol_using_reverse_bit_tree(number_of_distance_bits, selected_probability_tree, normalized_match_distance));
        return {};
    }

    // The value is split into direct bits (everything except the last four bits) and alignment bits (last four bits).
    auto direct_bits = normalized_match_distance & ~((1 << number_of_alignment_bits) - 1);
    auto const alignment_bits = normalized_match_distance & ((1 << number_of_alignment_bits) - 1);

    // Shift to-be-written direct bits to the most significant position for easier access.
    direct_bits <<= sizeof(direct_bits) * 8 - number_of_distance_bits;

    for (auto i = 0u; i < number_of_distance_bits - number_of_alignment_bits; i++) {
        TRY(encode_direct_bit((direct_bits & 0x80000000) ? 1 : 0));
        direct_bits <<= 1;
    }

    TRY(encode_symbol_using_reverse_bit_tree(number_of_alignment_bits, m_alignment_bit_probabilities, alignment_bits));

    return {};
}

u32 LzmaState::current_repetition_offset() const
{
    // LZMA never needs to read at offset 0 (i.e. the actual read head of the buffer).
    // Instead, the values are remapped so that the rep-value n starts reading n + 1 bytes back.
    // The special rep-value 0xFFFFFFFF is reserved for marking the end of the stream,
    // so this should never overflow.
    VERIFY(m_rep0 <= NumericLimits<u32>::max() - normalized_to_real_match_distance_offset);
    return m_rep0 + normalized_to_real_match_distance_offset;
}

void LzmaState::update_state_after_literal()
{
    if (m_state < 4)
        m_state = 0;
    else if (m_state < 10)
        m_state -= 3;
    else
        m_state -= 6;
}

void LzmaState::update_state_after_match()
{
    if (m_state < 7)
        m_state = 7;
    else
        m_state = 10;
}

void LzmaState::update_state_after_rep()
{
    if (m_state < 7)
        m_state = 8;
    else
        m_state = 11;
}

void LzmaState::update_state_after_short_rep()
{
    if (m_state < 7)
        m_state = 9;
    else
        m_state = 11;
}

ErrorOr<LzmaDecompressor::MatchType> LzmaDecompressor::decode_match_type()
{
    // "The decoder calculates "state2" variable value to select exact variable from
    //  "IsMatch" and "IsRep0Long" arrays."
    u16 position_state = m_total_processed_bytes & ((1 << m_options.position_bits) - 1);
    u16 state2 = (m_state << maximum_number_of_position_bits) + position_state;

    // "The decoder uses the following code flow scheme to select exact
    //  type of LITERAL or MATCH:
    //
    //  IsMatch[state2] decode
    //   0 - the Literal"
    if (TRY(decode_bit_with_probability(m_is_match_probabilities[state2])) == 0) {
        dbgln_if(LZMA_DEBUG, "Decoded match type 'Literal'");
        return MatchType::Literal;
    }

    // " 1 - the Match
    //     IsRep[state] decode
    //       0 - Simple Match"
    if (TRY(decode_bit_with_probability(m_is_rep_probabilities[m_state])) == 0) {
        dbgln_if(LZMA_DEBUG, "Decoded match type 'SimpleMatch'");
        return MatchType::SimpleMatch;
    }

    // "     1 - Rep Match
    //         IsRepG0[state] decode
    //           0 - the distance is rep0"
    if (TRY(decode_bit_with_probability(m_is_rep_g0_probabilities[m_state])) == 0) {
        // "       IsRep0Long[state2] decode
        //           0 - Short Rep Match"
        if (TRY(decode_bit_with_probability(m_is_rep0_long_probabilities[state2])) == 0) {
            dbgln_if(LZMA_DEBUG, "Decoded match type 'ShortRepMatch'");
            return MatchType::ShortRepMatch;
        }

        // "         1 - Rep Match 0"
        dbgln_if(LZMA_DEBUG, "Decoded match type 'RepMatch0'");
        return MatchType::RepMatch0;
    }

    // "         1 -
    //             IsRepG1[state] decode
    //               0 - Rep Match 1"
    if (TRY(decode_bit_with_probability(m_is_rep_g1_probabilities[m_state])) == 0) {
        dbgln_if(LZMA_DEBUG, "Decoded match type 'RepMatch1'");
        return MatchType::RepMatch1;
    }

    // "             1 -
    //                 IsRepG2[state] decode
    //                   0 - Rep Match 2"
    if (TRY(decode_bit_with_probability(m_is_rep_g2_probabilities[m_state])) == 0) {
        dbgln_if(LZMA_DEBUG, "Decoded match type 'RepMatch2'");
        return MatchType::RepMatch2;
    }

    // "                 1 - Rep Match 3"
    dbgln_if(LZMA_DEBUG, "Decoded match type 'RepMatch3'");
    return MatchType::RepMatch3;
}

ErrorOr<void> LzmaCompressor::encode_match_type(MatchType match_type)
{
    u16 position_state = m_total_processed_bytes & ((1 << m_options.position_bits) - 1);
    u16 state2 = (m_state << maximum_number_of_position_bits) + position_state;

    if (match_type == MatchType::Literal) {
        TRY(encode_bit_with_probability(m_is_match_probabilities[state2], 0));
        dbgln_if(LZMA_DEBUG, "Encoded match type 'Literal'");
        return {};
    }
    TRY(encode_bit_with_probability(m_is_match_probabilities[state2], 1));

    if (match_type == MatchType::SimpleMatch) {
        TRY(encode_bit_with_probability(m_is_rep_probabilities[m_state], 0));
        dbgln_if(LZMA_DEBUG, "Encoded match type 'SimpleMatch'");
        return {};
    }
    TRY(encode_bit_with_probability(m_is_rep_probabilities[m_state], 1));

    if (match_type == MatchType::ShortRepMatch || match_type == MatchType::RepMatch0) {
        TRY(encode_bit_with_probability(m_is_rep_g0_probabilities[m_state], 0));
        TRY(encode_bit_with_probability(m_is_rep0_long_probabilities[state2], match_type == MatchType::RepMatch0));
        if constexpr (LZMA_DEBUG) {
            if (match_type == RepMatch0)
                dbgln("Encoded match type 'RepMatch0'");
            else
                dbgln("Encoded match type 'ShortRepMatch'");
        }
        return {};
    }
    TRY(encode_bit_with_probability(m_is_rep_g0_probabilities[m_state], 1));

    if (match_type == MatchType::RepMatch1) {
        TRY(encode_bit_with_probability(m_is_rep_g1_probabilities[m_state], 0));
        dbgln_if(LZMA_DEBUG, "Encoded match type 'RepMatch1'");
        return {};
    }
    TRY(encode_bit_with_probability(m_is_rep_g1_probabilities[m_state], 1));

    if (match_type == MatchType::RepMatch2) {
        TRY(encode_bit_with_probability(m_is_rep_g2_probabilities[m_state], 0));
        dbgln_if(LZMA_DEBUG, "Encoded match type 'RepMatch2'");
        return {};
    }
    TRY(encode_bit_with_probability(m_is_rep_g2_probabilities[m_state], 1));
    dbgln_if(LZMA_DEBUG, "Encoded match type 'RepMatch3'");
    return {};
}

ErrorOr<void> LzmaCompressor::encode_once()
{
    // Check if any of our existing match distances are currently usable.
    Vector<size_t> const existing_distances {
        m_rep0 + normalized_to_real_match_distance_offset,
        m_rep1 + normalized_to_real_match_distance_offset,
        m_rep2 + normalized_to_real_match_distance_offset,
        m_rep3 + normalized_to_real_match_distance_offset,
    };
    auto existing_distance_result = m_dictionary->find_copy_in_seekback(existing_distances, m_dictionary->used_space(), normalized_to_real_match_length_offset);

    if (existing_distance_result.has_value()) {
        auto selected_match = existing_distance_result.release_value();
        TRY(encode_existing_match(selected_match.distance, selected_match.length));
        return {};
    }

    // If we weren't able to find any viable existing offsets, we now have to search the rest of the dictionary for possible new offsets.
    auto new_distance_result = m_dictionary->find_copy_in_seekback(m_dictionary->used_space(), normalized_to_real_match_length_offset);

    if (new_distance_result.has_value()) {
        auto selected_match = new_distance_result.release_value();
        TRY(encode_new_match(selected_match.distance, selected_match.length));
        return {};
    }

    // If we weren't able to find any matches, we don't have any other choice than to encode the next byte as a literal.
    u8 next_byte { 0 };
    TRY(m_dictionary->read({ &next_byte, sizeof(next_byte) }));
    TRY(encode_literal(next_byte));
    return {};
}

ErrorOr<Bytes> LzmaDecompressor::read_some(Bytes bytes)
{
    while (m_dictionary->used_space() < bytes.size() && m_dictionary->empty_space() != 0) {
        if (m_found_end_of_stream_marker)
            break;

        if (has_reached_expected_data_size()) {
            // If the decoder is in a clean state, we assume that this is fine.
            if (is_range_decoder_in_clean_state())
                break;

            // Otherwise, we give it one last try to find the end marker in the remaining data.
        }

        auto copy_match_to_buffer = [&](u16 real_length) -> ErrorOr<void> {
            VERIFY(!m_leftover_match_length.has_value());

            if (m_options.uncompressed_size.has_value() && m_options.uncompressed_size.value() < m_total_processed_bytes + real_length)
                return Error::from_string_literal("Tried to copy match beyond expected uncompressed file size");

            auto copied_length = TRY(m_dictionary->copy_from_seekback(current_repetition_offset(), real_length));

            m_total_processed_bytes += copied_length;
            real_length -= copied_length;

            if (real_length > 0)
                m_leftover_match_length = real_length;

            return {};
        };

        // If we have a leftover part of a repeating match, we should finish that first.
        if (m_leftover_match_length.has_value()) {
            TRY(copy_match_to_buffer(m_leftover_match_length.release_value()));
            continue;
        }

        auto const match_type = TRY(decode_match_type());

        // If we are looking for EOS, but find another match type, the stream is also corrupted.
        if (has_reached_expected_data_size() && match_type != MatchType::SimpleMatch)
            return Error::from_string_literal("First match type after the expected uncompressed size is not a simple match");

        if (match_type == MatchType::Literal) {
            // "At first the LZMA decoder must check that it doesn't exceed
            //  specified uncompressed size."
            // This is already checked for at the beginning of the loop.

            // "Then it decodes literal value and puts it to sliding window."
            TRY(decode_literal_to_output_buffer());

            // "Then the decoder must update the "state" value."
            update_state_after_literal();
            continue;
        }

        if (match_type == MatchType::SimpleMatch) {
            // "The distance history table is updated with the following scheme:"
            m_rep3 = m_rep2;
            m_rep2 = m_rep1;
            m_rep1 = m_rep0;

            // "The zero-based length is decoded with "LenDecoder"."
            u16 normalized_length = TRY(decode_normalized_match_length(m_length_coder));

            // "The state is update with UpdateState_Match function."
            update_state_after_match();

            // "and the new "rep0" value is decoded with DecodeDistance."
            m_rep0 = TRY(decode_normalized_match_distance(normalized_length));

            // "If the value of "rep0" is equal to 0xFFFFFFFF, it means that we have
            //  "End of stream" marker, so we can stop decoding and check finishing
            //  condition in Range Decoder"
            if (m_rep0 == end_of_stream_marker) {
                // If we should reject end-of-stream markers, do so now.
                // Note that this is not part of LZMA, as LZMA allows end-of-stream markers in all contexts, so pure LZMA should never set this option.
                if (m_options.reject_end_of_stream_marker)
                    return Error::from_string_literal("An end-of-stream marker was found, but the LZMA stream is configured to reject them");

                // The range decoder condition is checked after breaking out of the loop.
                m_found_end_of_stream_marker = true;
                continue;
            }

            // If we are looking for EOS, but haven't found it here, the stream is corrupted.
            if (has_reached_expected_data_size())
                return Error::from_string_literal("First simple match after the expected uncompressed size is not the EOS marker");

            // "If uncompressed size is defined, LZMA decoder must check that it doesn't
            //  exceed that specified uncompressed size."
            // This is being checked for in the common "copy to buffer" implementation.

            // "Also the decoder must check that "rep0" value is not larger than dictionary size
            //  and is not larger than the number of already decoded bytes."
            if (current_repetition_offset() > m_dictionary->seekback_limit())
                return Error::from_string_literal("rep0 value is larger than the possible lookback size");

            // "Then the decoder must copy match bytes as described in
            //  "The match symbols copying" section."
            TRY(copy_match_to_buffer(normalized_length + normalized_to_real_match_length_offset));

            continue;
        }

        if (match_type == MatchType::ShortRepMatch) {
            // "LZMA doesn't update the distance history."

            // "If the subtype is "Short Rep Match", the decoder updates the state, puts
            //  the one byte from window to current position in window and goes to next
            //  MATCH/LITERAL symbol."
            update_state_after_short_rep();

            TRY(copy_match_to_buffer(1));

            continue;
        }

        // Note: We don't need to do anything specific for "Rep Match 0", we just need to make sure to not
        //       run the detection for other match types and to not switch around the distance history.

        if (match_type == MatchType::RepMatch1) {
            u32 distance = m_rep1;
            m_rep1 = m_rep0;
            m_rep0 = distance;
        }

        if (match_type == MatchType::RepMatch2) {
            u32 distance = m_rep2;
            m_rep2 = m_rep1;
            m_rep1 = m_rep0;
            m_rep0 = distance;
        }

        if (match_type == MatchType::RepMatch3) {
            u32 distance = m_rep3;
            m_rep3 = m_rep2;
            m_rep2 = m_rep1;
            m_rep1 = m_rep0;
            m_rep0 = distance;
        }

        // "In other cases (Rep Match 0/1/2/3), it decodes the zero-based
        //  length of match with "RepLenDecoder" decoder."
        u16 normalized_length = TRY(decode_normalized_match_length(m_rep_length_coder));

        // "Then it updates the state."
        update_state_after_rep();

        // "Then the decoder must copy match bytes as described in
        //  "The Match symbols copying" section."
        TRY(copy_match_to_buffer(normalized_length + normalized_to_real_match_length_offset));
    }

    if (m_found_end_of_stream_marker || has_reached_expected_data_size()) {
        if (m_options.uncompressed_size.has_value() && m_total_processed_bytes < m_options.uncompressed_size.value())
            return Error::from_string_literal("Found end-of-stream marker earlier than expected");

        if (!is_range_decoder_in_clean_state())
            return Error::from_string_literal("LZMA stream ends in an unclean state");
    }

    return m_dictionary->read(bytes);
}

ErrorOr<size_t> LzmaDecompressor::write_some(ReadonlyBytes)
{
    return Error::from_errno(EBADF);
}

bool LzmaDecompressor::is_eof() const
{
    if (m_dictionary->used_space() > 0)
        return false;

    if (has_reached_expected_data_size())
        return true;

    return m_found_end_of_stream_marker;
}

bool LzmaDecompressor::is_open() const
{
    return true;
}

void LzmaDecompressor::close()
{
}

ErrorOr<NonnullOwnPtr<LzmaCompressor>> LzmaCompressor::create_container(MaybeOwned<Stream> stream, LzmaCompressorOptions const& options)
{
    auto dictionary = TRY(try_make<SearchableCircularBuffer>(TRY(SearchableCircularBuffer::create_empty(options.dictionary_size + largest_real_match_length))));

    // "The LZMA Decoder uses (1 << (lc + lp)) tables with CProb values, where each table contains 0x300 CProb values."
    auto literal_probabilities = TRY(FixedArray<Probability>::create(literal_probability_table_size * (1 << (options.literal_context_bits + options.literal_position_bits))));

    auto header = TRY(LzmaHeader::from_compressor_options(options));
    TRY(stream->write_value(header));

    auto compressor = TRY(adopt_nonnull_own_or_enomem(new (nothrow) LzmaCompressor(move(stream), options, move(dictionary), move(literal_probabilities))));

    return compressor;
}

LzmaCompressor::LzmaCompressor(MaybeOwned<AK::Stream> stream, Compress::LzmaCompressorOptions options, MaybeOwned<SearchableCircularBuffer> dictionary, FixedArray<Compress::LzmaState::Probability> literal_probabilities)
    : LzmaState(move(literal_probabilities))
    , m_stream(move(stream))
    , m_options(move(options))
    , m_dictionary(move(dictionary))
{
}

ErrorOr<Bytes> LzmaCompressor::read_some(Bytes)
{
    return Error::from_errno(EBADF);
}

ErrorOr<size_t> LzmaCompressor::write_some(ReadonlyBytes bytes)
{
    // Fill the input buffer until it's full or until we can't read any more data.
    size_t processed_bytes = min(bytes.size(), largest_real_match_length - m_dictionary->used_space());
    bytes = bytes.trim(processed_bytes);

    while (bytes.size() > 0) {
        auto const written_bytes = m_dictionary->write(bytes);
        bytes = bytes.slice(written_bytes);
    }

    VERIFY(m_dictionary->used_space() <= largest_real_match_length);

    if (m_options.uncompressed_size.has_value() && m_total_processed_bytes + m_dictionary->used_space() > m_options.uncompressed_size.value())
        return Error::from_string_literal("Tried to compress more LZMA data than announced");

    TRY(encode_once());

    // If we read enough data to reach the final uncompressed size, flush automatically.
    // Flushing will handle encoding the remaining data for us and finalize the stream.
    if (m_options.uncompressed_size.has_value() && m_total_processed_bytes + m_dictionary->used_space() >= m_options.uncompressed_size.value())
        TRY(flush());

    return processed_bytes;
}

ErrorOr<void> LzmaCompressor::flush()
{
    if (m_has_flushed_data)
        return Error::from_string_literal("Flushed an LZMA stream twice");

    while (m_dictionary->used_space() > 0)
        TRY(encode_once());

    if (m_options.uncompressed_size.has_value() && m_total_processed_bytes < m_options.uncompressed_size.value())
        return Error::from_string_literal("Flushing LZMA data with known but unreached uncompressed size");

    // The LZMA specification technically also allows both a known size and an end-of-stream marker simultaneously,
    // but LZMA2 rejects them, so skip emitting the end-of-stream marker if we know the uncompressed size.
    if (!m_options.uncompressed_size.has_value())
        TRY(encode_normalized_simple_match(end_of_stream_marker, 0));

    // Shifting the range encoder using the normal operation handles any pending overflows.
    TRY(shift_range_encoder());

    // Now, the remaining bytes are the cached byte, the chain of 0xFF, and the upper 3 bytes of the current `code`.
    // Incrementing the values does not have to be considered as no overflows are pending. The fourth byte is the
    // null byte that we just shifted in, which should not be flushed as it would be extraneous junk data.
    TRY(m_stream->write_value<u8>(m_range_encoder_cached_byte));
    for (size_t i = 0; i < m_range_encoder_ff_chain_length; i++)
        TRY(m_stream->write_value<u8>(0xFF));
    TRY(m_stream->write_value<u8>(m_range_encoder_code >> 24));
    TRY(m_stream->write_value<u8>(m_range_encoder_code >> 16));
    TRY(m_stream->write_value<u8>(m_range_encoder_code >> 8));

    m_has_flushed_data = true;
    return {};
}

bool LzmaCompressor::is_eof() const
{
    return true;
}

bool LzmaCompressor::is_open() const
{
    return !m_has_flushed_data;
}

void LzmaCompressor::close()
{
    if (!m_has_flushed_data) {
        // Note: We need a better API for specifying things like this.
        flush().release_value_but_fixme_should_propagate_errors();
    }
}

LzmaCompressor::~LzmaCompressor()
{
    if (!m_has_flushed_data) {
        // Note: We need a better API for specifying things like this.
        flush().release_value_but_fixme_should_propagate_errors();
    }
}

}
