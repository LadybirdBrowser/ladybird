/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/ByteString.h>
#include <AK/CharacterTypes.h>
#include <AK/Error.h>
#include <AK/Format.h>
#include <AK/GenericLexer.h>
#include <AK/HashMap.h>
#include <AK/IntegralMath.h>
#include <AK/LexicalPath.h>
#include <AK/String.h>
#include <AK/StringBuilder.h>
#include <AK/StringFloatingPointConversions.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#if defined(AK_OS_SERENITY)
#    include <serenity.h>
#endif

#if defined(AK_OS_ANDROID)
#    include <android/log.h>
#endif

#if defined(AK_OS_WINDOWS)
#    include <AK/Windows.h>
#endif

namespace AK {

class FormatParser : public GenericLexer {
public:
    struct FormatSpecifier {
        StringView flags;
        size_t index;
    };

    explicit FormatParser(StringView input);

    StringView consume_literal();
    bool consume_number(size_t& value);
    bool consume_specifier(FormatSpecifier& specifier);
    bool consume_replacement_field(size_t& index);
};

namespace {

static constexpr size_t use_next_index = NumericLimits<size_t>::max();

// The worst case is that we have the largest 64-bit value formatted as binary number, this would take
// 65 bytes (85 bytes with separators). Choosing a larger power of two won't hurt and is a bit of mitigation against out-of-bounds accesses.
static constexpr size_t convert_unsigned_to_string(u64 value, Array<u8, 128>& buffer, u8 base, bool upper_case, bool use_separator)
{
    VERIFY(base >= 2 && base <= 16);

    constexpr char const* lowercase_lookup = "0123456789abcdef";
    constexpr char const* uppercase_lookup = "0123456789ABCDEF";

    if (value == 0) {
        buffer[0] = '0';
        return 1;
    }

    size_t used = 0;
    size_t digit_count = 0;
    while (value > 0) {
        if (upper_case)
            buffer[used++] = uppercase_lookup[value % base];
        else
            buffer[used++] = lowercase_lookup[value % base];

        digit_count++;
        value /= base;

        if (use_separator && value > 0 && digit_count % 3 == 0)
            buffer[used++] = ',';
    }

    for (size_t i = 0; i < used / 2; ++i)
        swap(buffer[i], buffer[used - i - 1]);

    return used;
}

ErrorOr<void> vformat_impl(TypeErasedFormatParams& params, FormatBuilder& builder, FormatParser& parser)
{
    auto const literal = parser.consume_literal();
    TRY(builder.put_literal(literal));

    FormatParser::FormatSpecifier specifier;
    if (!parser.consume_specifier(specifier)) {
        VERIFY(parser.is_eof());
        return {};
    }

    if (specifier.index == use_next_index)
        specifier.index = params.take_next_index();

    auto& parameter = params.parameters().at(specifier.index);

    FormatParser argparser { specifier.flags };
    TRY(parameter.visit([&]<typename T>(T const& value) {
        if constexpr (IsSame<T, TypeErasedParameter::CustomType>) {
            return value.formatter(params, builder, argparser, value.value);
        } else {
            return __format_value<T>(params, builder, argparser, &value);
        }
    }));
    TRY(vformat_impl(params, builder, parser));
    return {};
}

} // namespace AK::{anonymous}

FormatParser::FormatParser(StringView input)
    : GenericLexer(input)
{
}
StringView FormatParser::consume_literal()
{
    auto const begin = tell();

    while (!is_eof()) {
        if (consume_specific("{{"sv))
            continue;

        if (consume_specific("}}"sv))
            continue;

        if (next_is(is_any_of("{}"sv)))
            return m_input.substring_view(begin, tell() - begin);

        consume();
    }

    return m_input.substring_view(begin);
}
bool FormatParser::consume_number(size_t& value)
{
    value = 0;

    bool consumed_at_least_one = false;
    while (next_is(is_ascii_digit)) {
        value *= 10;
        value += parse_ascii_digit(consume());
        consumed_at_least_one = true;
    }

    return consumed_at_least_one;
}
bool FormatParser::consume_specifier(FormatSpecifier& specifier)
{
    VERIFY(!next_is('}'));

    if (!consume_specific('{'))
        return false;

    if (!consume_number(specifier.index))
        specifier.index = use_next_index;

    if (consume_specific(':')) {
        auto const begin = tell();

        size_t level = 1;
        while (level > 0) {
            VERIFY(!is_eof());

            if (consume_specific('{')) {
                ++level;
                continue;
            }

            if (consume_specific('}')) {
                --level;
                continue;
            }

            consume();
        }

        specifier.flags = m_input.substring_view(begin, tell() - begin - 1);
    } else {
        if (!consume_specific('}'))
            VERIFY_NOT_REACHED();

        specifier.flags = ""sv;
    }

    return true;
}
bool FormatParser::consume_replacement_field(size_t& index)
{
    if (!consume_specific('{'))
        return false;

    if (!consume_number(index))
        index = use_next_index;

    if (!consume_specific('}'))
        VERIFY_NOT_REACHED();

    return true;
}

ErrorOr<void> FormatBuilder::put_padding(char fill, size_t amount)
{
    for (size_t i = 0; i < amount; ++i)
        TRY(m_builder.try_append(fill));
    return {};
}
ErrorOr<void> FormatBuilder::put_literal(StringView value)
{
    for (size_t i = 0; i < value.length(); ++i) {
        TRY(m_builder.try_append(value[i]));
        if (value[i] == '{' || value[i] == '}')
            ++i;
    }
    return {};
}

ErrorOr<void> FormatBuilder::put_string(
    StringView value,
    Align align,
    size_t min_width,
    size_t max_width,
    char fill)
{
    auto const used_by_string = min(max_width, value.length());
    auto const used_by_padding = max(min_width, used_by_string) - used_by_string;

    if (used_by_string < value.length())
        value = value.substring_view(0, used_by_string);

    if (align == Align::Left || align == Align::Default) {
        TRY(m_builder.try_append(value));
        TRY(put_padding(fill, used_by_padding));
    } else if (align == Align::Center) {
        auto const used_by_left_padding = used_by_padding / 2;
        auto const used_by_right_padding = ceil_div<size_t, size_t>(used_by_padding, 2);

        TRY(put_padding(fill, used_by_left_padding));
        TRY(m_builder.try_append(value));
        TRY(put_padding(fill, used_by_right_padding));
    } else if (align == Align::Right) {
        TRY(put_padding(fill, used_by_padding));
        TRY(m_builder.try_append(value));
    }
    return {};
}

ErrorOr<void> FormatBuilder::put_u64(
    u64 value,
    u8 base,
    bool prefix,
    bool upper_case,
    bool zero_pad,
    bool use_separator,
    Align align,
    size_t min_width,
    char fill,
    SignMode sign_mode,
    bool is_negative)
{
    if (align == Align::Default)
        align = Align::Right;

    Array<u8, 128> buffer;

    auto const used_by_digits = convert_unsigned_to_string(value, buffer, base, upper_case, use_separator);

    size_t used_by_prefix = 0;
    if (align == Align::Right && zero_pad) {
        // We want ByteString::formatted("{:#08x}", 32) to produce '0x00000020' instead of '0x000020'. This
        // behavior differs from both fmtlib and printf, but is more intuitive.
        used_by_prefix = 0;
    } else {
        if (is_negative || sign_mode != SignMode::OnlyIfNeeded)
            used_by_prefix += 1;

        if (prefix) {
            if (base == 8)
                used_by_prefix += 1;
            else if (base == 16)
                used_by_prefix += 2;
            else if (base == 2)
                used_by_prefix += 2;
        }
    }

    auto const used_by_field = used_by_prefix + used_by_digits;
    auto const used_by_padding = max(used_by_field, min_width) - used_by_field;

    auto const put_prefix = [&]() -> ErrorOr<void> {
        if (is_negative)
            TRY(m_builder.try_append('-'));
        else if (sign_mode == SignMode::Always)
            TRY(m_builder.try_append('+'));
        else if (sign_mode == SignMode::Reserved)
            TRY(m_builder.try_append(' '));

        if (prefix) {
            if (base == 2) {
                if (upper_case)
                    TRY(m_builder.try_append("0B"sv));
                else
                    TRY(m_builder.try_append("0b"sv));
            } else if (base == 8) {
                TRY(m_builder.try_append("0"sv));
            } else if (base == 16) {
                if (upper_case)
                    TRY(m_builder.try_append("0X"sv));
                else
                    TRY(m_builder.try_append("0x"sv));
            }
        }
        return {};
    };

    auto const put_digits = [&]() -> ErrorOr<void> {
        for (size_t i = 0; i < used_by_digits; ++i)
            TRY(m_builder.try_append(buffer[i]));
        return {};
    };

    if (align == Align::Left) {
        auto const used_by_right_padding = used_by_padding;

        TRY(put_prefix());
        TRY(put_digits());
        TRY(put_padding(fill, used_by_right_padding));
    } else if (align == Align::Center) {
        auto const used_by_left_padding = used_by_padding / 2;
        auto const used_by_right_padding = ceil_div<size_t, size_t>(used_by_padding, 2);

        TRY(put_padding(fill, used_by_left_padding));
        TRY(put_prefix());
        TRY(put_digits());
        TRY(put_padding(fill, used_by_right_padding));
    } else if (align == Align::Right) {
        auto const used_by_left_padding = used_by_padding;

        if (zero_pad) {
            TRY(put_prefix());
            TRY(put_padding('0', used_by_left_padding));
            TRY(put_digits());
        } else {
            TRY(put_padding(fill, used_by_left_padding));
            TRY(put_prefix());
            TRY(put_digits());
        }
    }
    return {};
}

ErrorOr<void> FormatBuilder::put_i64(
    i64 value,
    u8 base,
    bool prefix,
    bool upper_case,
    bool zero_pad,
    bool use_separator,
    Align align,
    size_t min_width,
    char fill,
    SignMode sign_mode)
{
    auto const is_negative = value < 0;
    u64 positive_value;
    if (value == NumericLimits<i64>::min()) {
        positive_value = static_cast<u64>(NumericLimits<i64>::max()) + 1;
    } else {
        positive_value = is_negative ? -value : value;
    }

    TRY(put_u64(positive_value, base, prefix, upper_case, zero_pad, use_separator, align, min_width, fill, sign_mode, is_negative));
    return {};
}

ErrorOr<void> FormatBuilder::put_fixed_point(
    bool is_negative,
    i64 integer_value,
    u64 fraction_value,
    u64 fraction_one,
    size_t precision,
    u8 base,
    bool upper_case,
    bool zero_pad,
    bool use_separator,
    Align align,
    size_t min_width,
    size_t fraction_max_width,
    char fill,
    SignMode sign_mode)
{
    StringBuilder string_builder;
    FormatBuilder format_builder { string_builder };

    if (is_negative)
        integer_value = -integer_value;

    TRY(format_builder.put_u64(static_cast<u64>(integer_value), base, false, upper_case, false, use_separator, Align::Right, 0, ' ', sign_mode, is_negative));

    if (fraction_max_width && (zero_pad || fraction_value)) {
        // FIXME: This is a terrible approximation but doing it properly would be a lot of work. If someone is up for that, a good
        // place to start would be the following video from CppCon 2019:
        // https://youtu.be/4P_kbF0EbZM (Stephan T. Lavavej “Floating-Point <charconv>: Making Your Code 10x Faster With C++17's Final Boss”)

        if (is_negative && fraction_value)
            fraction_value = fraction_one - fraction_value;

        TRY(string_builder.try_append('.'));

        if (base == 10) {
            u64 scale = pow<u64>(5, precision);
            // FIXME: overflows (not before: fraction_value = (2^precision - 1) and precision >= 20) (use wider integer type)
            auto fraction = scale * fraction_value;
            TRY(format_builder.put_u64(fraction, base, false, upper_case, true, use_separator, Align::Right, precision));
        } else if (base == 16 || base == 8 || base == 2) {
            auto bits_per_character = log2(base);
            auto fraction = fraction_value << ((bits_per_character - (precision % bits_per_character)) % bits_per_character);
            TRY(format_builder.put_u64(fraction, base, false, upper_case, false, use_separator, Align::Right, precision / bits_per_character + (precision % bits_per_character != 0), '0'));
        } else {
            VERIFY_NOT_REACHED();
        }
    }

    auto formatted_string = string_builder.string_view();
    if (fraction_max_width && (zero_pad || fraction_value)) {
        auto point_index = formatted_string.find('.').value_or(0);
        if (!point_index)
            VERIFY_NOT_REACHED();

        if (auto formatted_length = (formatted_string.length() - point_index - 1); formatted_length > fraction_max_width) {
            formatted_string = formatted_string.substring_view(0, 1 + point_index + fraction_max_width);
        } else {
            string_builder.append_repeated('0', fraction_max_width - formatted_length);
            formatted_string = string_builder.string_view();
        }

        if (!zero_pad)
            formatted_string = formatted_string.trim("0"sv, TrimMode::Right);

        if (formatted_string.ends_with('.'))
            formatted_string = formatted_string.trim("."sv, TrimMode::Right);
    }

    TRY(put_string(formatted_string, align, min_width, NumericLimits<size_t>::max(), fill));
    return {};
}

static ErrorOr<void> round_up_digits(StringBuilder& digits_builder)
{
    auto digits_buffer = TRY(digits_builder.to_byte_buffer());
    int current_position = digits_buffer.size() - 1;

    while (current_position >= 0) {
        if (digits_buffer[current_position] == '.') {
            --current_position;
            continue;
        }
        ++digits_buffer[current_position];
        if (digits_buffer[current_position] <= '9')
            break;
        digits_buffer[current_position] = '0';
        --current_position;
    }

    digits_builder.clear();
    if (current_position < 0)
        TRY(digits_builder.try_append('1'));
    return digits_builder.try_append(digits_buffer);
}

ErrorOr<void> FormatBuilder::put_f64_with_precision(
    double value,
    u8 base,
    bool upper_case,
    bool zero_pad,
    bool use_separator,
    Align align,
    size_t min_width,
    size_t precision,
    char fill,
    SignMode sign_mode,
    RealNumberDisplayMode display_mode)
{
    StringBuilder string_builder;
    FormatBuilder format_builder { string_builder };

    if (isnan(value) || isinf(value)) [[unlikely]] {
        if (value < 0.0)
            TRY(string_builder.try_append('-'));
        else if (sign_mode == SignMode::Always)
            TRY(string_builder.try_append('+'));
        else if (sign_mode == SignMode::Reserved)
            TRY(string_builder.try_append(' '));

        if (isnan(value))
            TRY(string_builder.try_append(upper_case ? "NAN"sv : "nan"sv));
        else
            TRY(string_builder.try_append(upper_case ? "INF"sv : "inf"sv));
        TRY(put_string(string_builder.string_view(), align, min_width, NumericLimits<size_t>::max(), fill));
        return {};
    }

    bool is_negative = value < 0.0;
    if (is_negative)
        value = -value;

    TRY(format_builder.put_u64(static_cast<u64>(value), base, false, upper_case, false, use_separator, Align::Right, 0, ' ', sign_mode, is_negative));
    value -= static_cast<i64>(value);

    bool did_emit_decimals = false;

    if (precision > 0) {
        // FIXME: This is a terrible approximation but doing it properly would be a lot of work. If someone is up for that, a good
        // place to start would be the following video from CppCon 2019:
        // https://youtu.be/4P_kbF0EbZM (Stephan T. Lavavej “Floating-Point <charconv>: Making Your Code 10x Faster With C++17's Final Boss”)
        double epsilon = 0.5;
        if (!zero_pad && display_mode != RealNumberDisplayMode::FixedPoint) {
            for (size_t i = 0; i < precision; ++i)
                epsilon /= 10.0;
        }

        for (size_t digit = 0; digit < precision; ++digit) {
            if (!zero_pad && display_mode != RealNumberDisplayMode::FixedPoint && value - static_cast<i64>(value) < epsilon)
                break;

            value *= 10.0;
            epsilon *= 10.0;

            if (value > NumericLimits<u32>::max())
                value -= static_cast<u64>(value) - (static_cast<u64>(value) % 10);

            if (digit == 0)
                TRY(string_builder.try_append('.'));

            TRY(string_builder.try_append('0' + (static_cast<u32>(value) % 10)));
            did_emit_decimals = true;
        }
    }

    // Round up if the following decimal is 5 or higher
    if (static_cast<u64>(value * 10.0) % 10 >= 5)
        TRY(round_up_digits(string_builder));

    if (did_emit_decimals && display_mode == RealNumberDisplayMode::Default) {
        while (!string_builder.is_empty()) {
            // Strip trailing zero decimals.
            if (string_builder.string_view().ends_with('0')) {
                string_builder.trim(1);
                continue;
            }
            // Strip trailing decimal point.
            if (string_builder.string_view().ends_with('.')) {
                string_builder.trim(1);
                break;
            }
            break;
        }
    }

    return put_string(string_builder.string_view(), align, min_width, NumericLimits<size_t>::max(), fill);
}

template<OneOf<f32, f64> T>
ErrorOr<void> FormatBuilder::put_f32_or_f64(
    T value,
    u8 base,
    bool upper_case,
    bool zero_pad,
    bool use_separator,
    Align align,
    size_t min_width,
    Optional<size_t> precision,
    char fill,
    SignMode sign_mode,
    RealNumberDisplayMode display_mode)
{
    if (precision.has_value() || base != 10)
        return put_f64_with_precision(value, base, upper_case, zero_pad, use_separator, align, min_width, precision.value_or(6), fill, sign_mode, display_mode);

    // No precision specified, so pick the best precision with roundtrip guarantees.
    StringBuilder builder;

    // Special cases: NaN, inf, -inf, 0 and -0.
    auto const is_nan = isnan(value);
    auto const is_inf = isinf(value);
    auto const is_zero = value == static_cast<T>(0.0) || value == static_cast<T>(-0.0);
    if (is_nan || is_inf || is_zero) {
        if (value < 0)
            TRY(builder.try_append('-'));
        else if (sign_mode == SignMode::Always)
            TRY(builder.try_append('+'));
        else if (sign_mode == SignMode::Reserved)
            TRY(builder.try_append(' '));

        if (is_nan)
            TRY(builder.try_append(upper_case ? "NAN"sv : "nan"sv));
        else if (is_inf)
            TRY(builder.try_append(upper_case ? "INF"sv : "inf"sv));
        else
            TRY(builder.try_append('0'));

        return put_string(builder.string_view(), align, min_width, NumericLimits<size_t>::max(), fill);
    }

    auto const [sign, mantissa, exponent] = convert_floating_point_to_decimal_exponential_form(value);

    auto convert_to_decimal_digits_array = [](auto x, auto& digits) -> size_t {
        size_t length = 0;
        for (; x; x /= 10)
            digits[length++] = x % 10 | '0';
        for (size_t i = 0; 2 * i + 1 < length; ++i)
            swap(digits[i], digits[length - i - 1]);
        return length;
    };

    Array<u8, 20> mantissa_digits;
    auto mantissa_length = convert_to_decimal_digits_array(mantissa, mantissa_digits);

    if (sign)
        TRY(builder.try_append('-'));
    else if (sign_mode == SignMode::Always)
        TRY(builder.try_append('+'));
    else if (sign_mode == SignMode::Reserved)
        TRY(builder.try_append(' '));

    auto const n = exponent + static_cast<i32>(mantissa_length);
    auto const mantissa_text = StringView { mantissa_digits.span().slice(0, mantissa_length) };
    size_t integral_part_end = 0;
    // NOTE: Range from ECMA262, seems like an okay default.
    if (n >= -5 && n <= 21) {
        if (exponent >= 0) {
            TRY(builder.try_append(mantissa_text));
            TRY(builder.try_append_repeated('0', exponent));
            integral_part_end = builder.length();
        } else if (n > 0) {
            TRY(builder.try_append(mantissa_text.substring_view(0, n)));
            integral_part_end = builder.length();
            TRY(builder.try_append('.'));
            TRY(builder.try_append(mantissa_text.substring_view(n)));
        } else {
            TRY(builder.try_append("0."sv));
            TRY(builder.try_append_repeated('0', -n));
            TRY(builder.try_append(mantissa_text));
            integral_part_end = 1;
        }
    } else {
        auto const exponent_sign = n < 0 ? '-' : '+';
        Array<u8, 5> exponent_digits;
        auto const exponent_length = convert_to_decimal_digits_array(abs(n - 1), exponent_digits);
        auto const exponent_text = StringView { exponent_digits.span().slice(0, exponent_length) };
        integral_part_end = 1;

        if (mantissa_length == 1) {
            // <mantissa>e<exponent>
            TRY(builder.try_append(mantissa_text));
            TRY(builder.try_append('e'));
            TRY(builder.try_append(exponent_sign));
            TRY(builder.try_append(exponent_text));
        } else {
            // <mantissa>.<mantissa[1..]>e<exponent>
            TRY(builder.try_append(mantissa_text.substring_view(0, 1)));
            TRY(builder.try_append('.'));
            TRY(builder.try_append(mantissa_text.substring_view(1)));
            TRY(builder.try_append('e'));
            TRY(builder.try_append(exponent_sign));
            TRY(builder.try_append(exponent_text));
        }
    }

    if (use_separator && integral_part_end > 3) {
        // Go backwards from the end of the integral part, inserting commas every 3 consecutive digits.
        StringBuilder separated_builder;
        auto const string_view = builder.string_view();
        for (size_t i = 0; i < integral_part_end; ++i) {
            auto const index_from_end = integral_part_end - i - 1;
            if (index_from_end > 0 && index_from_end != integral_part_end - 1 && index_from_end % 3 == 2)
                TRY(separated_builder.try_append(','));
            TRY(separated_builder.try_append(string_view[i]));
        }
        TRY(separated_builder.try_append(string_view.substring_view(integral_part_end)));
        builder = move(separated_builder);
    }

    return put_string(builder.string_view(), align, min_width, NumericLimits<size_t>::max(), fill);
}

ErrorOr<void> FormatBuilder::put_f80(
    long double value,
    u8 base,
    bool upper_case,
    bool use_separator,
    Align align,
    size_t min_width,
    size_t precision,
    char fill,
    SignMode sign_mode,
    RealNumberDisplayMode display_mode)
{
    StringBuilder string_builder;
    FormatBuilder format_builder { string_builder };

    if (isnan(value) || isinf(value)) [[unlikely]] {
        if (value < 0.0l)
            TRY(string_builder.try_append('-'));
        else if (sign_mode == SignMode::Always)
            TRY(string_builder.try_append('+'));
        else if (sign_mode == SignMode::Reserved)
            TRY(string_builder.try_append(' '));

        if (isnan(value))
            TRY(string_builder.try_append(upper_case ? "NAN"sv : "nan"sv));
        else
            TRY(string_builder.try_append(upper_case ? "INF"sv : "inf"sv));
        TRY(put_string(string_builder.string_view(), align, min_width, NumericLimits<size_t>::max(), fill));
        return {};
    }

    bool is_negative = value < 0.0l;
    if (is_negative)
        value = -value;

    TRY(format_builder.put_u64(static_cast<u64>(value), base, false, upper_case, false, use_separator, Align::Right, 0, ' ', sign_mode, is_negative));
    value -= static_cast<i64>(value);

    if (precision > 0) {
        // FIXME: This is a terrible approximation but doing it properly would be a lot of work. If someone is up for that, a good
        // place to start would be the following video from CppCon 2019:
        // https://youtu.be/4P_kbF0EbZM (Stephan T. Lavavej “Floating-Point <charconv>: Making Your Code 10x Faster With C++17's Final Boss”)
        long double epsilon = 0.5l;
        if (display_mode != RealNumberDisplayMode::FixedPoint) {
            for (size_t i = 0; i < precision; ++i)
                epsilon /= 10.0l;
        }

        for (size_t digit = 0; digit < precision; ++digit) {
            if (display_mode != RealNumberDisplayMode::FixedPoint && value - static_cast<i64>(value) < epsilon)
                break;

            value *= 10.0l;
            epsilon *= 10.0l;

            if (value > NumericLimits<u32>::max())
                value -= static_cast<u64>(value) - (static_cast<u64>(value) % 10);

            if (digit == 0)
                TRY(string_builder.try_append('.'));

            TRY(string_builder.try_append('0' + (static_cast<u32>(value) % 10)));
        }
    }

    // Round up if the following decimal is 5 or higher
    if (static_cast<u64>(value * 10.0l) % 10 >= 5)
        TRY(round_up_digits(string_builder));

    TRY(put_string(string_builder.string_view(), align, min_width, NumericLimits<size_t>::max(), fill));
    return {};
}

ErrorOr<void> FormatBuilder::put_hexdump(ReadonlyBytes bytes, size_t width, char fill)
{
    auto put_char_view = [&](auto i) -> ErrorOr<void> {
        TRY(put_padding(fill, 4));
        for (size_t j = i - min(i, width); j < i; ++j) {
            auto ch = bytes[j];
            TRY(m_builder.try_append(ch >= 32 && ch <= 127 ? ch : '.')); // silly hack
        }
        return {};
    };

    for (size_t i = 0; i < bytes.size(); ++i) {
        if (width > 0) {
            if (i % width == 0 && i) {
                TRY(put_char_view(i));
                TRY(put_literal("\n"sv));
            }
        }
        TRY(put_u64(bytes[i], 16, false, false, true, false, Align::Right, 2));
    }

    if (width > 0)
        TRY(put_char_view(bytes.size()));

    return {};
}

ErrorOr<void> vformat(StringBuilder& builder, StringView fmtstr, TypeErasedFormatParams& params)
{
    FormatBuilder fmtbuilder { builder };
    FormatParser parser { fmtstr };

    TRY(vformat_impl(params, fmtbuilder, parser));
    return {};
}

void StandardFormatter::parse(TypeErasedFormatParams& params, FormatParser& parser)
{
    if ("<^>"sv.contains(parser.peek(1))) {
        VERIFY(!parser.next_is(is_any_of("{}"sv)));
        m_fill = parser.consume();
    }

    if (parser.consume_specific('<'))
        m_align = FormatBuilder::Align::Left;
    else if (parser.consume_specific('^'))
        m_align = FormatBuilder::Align::Center;
    else if (parser.consume_specific('>'))
        m_align = FormatBuilder::Align::Right;

    if (parser.consume_specific('-'))
        m_sign_mode = FormatBuilder::SignMode::OnlyIfNeeded;
    else if (parser.consume_specific('+'))
        m_sign_mode = FormatBuilder::SignMode::Always;
    else if (parser.consume_specific(' '))
        m_sign_mode = FormatBuilder::SignMode::Reserved;

    if (parser.consume_specific('#'))
        m_alternative_form = true;

    if (parser.consume_specific('\''))
        m_use_separator = true;

    if (parser.consume_specific('0'))
        m_zero_pad = true;

    if (size_t index = 0; parser.consume_replacement_field(index)) {
        if (index == use_next_index)
            index = params.take_next_index();

        m_width = params.parameters().at(index).to_size();
    } else if (size_t width = 0; parser.consume_number(width)) {
        m_width = width;
    }

    if (parser.consume_specific('.')) {
        if (size_t index = 0; parser.consume_replacement_field(index)) {
            if (index == use_next_index)
                index = params.take_next_index();

            m_precision = params.parameters().at(index).to_size();
        } else if (size_t precision = 0; parser.consume_number(precision)) {
            m_precision = precision;
        }
    }

    if (parser.consume_specific('b'))
        m_mode = Mode::Binary;
    else if (parser.consume_specific('B'))
        m_mode = Mode::BinaryUppercase;
    else if (parser.consume_specific('d'))
        m_mode = Mode::Decimal;
    else if (parser.consume_specific('o'))
        m_mode = Mode::Octal;
    else if (parser.consume_specific('x'))
        m_mode = Mode::Hexadecimal;
    else if (parser.consume_specific('X'))
        m_mode = Mode::HexadecimalUppercase;
    else if (parser.consume_specific('c'))
        m_mode = Mode::Character;
    else if (parser.consume_specific('s'))
        m_mode = Mode::String;
    else if (parser.consume_specific('p'))
        m_mode = Mode::Pointer;
    else if (parser.consume_specific('f'))
        m_mode = Mode::FixedPoint;
    else if (parser.consume_specific('a'))
        m_mode = Mode::Hexfloat;
    else if (parser.consume_specific('A'))
        m_mode = Mode::HexfloatUppercase;
    else if (parser.consume_specific("hex-dump"sv))
        m_mode = Mode::HexDump;

    if (!parser.is_eof())
        dbgln("{} did not consume '{}'", __PRETTY_FUNCTION__, parser.remaining());

    VERIFY(parser.is_eof());
}

ErrorOr<void> Formatter<StringView>::format(FormatBuilder& builder, StringView value)
{
    if (m_sign_mode != FormatBuilder::SignMode::Default)
        VERIFY_NOT_REACHED();
    if (m_zero_pad)
        VERIFY_NOT_REACHED();
    if (m_mode != Mode::Default && m_mode != Mode::String && m_mode != Mode::Character && m_mode != Mode::HexDump)
        VERIFY_NOT_REACHED();

    m_width = m_width.value_or(0);
    m_precision = m_precision.value_or(NumericLimits<size_t>::max());

    if (m_mode == Mode::HexDump)
        return builder.put_hexdump(value.bytes(), m_width.value(), m_fill);
    return builder.put_string(value, m_align, m_width.value(), m_precision.value(), m_fill);
}

ErrorOr<void> Formatter<FormatString>::vformat(FormatBuilder& builder, StringView fmtstr, TypeErasedFormatParams& params)
{
    StringBuilder string_builder;
    TRY(AK::vformat(string_builder, fmtstr, params));
    TRY(Formatter<StringView>::format(builder, string_builder.string_view()));
    return {};
}

template<Integral T>
ErrorOr<void> Formatter<T>::format(FormatBuilder& builder, T value)
{
    if (m_mode == Mode::Character) {
        // FIXME: We just support ASCII for now, in the future maybe unicode?
        //        VERIFY(value >= 0 && value <= 127);

        m_mode = Mode::String;

        Formatter<StringView> formatter { *this };

        // convert value to single byte, important for big-endian because the LSB is the last byte.
        VERIFY(value >= 0 && value <= 127);
        char const c = (value & 0x7f);

        return formatter.format(builder, StringView { &c, 1 });
    }

    if (m_precision.has_value())
        VERIFY_NOT_REACHED();

    if (m_mode == Mode::Pointer) {
        if (m_sign_mode != FormatBuilder::SignMode::Default)
            VERIFY_NOT_REACHED();
        if (m_align != FormatBuilder::Align::Default)
            VERIFY_NOT_REACHED();
        if (m_alternative_form)
            VERIFY_NOT_REACHED();
        if (m_width.has_value())
            VERIFY_NOT_REACHED();

        m_mode = Mode::Hexadecimal;
        m_alternative_form = true;
        m_width = 2 * sizeof(void*);
        m_zero_pad = true;
    }

    u8 base = 0;
    bool upper_case = false;
    if (m_mode == Mode::Binary) {
        base = 2;
    } else if (m_mode == Mode::BinaryUppercase) {
        base = 2;
        upper_case = true;
    } else if (m_mode == Mode::Octal) {
        base = 8;
    } else if (m_mode == Mode::Decimal || m_mode == Mode::Default) {
        base = 10;
    } else if (m_mode == Mode::Hexadecimal) {
        base = 16;
    } else if (m_mode == Mode::HexadecimalUppercase) {
        base = 16;
        upper_case = true;
    } else if (m_mode == Mode::HexDump) {
        m_width = m_width.value_or(32);
        return builder.put_hexdump({ &value, sizeof(value) }, m_width.value(), m_fill);
    } else {
        VERIFY_NOT_REACHED();
    }

    m_width = m_width.value_or(0);

    if constexpr (IsSame<MakeUnsigned<T>, T>)
        return builder.put_u64(value, base, m_alternative_form, upper_case, m_zero_pad, m_use_separator, m_align, m_width.value(), m_fill, m_sign_mode);
    else
        return builder.put_i64(value, base, m_alternative_form, upper_case, m_zero_pad, m_use_separator, m_align, m_width.value(), m_fill, m_sign_mode);
}

ErrorOr<void> Formatter<char>::format(FormatBuilder& builder, char value)
{
    if (m_mode == Mode::Binary || m_mode == Mode::BinaryUppercase || m_mode == Mode::Decimal || m_mode == Mode::Octal || m_mode == Mode::Hexadecimal || m_mode == Mode::HexadecimalUppercase) {
        // Trick: signed char != char. (Sometimes weird features are actually helpful.)
        Formatter<signed char> formatter { *this };
        return formatter.format(builder, static_cast<signed char>(value));
    } else {
        Formatter<StringView> formatter { *this };
        return formatter.format(builder, { &value, 1 });
    }
}
ErrorOr<void> Formatter<char32_t>::format(FormatBuilder& builder, char32_t value)
{
    if (m_mode == Mode::Binary || m_mode == Mode::BinaryUppercase || m_mode == Mode::Decimal || m_mode == Mode::Octal || m_mode == Mode::Hexadecimal || m_mode == Mode::HexadecimalUppercase) {
        Formatter<u32> formatter { *this };
        return formatter.format(builder, value);
    } else {
        StringBuilder codepoint;
        codepoint.append_code_point(value);

        Formatter<StringView> formatter { *this };
        return formatter.format(builder, codepoint.string_view());
    }
}
ErrorOr<void> Formatter<bool>::format(FormatBuilder& builder, bool value)
{
    if (m_mode == Mode::Binary || m_mode == Mode::BinaryUppercase || m_mode == Mode::Decimal || m_mode == Mode::Octal || m_mode == Mode::Hexadecimal || m_mode == Mode::HexadecimalUppercase) {
        Formatter<u8> formatter { *this };
        return formatter.format(builder, static_cast<u8>(value));
    } else if (m_mode == Mode::HexDump) {
        return builder.put_hexdump({ &value, sizeof(value) }, m_width.value_or(32), m_fill);
    } else {
        Formatter<StringView> formatter { *this };
        return formatter.format(builder, value ? "true"sv : "false"sv);
    }
}
ErrorOr<void> Formatter<long double>::format(FormatBuilder& builder, long double value)
{
    u8 base;
    bool upper_case;
    FormatBuilder::RealNumberDisplayMode real_number_display_mode = FormatBuilder::RealNumberDisplayMode::General;
    if (m_mode == Mode::Default || m_mode == Mode::FixedPoint) {
        base = 10;
        upper_case = false;
        if (m_mode == Mode::FixedPoint)
            real_number_display_mode = FormatBuilder::RealNumberDisplayMode::FixedPoint;
    } else if (m_mode == Mode::Hexfloat) {
        base = 16;
        upper_case = false;
    } else if (m_mode == Mode::HexfloatUppercase) {
        base = 16;
        upper_case = true;
    } else {
        VERIFY_NOT_REACHED();
    }

    m_width = m_width.value_or(0);
    m_precision = m_precision.value_or(6);

    return builder.put_f80(value, base, upper_case, m_use_separator, m_align, m_width.value(), m_precision.value(), m_fill, m_sign_mode, real_number_display_mode);
}

ErrorOr<void> Formatter<f16>::format(FormatBuilder& builder, f16 value)
{
    // FIXME: Create a proper put_f16() implementation
    Formatter<double> formatter { *this };
    return TRY(formatter.format(builder, static_cast<double>(value)));
}

ErrorOr<void> Formatter<double>::format(FormatBuilder& builder, double value)
{
    u8 base;
    bool upper_case;
    FormatBuilder::RealNumberDisplayMode real_number_display_mode = FormatBuilder::RealNumberDisplayMode::General;
    if (m_mode == Mode::Default || m_mode == Mode::FixedPoint) {
        base = 10;
        upper_case = false;
        if (m_mode == Mode::FixedPoint)
            real_number_display_mode = FormatBuilder::RealNumberDisplayMode::FixedPoint;
    } else if (m_mode == Mode::Hexfloat) {
        base = 16;
        upper_case = false;
    } else if (m_mode == Mode::HexfloatUppercase) {
        base = 16;
        upper_case = true;
    } else {
        VERIFY_NOT_REACHED();
    }

    m_width = m_width.value_or(0);

    return builder.put_f32_or_f64(value, base, upper_case, m_zero_pad, m_use_separator, m_align, m_width.value(), m_precision, m_fill, m_sign_mode, real_number_display_mode);
}

ErrorOr<void> Formatter<float>::format(FormatBuilder& builder, float value)
{
    u8 base;
    bool upper_case;
    FormatBuilder::RealNumberDisplayMode real_number_display_mode = FormatBuilder::RealNumberDisplayMode::General;
    if (m_mode == Mode::Default || m_mode == Mode::FixedPoint) {
        base = 10;
        upper_case = false;
        if (m_mode == Mode::FixedPoint)
            real_number_display_mode = FormatBuilder::RealNumberDisplayMode::FixedPoint;
    } else if (m_mode == Mode::Hexfloat) {
        base = 16;
        upper_case = false;
    } else if (m_mode == Mode::HexfloatUppercase) {
        base = 16;
        upper_case = true;
    } else {
        VERIFY_NOT_REACHED();
    }

    m_width = m_width.value_or(0);

    return builder.put_f32_or_f64(value, base, upper_case, m_zero_pad, m_use_separator, m_align, m_width.value(), m_precision, m_fill, m_sign_mode, real_number_display_mode);
}

template ErrorOr<void> FormatBuilder::put_f32_or_f64<float>(float, u8, bool, bool, bool, Align, size_t, Optional<size_t>, char, SignMode, RealNumberDisplayMode);
template ErrorOr<void> FormatBuilder::put_f32_or_f64<double>(double, u8, bool, bool, bool, Align, size_t, Optional<size_t>, char, SignMode, RealNumberDisplayMode);

void vout(FILE* file, StringView fmtstr, TypeErasedFormatParams& params, bool newline)
{
    StringBuilder builder;
    MUST(vformat(builder, fmtstr, params));

    if (newline)
        builder.append('\n');

    auto const string = builder.string_view();
    auto const retval = ::fwrite(string.characters_without_null_termination(), 1, string.length(), file);
    if (static_cast<size_t>(retval) != string.length()) {
        auto error = ferror(file);
        dbgln("vout() failed ({} written out of {}), error was {} ({})", retval, string.length(), error, strerror(error));
    }
}
#if defined(AK_OS_WINDOWS)
ErrorOr<void> Formatter<Error>::format_windows_error(FormatBuilder& builder, Error const& error)
{
    thread_local HashMap<u32, ByteString> windows_errors;

    int code = error.code();
    Optional<ByteString&> string = windows_errors.get(static_cast<u32>(code));
    if (string.has_value()) {
        return Formatter<StringView>::format(builder, string->view());
    }

    TCHAR* message = nullptr;
    u32 size = FormatMessage(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr,
        static_cast<DWORD>(code),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        message,
        0,
        nullptr);
    if (size == 0) {
        auto format_error = GetLastError();
        return Formatter<FormatString>::format(builder, "Error {:08x} when formatting code {:08x}"sv, format_error, code);
    }

    auto& string_in_map = windows_errors.ensure(code, [message, size] { return ByteString { message, size }; });
    LocalFree(message);
    return Formatter<StringView>::format(builder, string_in_map.view());
}
#else
ErrorOr<void> Formatter<Error>::format_windows_error(FormatBuilder&, Error const&)
{
    VERIFY_NOT_REACHED();
}
#endif

ErrorOr<void> Formatter<Error>::format(FormatBuilder& builder, Error const& error)
{
    switch (error.kind()) {
    case Error::Kind::Syscall:
        return Formatter<FormatString>::format(builder, "{}: {} (errno={})"sv, error.string_literal(), strerror(error.code()), error.code());
    case Error::Kind::Errno:
        return Formatter<FormatString>::format(builder, "{} (errno={})"sv, strerror(error.code()), error.code());
    case Error::Kind::Windows:
        return Formatter<Error>::format_windows_error(builder, error);
    case Error::Kind::StringLiteral:
        return Formatter<FormatString>::format(builder, "{}"sv, error.string_literal());
    }
    VERIFY_NOT_REACHED();
}

#ifdef AK_OS_ANDROID
static char const* s_log_tag_name = "Serenity";
void set_log_tag_name(char const* tag_name)
{
    static String s_log_tag_storage;
    // NOTE: Make sure to copy the null terminator
    s_log_tag_storage = MUST(String::from_utf8({ tag_name, strlen(tag_name) + 1 }));
    s_log_tag_name = s_log_tag_storage.bytes_as_string_view().characters_without_null_termination();
}

void vout(LogLevel log_level, StringView fmtstr, TypeErasedFormatParams& params, bool newline)
{
    StringBuilder builder;
    MUST(vformat(builder, fmtstr, params));

    if (newline)
        builder.append('\n');
    builder.append('\0');

    auto const string = builder.string_view();

    auto ndk_log_level = ANDROID_LOG_UNKNOWN;
    switch (log_level) {
    case LogLevel ::Debug:
        ndk_log_level = ANDROID_LOG_DEBUG;
        break;
    case LogLevel ::Info:
        ndk_log_level = ANDROID_LOG_INFO;
        break;
    case LogLevel::Warning:
        ndk_log_level = ANDROID_LOG_WARN;
        break;
    }

    __android_log_write(ndk_log_level, s_log_tag_name, string.characters_without_null_termination());
}

#endif

// FIXME: Deduplicate with Core::Process:get_name()
[[gnu::used]] static ByteString process_name_helper()
{
#if defined(AK_OS_SERENITY)
    char buffer[BUFSIZ] = {};
    int rc = get_process_name(buffer, BUFSIZ);
    if (rc != 0)
        return ByteString {};
    return StringView { buffer, strlen(buffer) };
#elif defined(AK_LIBC_GLIBC) || (defined(AK_OS_LINUX) && !defined(AK_OS_ANDROID))
    return StringView { program_invocation_name, strlen(program_invocation_name) };
#elif defined(AK_OS_BSD_GENERIC) || defined(AK_OS_HAIKU)
    auto const* progname = getprogname();
    return StringView { progname, strlen(progname) };
#elif defined AK_OS_WINDOWS
    char path[MAX_PATH] = {};
    auto length = GetModuleFileName(NULL, path, MAX_PATH);
    return { path, length };
#else
    // FIXME: Implement process_name_helper() for other platforms.
    return StringView {};
#endif
}

static StringView process_name_for_logging()
{
    // NOTE: We use AK::Format in the DynamicLoader and LibC, which cannot use thread-safe statics
    // Also go to extraordinary lengths here to avoid strlen() on the process name every call to dbgln
    static char process_name_buf[256] = {};
    static StringView process_name;
    static bool process_name_retrieved = false;
    if (!process_name_retrieved) {
        auto path = LexicalPath(process_name_helper());
        process_name_retrieved = true;
        (void)path.title().copy_characters_to_buffer(process_name_buf, sizeof(process_name_buf));
        process_name = { process_name_buf, strlen(process_name_buf) };
    }
    return process_name;
}

static bool is_debug_enabled = true;

void set_debug_enabled(bool value)
{
    is_debug_enabled = value;
}

// On Serenity, dbgln goes to a non-stderr output
static bool is_rich_debug_enabled =
#if defined(AK_OS_SERENITY)
    true;
#else
    false;
#endif

void set_rich_debug_enabled(bool value)
{
    is_rich_debug_enabled = value;
}

#ifdef AK_OS_WINDOWS

#    define YELLOW(str) "\33[93m" str "\33[0m"

static int main_thread_id = GetCurrentThreadId();

static int enable_escape_sequences()
{
    HANDLE console_handle = CreateFile("CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (console_handle == INVALID_HANDLE_VALUE) {
        dbgln("Unable to get console handle");
        return 0;
    }

    ScopeGuard guard = [&] { CloseHandle(console_handle); };

    DWORD mode = 0;
    if (!GetConsoleMode(console_handle, &mode)) {
        dbgln("Unable to get console mode");
        return 0;
    }

    mode |= ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(console_handle, mode)) {
        dbgln("Unable to set console mode");
        return 0;
    }

    return 0;
}

static int dummy = enable_escape_sequences();

#endif

void vdbg(StringView fmtstr, TypeErasedFormatParams& params, bool newline)
{
    if (!is_debug_enabled)
        return;

    StringBuilder builder;

    if (is_rich_debug_enabled) {
#ifndef AK_OS_WINDOWS
        auto process_name = process_name_for_logging();
        if (!process_name.is_empty()) {
            struct timespec ts = {};
            clock_gettime(CLOCK_MONOTONIC_COARSE, &ts);
            auto pid = getpid();
#    if defined(AK_OS_SERENITY) || defined(AK_OS_LINUX)
            // Linux and Serenity handle thread IDs as if they are related to process ids
            auto tid = gettid();
            if (pid == tid)
#    endif
            {
                builder.appendff("{}.{:03} \033[33;1m{}({})\033[0m: ", ts.tv_sec, ts.tv_nsec / 1000000, process_name, pid);
            }
#    if defined(AK_OS_SERENITY) || defined(AK_OS_LINUX)
            else {
                builder.appendff("{}.{:03} \033[33;1m{}({}:{})\033[0m: ", ts.tv_sec, ts.tv_nsec / 1000000, process_name, pid, tid);
            }
#    endif
        }
#else
        auto process_name = process_name_for_logging();
        if (!process_name.is_empty()) {
            int tid = GetCurrentThreadId();
            if (tid == main_thread_id)
                builder.appendff(YELLOW("{}: "), process_name);
            else
                builder.appendff(YELLOW("{}:{}: "), process_name, tid);
        }
#endif
    }

    MUST(vformat(builder, fmtstr, params));
    if (newline)
        builder.append('\n');
#ifdef AK_OS_ANDROID
    builder.append('\0');
#endif
    auto const string = builder.string_view();

#ifdef AK_OS_ANDROID
    __android_log_write(ANDROID_LOG_DEBUG, s_log_tag_name, string.characters_without_null_termination());
#elif defined(AK_OS_WINDOWS)
    [[maybe_unused]] auto rc = _write(_fileno(stderr), string.characters_without_null_termination(), string.length());
#else
    [[maybe_unused]] auto rc = write(STDERR_FILENO, string.characters_without_null_termination(), string.length());
#endif
}

template struct Formatter<unsigned char, void>;
template struct Formatter<unsigned short, void>;
template struct Formatter<unsigned int, void>;
template struct Formatter<unsigned long, void>;
template struct Formatter<unsigned long long, void>;
template struct Formatter<short, void>;
template struct Formatter<int, void>;
template struct Formatter<long, void>;
template struct Formatter<long long, void>;
template struct Formatter<signed char, void>;

} // namespace AK
