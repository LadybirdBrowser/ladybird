/*
 * Copyright (c) 2022, Michiel Visser <opensource@webmichiel.nl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <AK/QuickSort.h>
#include <LibCompress/Brotli.h>
#include <LibCompress/BrotliDictionary.h>

namespace Compress {

ErrorOr<size_t> Brotli::CanonicalCode::read_symbol(LittleEndianInputBitStream& input_stream) const
{
    size_t code_bits = 1;

    while (code_bits < (1 << 16)) {
        // FIXME: This is very inefficient and could greatly be improved by implementing this
        //        algorithm: https://www.hanshq.net/zip.html#huffdec
        size_t index;
        if (binary_search(m_symbol_codes.span(), code_bits, &index))
            return m_symbol_values[index];

        code_bits = (code_bits << 1) | TRY(input_stream.read_bit());
    }

    return Error::from_string_literal("no matching code found");
}

BrotliDecompressionStream::BrotliDecompressionStream(MaybeOwned<Stream> stream)
    : m_input_stream(move(stream))
{
}

ErrorOr<size_t> BrotliDecompressionStream::read_window_length()
{
    if (TRY(m_input_stream.read_bit())) {
        switch (TRY(m_input_stream.read_bits(3))) {
        case 0: {
            switch (TRY(m_input_stream.read_bits(3))) {
            case 0:
                return 17;
            case 1:
                return Error::from_string_literal("invalid window length");
            case 2:
                return 10;
            case 3:
                return 11;
            case 4:
                return 12;
            case 5:
                return 13;
            case 6:
                return 14;
            case 7:
                return 15;
            default:
                VERIFY_NOT_REACHED();
            }
        }
        case 1:
            return 18;
        case 2:
            return 19;
        case 3:
            return 20;
        case 4:
            return 21;
        case 5:
            return 22;
        case 6:
            return 23;
        case 7:
            return 24;
        default:
            VERIFY_NOT_REACHED();
        }
    } else {
        return 16;
    }
}

ErrorOr<size_t> BrotliDecompressionStream::read_size_number_of_nibbles()
{
    switch (TRY(m_input_stream.read_bits(2))) {
    case 0:
        return 4;
    case 1:
        return 5;
    case 2:
        return 6;
    case 3:
        return 0;
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<size_t> BrotliDecompressionStream::read_variable_length()
{
    //  Value    Bit Pattern
    //   -----    -----------
    //     1                0
    //     2             0001
    //   3..4           x0011
    //   5..8          xx0101
    //   9..16        xxx0111
    //  17..32       xxxx1001
    //  33..64      xxxxx1011
    //  65..128    xxxxxx1101
    // 129..256   xxxxxxx1111

    if (TRY(m_input_stream.read_bit())) {
        switch (TRY(m_input_stream.read_bits(3))) {
        case 0:
            return 2;
        case 1:
            return 3 + TRY(m_input_stream.read_bits(1));
        case 2:
            return 5 + TRY(m_input_stream.read_bits(2));
        case 3:
            return 9 + TRY(m_input_stream.read_bits(3));
        case 4:
            return 17 + TRY(m_input_stream.read_bits(4));
        case 5:
            return 33 + TRY(m_input_stream.read_bits(5));
        case 6:
            return 65 + TRY(m_input_stream.read_bits(6));
        case 7:
            return 129 + TRY(m_input_stream.read_bits(7));
        default:
            VERIFY_NOT_REACHED();
        }
    } else {
        return 1;
    }
}

ErrorOr<size_t> Brotli::CanonicalCode::read_complex_prefix_code_length(LittleEndianInputBitStream& stream)
{
    // Symbol   Code
    // ------   ----
    // 0          00
    // 1        0111
    // 2         011
    // 3          10
    // 4          01
    // 5        1111

    switch (TRY(stream.read_bits(2))) {
    case 0:
        return 0;
    case 1:
        return 4;
    case 2:
        return 3;
    case 3: {
        if (TRY(stream.read_bit()) == 0) {
            return 2;
        } else {
            if (TRY(stream.read_bit()) == 0) {
                return 1;
            } else {
                return 5;
            }
        }
    }
    default:
        VERIFY_NOT_REACHED();
    }
}

ErrorOr<Brotli::CanonicalCode> Brotli::CanonicalCode::read_prefix_code(LittleEndianInputBitStream& stream, size_t alphabet_size)
{
    size_t hskip = TRY(stream.read_bits(2));

    if (hskip == 1)
        return TRY(read_simple_prefix_code(stream, alphabet_size));

    return TRY(read_complex_prefix_code(stream, alphabet_size, hskip));
}

ErrorOr<Brotli::CanonicalCode> Brotli::CanonicalCode::read_simple_prefix_code(LittleEndianInputBitStream& stream, size_t alphabet_size)
{
    CanonicalCode code {};

    size_t number_of_symbols = 1 + TRY(stream.read_bits(2));

    size_t symbol_size = 0;
    while ((1u << symbol_size) < alphabet_size)
        symbol_size++;

    Vector<size_t> symbols;
    for (size_t i = 0; i < number_of_symbols; i++) {
        size_t symbol = TRY(stream.read_bits(symbol_size));
        symbols.append(symbol);

        if (symbol >= alphabet_size)
            return Error::from_string_literal("symbol larger than alphabet");
    }

    if (number_of_symbols == 1) {
        code.m_symbol_codes.append(0b1);
        code.m_symbol_values = move(symbols);
    } else if (number_of_symbols == 2) {
        code.m_symbol_codes.extend({ 0b10, 0b11 });
        if (symbols[0] > symbols[1])
            swap(symbols[0], symbols[1]);
        code.m_symbol_values = move(symbols);
    } else if (number_of_symbols == 3) {
        code.m_symbol_codes.extend({ 0b10, 0b110, 0b111 });
        if (symbols[1] > symbols[2])
            swap(symbols[1], symbols[2]);
        code.m_symbol_values = move(symbols);
    } else if (number_of_symbols == 4) {
        bool tree_select = TRY(stream.read_bit());
        if (tree_select) {
            code.m_symbol_codes.extend({ 0b10, 0b110, 0b1110, 0b1111 });
            if (symbols[2] > symbols[3])
                swap(symbols[2], symbols[3]);
            code.m_symbol_values = move(symbols);
        } else {
            code.m_symbol_codes.extend({ 0b100, 0b101, 0b110, 0b111 });
            quick_sort(symbols);
            code.m_symbol_values = move(symbols);
        }
    }

    return code;
}

ErrorOr<Brotli::CanonicalCode> Brotli::CanonicalCode::read_complex_prefix_code(LittleEndianInputBitStream& stream, size_t alphabet_size, size_t hskip)
{
    // hskip should only be 0, 2 or 3
    VERIFY(hskip != 1);
    VERIFY(hskip <= 3);

    // Read the prefix code_value that is used to encode the actual prefix code_value
    size_t const symbol_mapping[18] = { 1, 2, 3, 4, 0, 5, 17, 6, 16, 7, 8, 9, 10, 11, 12, 13, 14, 15 };
    size_t code_length[18] { 0 };
    size_t code_length_counts[6] { 0 };

    size_t sum = 0;
    size_t number_of_non_zero_symbols = 0;
    for (size_t i = hskip; i < 18; i++) {
        size_t len = TRY(read_complex_prefix_code_length(stream));
        code_length[symbol_mapping[i]] = len;

        if (len != 0) {
            code_length_counts[len]++;
            sum += (32 >> len);
            number_of_non_zero_symbols++;
        }

        if (sum == 32)
            break;
        else if (sum > 32)
            return Error::from_string_literal("invalid prefix code");
    }

    CanonicalCode temp_code;
    if (number_of_non_zero_symbols > 1) {
        size_t code_value = 0;
        for (size_t bits = 1; bits <= 5; bits++) {
            code_value = (code_value + code_length_counts[bits - 1]) << 1;
            size_t current_code_value = code_value;

            for (size_t i = 0; i < 18; i++) {
                size_t len = code_length[i];
                if (len == bits) {
                    temp_code.m_symbol_codes.append((1 << bits) | current_code_value);
                    temp_code.m_symbol_values.append(i);
                    current_code_value++;
                }
            }
        }
    } else {
        for (size_t i = 0; i < 18; i++) {
            size_t len = code_length[i];
            if (len != 0) {
                temp_code.m_symbol_codes.append(1);
                temp_code.m_symbol_values.append(i);
                break;
            }
        }
    }

    // Read the actual prefix code_value
    sum = 0;
    size_t i = 0;

    size_t previous_non_zero_code_length = 8;
    size_t last_symbol = 0;
    size_t last_repeat = 0;

    Vector<size_t> result_symbols;
    Vector<size_t> result_lengths;
    size_t result_lengths_count[16] { 0 };
    while (i < alphabet_size) {
        auto symbol = TRY(temp_code.read_symbol(stream));

        if (symbol < 16) {
            result_symbols.append(i);
            result_lengths.append(symbol);
            result_lengths_count[symbol]++;

            if (symbol != 0) {
                previous_non_zero_code_length = symbol;
                sum += (32768 >> symbol);
                if (sum == 32768)
                    break;
                else if (sum > 32768)
                    return Error::from_string_literal("invalid prefix code");
            }

            last_repeat = 0;
            i++;
        } else if (symbol == 16) {
            size_t repeat_count = 0;
            if (last_symbol == 16 && last_repeat != 0) {
                repeat_count = (4 * (last_repeat - 2));
            } else {
                last_repeat = 0;
            }
            repeat_count += 3 + TRY(stream.read_bits(2));

            for (size_t rep = 0; rep < (repeat_count - last_repeat); rep++) {
                result_symbols.append(i);
                result_lengths.append(previous_non_zero_code_length);
                result_lengths_count[previous_non_zero_code_length]++;

                if (previous_non_zero_code_length != 0) {
                    sum += (32768 >> previous_non_zero_code_length);
                    if (sum == 32768)
                        break;
                    else if (sum > 32768)
                        return Error::from_string_literal("invalid prefix code");
                }

                i++;
                if (i >= alphabet_size)
                    break;
            }
            if (sum == 32768)
                break;
            VERIFY(sum < 32768);

            last_repeat = repeat_count;
        } else if (symbol == 17) {
            size_t repeat_count = 0;
            if (last_symbol == 17 && last_repeat != 0) {
                repeat_count = (8 * (last_repeat - 2));
            } else {
                last_repeat = 0;
            }
            repeat_count += 3 + TRY(stream.read_bits(3));

            i += (repeat_count - last_repeat);
            last_repeat = repeat_count;
        }

        last_symbol = symbol;
    }
    result_lengths_count[0] = 0;

    CanonicalCode final_code;

    size_t code_value = 0;
    for (size_t bits = 1; bits < 16; bits++) {
        code_value = (code_value + result_lengths_count[bits - 1]) << 1;
        size_t current_code_value = code_value;

        for (size_t n = 0; n < result_symbols.size(); n++) {
            size_t len = result_lengths[n];
            if (len == bits) {
                final_code.m_symbol_codes.append((1 << bits) | current_code_value);
                final_code.m_symbol_values.append(result_symbols[n]);
                current_code_value++;
            }
        }
    }

    return final_code;
}

static void inverse_move_to_front_transform(Span<u8> v)
{
    // RFC 7932 section 7.3
    u8 mtf[256];
    for (size_t i = 0; i < 256; ++i) {
        mtf[i] = (u8)i;
    }
    for (size_t i = 0; i < v.size(); ++i) {
        u8 index = v[i];
        u8 value = mtf[index];
        v[i] = value;
        for (; index; --index) {
            mtf[index] = mtf[index - 1];
        }
        mtf[0] = value;
    }
}

ErrorOr<void> BrotliDecompressionStream::read_context_map(size_t number_of_codes, Vector<u8>& context_map, size_t context_map_size)
{
    bool use_run_length_encoding = TRY(m_input_stream.read_bit());
    size_t run_length_encoding_max = 0;
    if (use_run_length_encoding) {
        run_length_encoding_max = 1 + TRY(m_input_stream.read_bits(4));
    }

    auto const code = TRY(CanonicalCode::read_prefix_code(m_input_stream, number_of_codes + run_length_encoding_max));

    size_t i = 0;
    while (i < context_map_size) {
        size_t symbol = TRY(code.read_symbol(m_input_stream));

        if (symbol <= run_length_encoding_max) {
            size_t repeat_base = 1 << symbol;
            size_t repeat_additional = TRY(m_input_stream.read_bits(symbol));
            size_t repeat_count = repeat_base + repeat_additional;
            while (repeat_count--) {
                context_map.append(0);
                i++;
            }
        } else {
            size_t value = symbol - run_length_encoding_max;
            context_map.append(value);
            i++;
        }
    }

    bool inverse_move_to_front = TRY(m_input_stream.read_bit());
    if (inverse_move_to_front)
        inverse_move_to_front_transform(context_map.span());

    return {};
}

ErrorOr<void> BrotliDecompressionStream::read_block_configuration(Block& block)
{
    size_t blocks_of_type = TRY(read_variable_length());

    block.type = 0;
    block.type_previous = 1;
    block.number_of_types = blocks_of_type;

    if (blocks_of_type == 1) {
        block.length = 16 * MiB;
        block.type_code = {};
        block.length_code = {};
    } else {
        block.type_code = TRY(CanonicalCode::read_prefix_code(m_input_stream, 2 + blocks_of_type));
        block.length_code = TRY(CanonicalCode::read_prefix_code(m_input_stream, 26));
        TRY(block_update_length(block));
    }

    return {};
}

ErrorOr<void> BrotliDecompressionStream::block_update_length(Block& block)
{
    size_t const block_length_code_base[26] { 1, 5, 9, 13, 17, 25, 33, 41, 49, 65, 81, 97, 113, 145, 177, 209, 241, 305, 369, 497, 753, 1265, 2289, 4337, 8433, 16625 };
    size_t const block_length_code_extra[26] { 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 6, 6, 7, 8, 9, 10, 11, 12, 13, 24 };

    size_t symbol = TRY(block.length_code.read_symbol(m_input_stream));
    size_t block_length = block_length_code_base[symbol] + TRY(m_input_stream.read_bits(block_length_code_extra[symbol]));

    block.length = block_length;
    return {};
}

ErrorOr<void> BrotliDecompressionStream::block_read_new_state(Block& block)
{
    size_t block_type_symbol = TRY(block.type_code.read_symbol(m_input_stream));
    TRY(block_update_length(block));

    if (block_type_symbol == 0) {
        swap(block.type, block.type_previous);
    } else if (block_type_symbol == 1) {
        block.type_previous = block.type;
        block.type = (block.type + 1) % block.number_of_types;
    } else {
        block.type_previous = block.type;
        block.type = block_type_symbol - 2;
    }

    return {};
}

size_t BrotliDecompressionStream::literal_code_index_from_context()
{
    size_t const context_id_lut0[256] {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 4, 4, 0, 0, 4, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        8, 12, 16, 12, 12, 20, 12, 16, 24, 28, 12, 12, 32, 12, 36, 12,
        44, 44, 44, 44, 44, 44, 44, 44, 44, 44, 32, 32, 24, 40, 28, 12,
        12, 48, 52, 52, 52, 48, 52, 52, 52, 48, 52, 52, 52, 52, 52, 48,
        52, 52, 52, 52, 52, 48, 52, 52, 52, 52, 52, 24, 12, 28, 12, 12,
        12, 56, 60, 60, 60, 56, 60, 60, 60, 56, 60, 60, 60, 60, 60, 56,
        60, 60, 60, 60, 60, 56, 60, 60, 60, 60, 60, 24, 12, 28, 12, 0,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1,
        2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
        2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
        2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3,
        2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3
    };
    size_t const context_id_lut1[256] {
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1,
        1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1,
        1, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 1, 1, 1, 1, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2
    };
    size_t const context_id_lut2[256] {
        0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
        6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 7
    };

    size_t context_mode = m_literal_context_modes[m_literal_block.type];
    size_t context_id;
    switch (context_mode) {
    case 0:
        context_id = m_lookback_buffer.value().lookback(1, 0) & 0x3f;
        break;
    case 1:
        context_id = m_lookback_buffer.value().lookback(1, 0) >> 2;
        break;
    case 2:
        context_id = context_id_lut0[m_lookback_buffer.value().lookback(1, 0)] | context_id_lut1[m_lookback_buffer.value().lookback(2, 0)];
        break;
    case 3:
        context_id = (context_id_lut2[m_lookback_buffer.value().lookback(1, 0)] << 3) | context_id_lut2[m_lookback_buffer.value().lookback(2, 0)];
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    size_t literal_code_index = m_context_mapping_literal[64 * m_literal_block.type + context_id];
    return literal_code_index;
}

ErrorOr<Bytes> BrotliDecompressionStream::read_some(Bytes output_buffer)
{
    size_t bytes_read = 0;
    while (bytes_read < output_buffer.size()) {
        if (m_current_state == State::WindowSize) {
            size_t window_bits = TRY(read_window_length());
            m_window_size = (1 << window_bits) - 16;

            m_lookback_buffer = TRY(LookbackBuffer::try_create(m_window_size));

            m_current_state = State::Idle;
        } else if (m_current_state == State::Idle) {
            // If the final block was read, we are done decompressing
            if (m_read_final_block)
                break;

            // RFC 7932 section 9.1
            //
            // 1 bit:  ISLAST, set to 1 if this is the last meta-block
            m_read_final_block = TRY(m_input_stream.read_bit());
            if (m_read_final_block) {
                // 1 bit:  ISLASTEMPTY, if set to 1, the meta-block is empty; this
                //       field is only present if ISLAST bit is set -- if it is 1,
                //       then the meta-block and the brotli stream ends at that
                //       bit, with any remaining bits in the last byte of the
                //       compressed stream filled with zeros (if the fill bits are
                //       not zero, then the stream should be rejected as invalid)
                bool is_last_block_empty = TRY(m_input_stream.read_bit());
                // If the last block is empty we are done decompressing
                if (is_last_block_empty)
                    break;
            }

            // 2 bits: MNIBBLES, number of nibbles to represent the uncompressed
            //         length
            size_t size_number_of_nibbles = TRY(read_size_number_of_nibbles());

            // If MNIBBLES is 0, the meta-block is empty, i.e., it does
            // not generate any uncompressed data.  In this case, the
            // rest of the meta-block has the following format:
            if (size_number_of_nibbles == 0) {

                // 1 bit:  reserved, must be zero
                bool reserved = TRY(m_input_stream.read_bit());
                if (reserved)
                    return Error::from_string_literal("invalid reserved bit");

                // 2 bits: MSKIPBYTES, number of bytes to represent
                //         metadata length
                //
                // MSKIPBYTES * 8 bits: MSKIPLEN - 1, where MSKIPLEN is
                //    the number of metadata bytes; this field is
                //    only present if MSKIPBYTES is positive;
                //    otherwise, MSKIPLEN is 0 (if MSKIPBYTES is
                //    greater than 1, and the last byte is all
                //    zeros, then the stream should be rejected as
                //    invalid)
                size_t skip_bytes = TRY(m_input_stream.read_bits(2));
                if (skip_bytes == 0) {
                    // 0..7 bits: fill bits until the next byte boundary,
                    //         must be all zeros
                    u8 remainder = m_input_stream.align_to_byte_boundary();
                    if (remainder != 0)
                        return Error::from_string_literal("remainder bits are non-zero");
                    continue;
                }

                // MSKIPLEN bytes of metadata, not part of the
                //         uncompressed data or the sliding window
                size_t skip_length = 1 + TRY(m_input_stream.read_bits(8 * skip_bytes));

                u8 remainder = m_input_stream.align_to_byte_boundary();
                if (remainder != 0)
                    return Error::from_string_literal("remainder bits are non-zero");

                // Discard meta-data bytes
                u8 temp_buffer[4096];
                Bytes temp_bytes { temp_buffer, 4096 };
                while (skip_length > 0) {
                    Bytes temp_bytes_slice = temp_bytes.slice(0, min(4096, skip_length));
                    auto metadata_bytes = TRY(m_input_stream.read_some(temp_bytes_slice));
                    if (metadata_bytes.is_empty())
                        return Error::from_string_literal("eof");
                    if (metadata_bytes.last() == 0)
                        return Error::from_string_literal("invalid stream");
                    skip_length -= metadata_bytes.size();
                }

                continue;
            }

            size_t uncompressed_size = 1 + TRY(m_input_stream.read_bits(4 * size_number_of_nibbles));

            // 1 bit:  ISUNCOMPRESSED, if set to 1, any bits of compressed data
            //       up to the next byte boundary are ignored, and the rest of
            //       the meta-block contains MLEN bytes of literal data; this
            //       field is only present if the ISLAST bit is not set (if the
            //       ignored bits are not all zeros, the stream should be
            //       rejected as invalid)
            bool is_uncompressed = false;
            if (!m_read_final_block)
                is_uncompressed = TRY(m_input_stream.read_bit());

            m_bytes_left = uncompressed_size;
            if (is_uncompressed) {
                u8 remainder = m_input_stream.align_to_byte_boundary();
                if (remainder != 0)
                    return Error::from_string_literal("remainder is non-zero");
                m_current_state = State::UncompressedData;
            } else {
                TRY(read_block_configuration(m_literal_block));
                TRY(read_block_configuration(m_insert_and_copy_block));
                TRY(read_block_configuration(m_distance_block));

                m_postfix_bits = TRY(m_input_stream.read_bits(2));
                m_direct_distances = TRY(m_input_stream.read_bits(4)) << m_postfix_bits;

                m_literal_context_modes.clear();
                for (size_t i = 0; i < m_literal_block.number_of_types; i++) {
                    size_t context_mode = TRY(m_input_stream.read_bits(2));
                    m_literal_context_modes.append(context_mode);
                }

                m_context_mapping_literal.clear();
                size_t number_of_literal_codes = TRY(read_variable_length());
                if (number_of_literal_codes == 1) {
                    for (size_t i = 0; i < 64 * m_literal_block.number_of_types; i++)
                        m_context_mapping_literal.append(0);
                } else {
                    TRY(read_context_map(number_of_literal_codes, m_context_mapping_literal, 64 * m_literal_block.number_of_types));
                }

                m_context_mapping_distance.clear();
                size_t number_of_distance_codes = TRY(read_variable_length());
                if (number_of_distance_codes == 1) {
                    for (size_t i = 0; i < 4 * m_distance_block.number_of_types; i++)
                        m_context_mapping_distance.append(0);
                } else {
                    TRY(read_context_map(number_of_distance_codes, m_context_mapping_distance, 4 * m_distance_block.number_of_types));
                }

                m_literal_codes.clear();
                for (size_t i = 0; i < number_of_literal_codes; i++) {
                    m_literal_codes.append(TRY(CanonicalCode::read_prefix_code(m_input_stream, 256)));
                }

                m_insert_and_copy_codes.clear();
                for (size_t i = 0; i < m_insert_and_copy_block.number_of_types; i++) {
                    m_insert_and_copy_codes.append(TRY(CanonicalCode::read_prefix_code(m_input_stream, 704)));
                }

                m_distance_codes.clear();
                for (size_t i = 0; i < number_of_distance_codes; i++) {
                    m_distance_codes.append(TRY(CanonicalCode::read_prefix_code(m_input_stream, 16 + m_direct_distances + (48 << m_postfix_bits))));
                }

                m_current_state = State::CompressedCommand;
            }
        } else if (m_current_state == State::UncompressedData) {
            size_t number_of_fitting_bytes = min(output_buffer.size() - bytes_read, m_bytes_left);
            VERIFY(number_of_fitting_bytes > 0);

            auto uncompressed_bytes = TRY(m_input_stream.read_some(output_buffer.slice(bytes_read, number_of_fitting_bytes)));
            if (uncompressed_bytes.is_empty())
                return Error::from_string_literal("eof");

            // TODO: Replace the home-grown LookbackBuffer with AK::CircularBuffer.
            for (auto c : uncompressed_bytes)
                m_lookback_buffer.value().write(c);

            m_bytes_left -= uncompressed_bytes.size();
            bytes_read += uncompressed_bytes.size();

            // If all bytes were read, return to the idle state
            if (m_bytes_left == 0)
                m_current_state = State::Idle;
        } else if (m_current_state == State::CompressedCommand) {
            if (m_insert_and_copy_block.length == 0) {
                TRY(block_read_new_state(m_insert_and_copy_block));
            }
            m_insert_and_copy_block.length--;

            size_t insert_and_copy_symbol = TRY(m_insert_and_copy_codes[m_insert_and_copy_block.type].read_symbol(m_input_stream));

            size_t const insert_length_code_base[11] { 0, 0, 0, 0, 8, 8, 0, 16, 8, 16, 16 };
            size_t const copy_length_code_base[11] { 0, 8, 0, 8, 0, 8, 16, 0, 16, 8, 16 };
            bool const implicit_zero_distance[11] { true, true, false, false, false, false, false, false, false, false, false };

            size_t insert_and_copy_index = insert_and_copy_symbol >> 6;
            size_t insert_length_code_offset = (insert_and_copy_symbol >> 3) & 0b111;
            size_t copy_length_code_offset = insert_and_copy_symbol & 0b111;

            size_t insert_length_code = insert_length_code_base[insert_and_copy_index] + insert_length_code_offset;
            size_t copy_length_code = copy_length_code_base[insert_and_copy_index] + copy_length_code_offset;

            m_implicit_zero_distance = implicit_zero_distance[insert_and_copy_index];

            size_t const insert_length_base[24] { 0, 1, 2, 3, 4, 5, 6, 8, 10, 14, 18, 26, 34, 50, 66, 98, 130, 194, 322, 578, 1090, 2114, 6210, 22594 };
            size_t const insert_length_extra[24] { 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 12, 14, 24 };
            size_t const copy_length_base[24] { 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 14, 18, 22, 30, 38, 54, 70, 102, 134, 198, 326, 582, 1094, 2118 };
            size_t const copy_length_extra[24] { 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 7, 8, 9, 10, 24 };

            m_insert_length = insert_length_base[insert_length_code] + TRY(m_input_stream.read_bits(insert_length_extra[insert_length_code]));
            m_copy_length = copy_length_base[copy_length_code] + TRY(m_input_stream.read_bits(copy_length_extra[copy_length_code]));

            if (m_insert_length > 0) {
                m_current_state = State::CompressedLiteral;
            } else {
                m_current_state = State::CompressedDistance;
            }
        } else if (m_current_state == State::CompressedLiteral) {
            if (m_literal_block.length == 0) {
                TRY(block_read_new_state(m_literal_block));
            }
            m_literal_block.length--;

            size_t literal_code_index = literal_code_index_from_context();
            size_t literal_value = TRY(m_literal_codes[literal_code_index].read_symbol(m_input_stream));

            output_buffer[bytes_read] = literal_value;
            m_lookback_buffer.value().write(literal_value);
            bytes_read++;
            m_insert_length--;
            m_bytes_left--;

            if (m_bytes_left == 0)
                m_current_state = State::Idle;
            else if (m_insert_length == 0)
                m_current_state = State::CompressedDistance;
        } else if (m_current_state == State::CompressedDistance) {
            size_t distance_symbol;
            if (m_implicit_zero_distance) {
                distance_symbol = 0;
            } else {
                if (m_distance_block.length == 0) {
                    TRY(block_read_new_state(m_distance_block));
                }
                m_distance_block.length--;

                size_t context_id = clamp(m_copy_length - 2, 0, 3);
                size_t distance_code_index = m_context_mapping_distance[4 * m_distance_block.type + context_id];

                distance_symbol = TRY(m_distance_codes[distance_code_index].read_symbol(m_input_stream));
            }

            size_t distance;
            bool reuse_previous_distance = false;
            if (distance_symbol < 16) {
                switch (distance_symbol) {
                case 0:
                    distance = m_distances[0];
                    reuse_previous_distance = true;
                    break;
                case 1:
                    distance = m_distances[1];
                    break;
                case 2:
                    distance = m_distances[2];
                    break;
                case 3:
                    distance = m_distances[3];
                    break;
                case 4:
                    distance = m_distances[0] - 1;
                    break;
                case 5:
                    distance = m_distances[0] + 1;
                    break;
                case 6:
                    distance = m_distances[0] - 2;
                    break;
                case 7:
                    distance = m_distances[0] + 2;
                    break;
                case 8:
                    distance = m_distances[0] - 3;
                    break;
                case 9:
                    distance = m_distances[0] + 3;
                    break;
                case 10:
                    distance = m_distances[1] - 1;
                    break;
                case 11:
                    distance = m_distances[1] + 1;
                    break;
                case 12:
                    distance = m_distances[1] - 2;
                    break;
                case 13:
                    distance = m_distances[1] + 2;
                    break;
                case 14:
                    distance = m_distances[1] - 3;
                    break;
                case 15:
                    distance = m_distances[1] + 3;
                    break;
                }
            } else if (distance_symbol < 16 + m_direct_distances) {
                distance = distance_symbol - 15;
            } else {
                size_t POSTFIX_MASK = (1 << m_postfix_bits) - 1;

                size_t ndistbits = 1 + ((distance_symbol - m_direct_distances - 16) >> (m_postfix_bits + 1));
                size_t dextra = TRY(m_input_stream.read_bits(ndistbits));

                size_t hcode = (distance_symbol - m_direct_distances - 16) >> m_postfix_bits;
                size_t lcode = (distance_symbol - m_direct_distances - 16) & POSTFIX_MASK;
                size_t offset = ((2 + (hcode & 1)) << ndistbits) - 4;
                distance = ((offset + dextra) << m_postfix_bits) + lcode + m_direct_distances + 1;
            }
            m_distance = distance;

            size_t total_written = m_lookback_buffer.value().total_written();
            size_t max_lookback = min(total_written, m_window_size);

            if (distance > max_lookback) {
                size_t word_index = distance - (max_lookback + 1);
                m_dictionary_data = TRY(BrotliDictionary::lookup_word(word_index, m_copy_length));
                m_copy_length = m_dictionary_data.size();

                if (m_copy_length == 0)
                    m_current_state = State::CompressedCommand;
                else
                    m_current_state = State::CompressedDictionary;
            } else {
                if (!reuse_previous_distance) {
                    m_distances[3] = m_distances[2];
                    m_distances[2] = m_distances[1];
                    m_distances[1] = m_distances[0];
                    m_distances[0] = distance;
                }

                m_current_state = State::CompressedCopy;
            }
        } else if (m_current_state == State::CompressedCopy) {
            u8 copy_value = m_lookback_buffer.value().lookback(m_distance);

            output_buffer[bytes_read] = copy_value;
            m_lookback_buffer.value().write(copy_value);
            bytes_read++;
            m_copy_length--;
            m_bytes_left--;

            if (m_bytes_left == 0)
                m_current_state = State::Idle;
            else if (m_copy_length == 0)
                m_current_state = State::CompressedCommand;
        } else if (m_current_state == State::CompressedDictionary) {
            size_t offset = m_dictionary_data.size() - m_copy_length;
            u8 dictionary_value = m_dictionary_data[offset];

            output_buffer[bytes_read] = dictionary_value;
            m_lookback_buffer.value().write(dictionary_value);
            bytes_read++;
            m_copy_length--;
            m_bytes_left--;

            if (m_bytes_left == 0)
                m_current_state = State::Idle;
            else if (m_copy_length == 0)
                m_current_state = State::CompressedCommand;
        }
    }

    return output_buffer.slice(0, bytes_read);
}

bool BrotliDecompressionStream::is_eof() const
{
    return m_read_final_block && m_current_state == State::Idle;
}

}
