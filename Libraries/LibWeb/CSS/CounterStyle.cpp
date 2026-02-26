/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CounterStyle.h"
#include <AK/HashTable.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-counter-styles-3/#decimal
CounterStyle CounterStyle::decimal()
{
    return CounterStyle::create(
        "decimal"_fly_string,
        GenericCounterStyleAlgorithm { CounterStyleSystem::Numeric, { "0"_fly_string, "1"_fly_string, "2"_fly_string, "3"_fly_string, "4"_fly_string, "5"_fly_string, "6"_fly_string, "7"_fly_string, "8"_fly_string, "9"_fly_string } },
        CounterStyleNegativeSign { .prefix = "-"_fly_string, .suffix = ""_fly_string },
        ""_fly_string,
        ". "_fly_string,
        { { NumericLimits<i64>::min(), NumericLimits<i64>::max() } },
        {},
        CounterStylePad { .minimum_length = 0, .symbol = ""_fly_string });
}

// https://drafts.csswg.org/css-counter-styles-3/#disc
CounterStyle CounterStyle::disc()
{
    return CounterStyle::create(
        "disc"_fly_string,
        GenericCounterStyleAlgorithm { CounterStyleSystem::Cyclic, { "•"_fly_string } },
        CounterStyleNegativeSign { .prefix = ""_fly_string, .suffix = " "_fly_string },
        ""_fly_string,
        " "_fly_string,
        { { NumericLimits<i64>::min(), NumericLimits<i64>::max() } },
        "decimal"_fly_string,
        CounterStylePad { .minimum_length = 0, .symbol = ""_fly_string });
}

CounterStyle CounterStyle::from_counter_style_definition(CounterStyleDefinition const& definition, HashMap<FlyString, CounterStyle> const& registered_counter_styles)
{
    return definition.algorithm().visit(
        [&](CounterStyleSystemStyleValue::Extends const& extends) {
            // NB: The caller should ensure that this is always set (i.e. by ensuring the relevant rule is registered
            //     before this one, and replacing the extended counter style with "decimal" if it is not defined).
            auto extended_counter_style = registered_counter_styles.get(extends.name).value();

            return CounterStyle::create(
                definition.name(),
                extended_counter_style.algorithm(),
                definition.negative_sign().value_or(extended_counter_style.negative_sign()),
                definition.prefix().value_or(extended_counter_style.prefix()),
                definition.suffix().value_or(extended_counter_style.suffix()),
                definition.range().visit(
                    [&](Empty const&) { return extended_counter_style.range(); },
                    [](Vector<CounterStyleRangeEntry> const& range) { return range; },
                    [&](AutoRange const&) { return AutoRange::resolve(extended_counter_style.algorithm()); }),
                definition.fallback().value_or(extended_counter_style.fallback().value_or("decimal"_fly_string)),
                definition.pad().value_or(extended_counter_style.pad()));
        },
        [&](CounterStyleAlgorithm const& algorithm) {
            return CounterStyle::create(
                definition.name(),
                algorithm,
                definition.negative_sign().value_or({ .prefix = "-"_fly_string, .suffix = ""_fly_string }),
                definition.prefix().value_or(""_fly_string), definition.suffix().value_or(". "_fly_string),
                definition.range().visit(
                    [](Vector<CounterStyleRangeEntry> const& range) { return range; },
                    [&](auto const&) { return AutoRange::resolve(algorithm); }),
                definition.fallback().value_or("decimal"_fly_string),
                definition.pad().value_or({ .minimum_length = 0, .symbol = ""_fly_string }));
        });
}

// https://drafts.csswg.org/css-counter-styles-3/#extended-range-optional
static String generate_an_initial_representation_for_extended_cjk_system(i64 value, ExtendedCJKCounterStyleAlgorithm::Type type, Array<FlyString, 10> const& digit_strings, Array<FlyString, 3> const& digit_marker_strings, Array<FlyString, 3> const& group_marker_strings)
{
    // 1. If the counter value is 0, the representation is the character for 0 specified for the given counter style.
    //    Skip the rest of this algorithm.
    if (value == 0)
        return digit_strings[0].to_string();

    // 2. If the counter value is negative, instead use the absolute value of the counter value for the remaining steps
    //    of this algorithm.
    // NB: This is handled within `generate_a_counter_representation_impl`

    // 3. Initially represent the counter value as a decimal number. Starting from the right (ones place), split the
    //    decimal number into groups of four digits.
    Vector<u16> groups;
    while (value > 0) {
        groups.append(value % 10000);
        value /= 10000;
    }

    StringBuilder builder;

    for (i32 group_index = static_cast<i32>(groups.size()) - 1; group_index >= 0; --group_index) {
        auto const group_value = groups[group_index];

        Vector<u8> digits;

        auto temp_group_value = group_value;
        while (temp_group_value > 0) {
            digits.append(temp_group_value % 10);
            temp_group_value /= 10;
        }

        // NB: Pad the group with zeroes up to four digits, unless this is the most significant group.
        if (group_index != static_cast<i32>(groups.size()) - 1) {
            while (digits.size() < 4)
                digits.append(0);
        }

        // NB: We move around the order of spec steps to work with a string builder rather than replacing characters in
        //     a string.
        for (i32 digit_index = static_cast<i32>(digits.size()) - 1; digit_index >= 0; --digit_index) {
            auto const digit_value = digits[digit_index];

            bool should_drop_digit = false;
            // 6. Drop ones:
            //  - For the Chinese informal styles, for any group with a value between ten and nineteen, remove the tens
            //    digit (leave the digit marker).
            if (first_is_one_of(type, ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseInformal, ExtendedCJKCounterStyleAlgorithm::Type::TradChineseInformal))
                should_drop_digit |= group_value >= 10 && group_value < 20 && digit_index == 1;

            //  - For the Japanese informal and Korean informal styles, if any of the digit markers are preceded
            //   by the digit 1, and that digit is not the first digit of the group, remove the digit (leave the
            //    digit marker).
            // FIXME: Support Korean
            if (first_is_one_of(type, ExtendedCJKCounterStyleAlgorithm::Type::JapaneseInformal))
                should_drop_digit |= digit_value == 1 && digit_index != 0;

            // FIXME: - For Korean informal styles, if the value of the ten-thousands group is 1, drop the digit (leave
            //          the digit marker).

            // 7. Drop zeros:
            //  - For the Japanese and Korean styles, drop all zero digits.
            // FIXME: Support Korean styles
            if (first_is_one_of(type, ExtendedCJKCounterStyleAlgorithm::Type::JapaneseInformal, ExtendedCJKCounterStyleAlgorithm::Type::JapaneseFormal))
                should_drop_digit |= digit_value == 0;

            //  - For the Chinese styles, drop any trailing zeros for all non-zero groups and collapse (across groups)
            //    each remaining consecutive group of zeros into a single zero digit.
            if (first_is_one_of(type, ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseInformal, ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseFormal, ExtendedCJKCounterStyleAlgorithm::Type::TradChineseInformal, ExtendedCJKCounterStyleAlgorithm::Type::TradChineseFormal)) {
                bool is_trailing_zero = true;
                for (i32 trailing_zero_index = digit_index; trailing_zero_index >= 0; --trailing_zero_index) {
                    if (digits[trailing_zero_index] != 0) {
                        is_trailing_zero = false;
                        break;
                    }
                }

                should_drop_digit |= is_trailing_zero;

                // NB: We don't need to worry about collapsing across groups since dropping trailing zeroes above means
                //     that a run of zeroes can't occur at the end of a group.
                should_drop_digit |= digit_value == 0 && digit_index != 3 && digits[digit_index + 1] == 0;
            }

            // 9. Replace the digits 0-9 with the appropriate character for the given counter style.
            if (!should_drop_digit)
                builder.append(digit_strings[digit_value]);

            // 5. Within each group, for each digit that is not 0, append the appropriate digit marker to the digit.
            //    The ones digit of each group has no marker.
            if (digit_value != 0 && digit_index != 0)
                builder.append(digit_marker_strings[digit_index - 1]);
        }

        // 4. For each group with a non-zero value, append the appropriate group marker to the group. The ones group has no marker.
        if (groups[group_index] != 0 && group_index != 0)
            builder.append(group_marker_strings[group_index - 1]);

        // FIXME: 8. For the Korean styles, insert a space (" " U+0020) between each group.
    }

    // 10. If the counter value was negative, prepend the appropriate negative sign character for the given counter
    //     style as specified in the table of characters for each style.
    // NB: This is handled within `generate_a_counter_representation_impl`

    // 11. Return the resultant string as the representation of the counter value.
    return MUST(builder.to_string());
}

Optional<String> CounterStyle::generate_an_initial_representation_for_the_counter_value(i64 value) const
{
    return m_algorithm.visit(
        [&](AdditiveCounterStyleAlgorithm const& additive_algorithm) -> Optional<String> {
            // https://drafts.csswg.org/css-counter-styles-3/#additive-system
            // To construct the representation:

            // 1. Let value initially be the counter value, S initially be the empty string, and symbol list initially
            //    be the list of additive tuples.

            // 2. If value is zero:
            if (value == 0) {
                // 1. If symbol list contains a tuple with a weight of zero, append that tuple’s counter symbol to S and
                //    return S.
                if (auto it = additive_algorithm.symbol_list.find_if([](auto const& tuple) { return tuple.weight == 0; }); it != additive_algorithm.symbol_list.end())
                    return it->symbol.to_string();

                // 2. Otherwise, the given counter value cannot be represented by this counter style, and must instead
                //    be represented by the fallback counter style.
                return {};
            }

            StringBuilder builder;

            // 3. For each tuple in symbol list:
            for (auto const& tuple : additive_algorithm.symbol_list) {
                // 1. Let symbol and weight be tuple’s counter symbol and weight, respectively.

                // 2. If weight is zero, or weight is greater than value, continue.
                if (tuple.weight == 0 || tuple.weight > value)
                    continue;

                // 3. Let reps be floor( value / weight ).
                auto reps = value / tuple.weight;

                // 4. Append symbol to S reps times.
                for (int i = 0; i < reps; ++i)
                    builder.append(tuple.symbol);

                // 5. Decrement value by weight * reps.
                value -= tuple.weight * reps;

                // 6. If value is zero, return S.
                if (value == 0)
                    return MUST(builder.to_string());
            }

            // Assertion: value is still non-zero.
            VERIFY(value != 0);

            // The given counter value cannot be represented by this counter style, and must instead be represented by
            // the fallback counter style.
            return {};
        },
        [&](FixedCounterStyleAlgorithm const& fixed_algorithm) -> Optional<String> {
            // https://drafts.csswg.org/css-counter-styles-3/#fixed-system
            // The first counter symbol is the representation for the first symbol value, and subsequent counter values
            // are represented by subsequent counter symbols. Once the list of counter symbols is exhausted, further
            // values cannot be represented by this counter style, and must instead be represented by the fallback
            // counter style.
            auto index = value - fixed_algorithm.first_symbol;

            if (index < 0 || index >= static_cast<i64>(fixed_algorithm.symbol_list.size()))
                return {};

            return fixed_algorithm.symbol_list[index].to_string();
        },
        [&](GenericCounterStyleAlgorithm const& generic_algorithm) -> Optional<String> {
            switch (generic_algorithm.type) {
            case CounterStyleSystem::Cyclic: {
                // https://drafts.csswg.org/css-counter-styles-3/#cyclic-system
                // If there are N counter symbols and a representation is being constructed for the integer value, the
                // representation is the counter symbol at index ( (value-1) mod N) of the list of counter symbols
                // (0-indexed).
                return generic_algorithm.symbol_list[(value - 1) % generic_algorithm.symbol_list.size()].to_string();
            }
            case CounterStyleSystem::Numeric: {
                // https://drafts.csswg.org/css-counter-styles-3/#numeric-system
                // If there are N counter symbols, the representation is a base N number using the counter symbols as
                // digits. To construct the representation, run the following algorithm:

                // Let N be the length of the list of counter symbols, value initially be the counter value, S
                // initially be the empty string, and symbol(n) be the nth counter symbol in the list of counter
                // symbols (0-indexed).

                // 1. If value is 0, append symbol(0) to S and return S.
                if (value == 0)
                    return generic_algorithm.symbol_list[0].to_string();

                // NB: Our string builder doesn't support prepending, so we use a vector and convert that to a string at
                //     the end.
                Vector<CounterStyleSymbol> symbols;

                // 2. While value is not equal to 0:
                while (value != 0) {
                    // 1. Prepend symbol( value mod N ) to S.
                    symbols.prepend(generic_algorithm.symbol_list[value % generic_algorithm.symbol_list.size()]);

                    // 2. Set value to floor( value / N ).
                    value /= generic_algorithm.symbol_list.size();
                }

                // 3. Return S.
                return MUST(String::join(""sv, symbols));
            }
            case CounterStyleSystem::Alphabetic: {
                // https://drafts.csswg.org/css-counter-styles-3/#alphabetic-system
                // If there are N counter symbols, the representation is a base N alphabetic number using the counter
                // symbols as digits. To construct the representation, run the following algorithm:

                // Let N be the length of the list of counter symbols, value initially be the counter value, S initially
                // be the empty string, and symbol(n) be the nth counter symbol in the list of counter symbols
                // (0-indexed).

                // NB: Our string builder doesn't support prepending, so we use a vector and convert that to a string at
                //     the end.
                Vector<CounterStyleSymbol> symbols;

                // While value is not equal to 0:
                while (value != 0) {
                    // 1. Set value to value - 1.
                    value -= 1;

                    // 2. Prepend symbol( value mod N ) to S.
                    symbols.prepend(generic_algorithm.symbol_list[value % generic_algorithm.symbol_list.size()]);

                    // 3. Set value to floor( value / N ).
                    value /= generic_algorithm.symbol_list.size();
                }

                // Finally, return S.
                return MUST(String::join(""sv, symbols));
            }
            case CounterStyleSystem::Symbolic: {
                // https://drafts.csswg.org/css-counter-styles-3/#symbolic-system
                // To construct the representation, run the following algorithm:

                // Let N be the length of the list of counter symbols, value initially be the counter value, S initially
                // be the empty string, and symbol(n) be the nth counter symbol in the list of counter symbols
                // (0-indexed).

                // 1. Let the chosen symbol be symbol( (value - 1) mod N).
                auto const& symbol = generic_algorithm.symbol_list[(value - 1) % generic_algorithm.symbol_list.size()];

                // 2. Let the representation length be ceil( value / N ).
                auto representation_length = (value + generic_algorithm.symbol_list.size() - 1) / generic_algorithm.symbol_list.size();

                // 3. Append the chosen symbol to S a number of times equal to the representation length.
                // Finally, return S.
                return MUST(String::repeated(symbol.to_string(), representation_length));
            }
            case CounterStyleSystem::Additive:
                // NB: This is handled by AdditiveCounterStyleAlgorithm.
                VERIFY_NOT_REACHED();
            }

            VERIFY_NOT_REACHED();
        },
        [&](EthiopicNumericCounterStyleAlgorithm const&) -> Optional<String> {
            // https://drafts.csswg.org/css-counter-styles-3/#ethiopic-numeric-counter-style
            // The following algorithm converts decimal digits to ethiopic numbers:

            // 1. If the number is 1, return "፩" (U+1369).
            if (value == 1)
                return "\U00001369"_string;

            // 2. Split the number into groups of two digits, starting with the least significant decimal digit.
            Vector<u8> groups {};
            while (value != 0) {
                groups.append(value % 100);
                value /= 100;
            }

            StringBuilder builder;

            // 3. Index each group sequentially, starting from the least significant as group number zero.

            // NB: We iterate in descending order of significance, so we can append to the string builder in order
            for (i32 i = groups.size() - 1; i >= 0; --i) {
                auto group_value = groups[i];

                // 4. If the group has the value zero, or if the group is the most significant one and has the value
                //    1, or if the group has an odd index (as given in the previous step) and has the value 1, then
                //    remove the digits (but leave the group, so it still has a separator appended below).
                if (group_value != 0 && !(i == static_cast<i32>(groups.size()) - 1 && group_value == 1) && !(i % 2 == 1 && group_value == 1)) {
                    // 5. For each remaining digit, substitute the relevant ethiopic character from the list below.
                    //      Tens
                    // Values Codepoints
                    // 10     ፲ U+1372
                    // 20     ፳ U+1373
                    // 30     ፴ U+1374
                    // 40     ፵ U+1375
                    // 50     ፶ U+1376
                    // 60     ፷ U+1377
                    // 70     ፸ U+1378
                    // 80     ፹ U+1379
                    // 90     ፺ U+137A
                    auto tens = group_value / 10;
                    if (tens != 0)
                        builder.append_code_point(0x1372 + tens - 1);

                    //     Units
                    // Values Codepoints
                    // 1      ፩ U+1369
                    // 2      ፪ U+136A
                    // 3      ፫ U+136B
                    // 4      ፬ U+136C
                    // 5      ፭ U+136D
                    // 6      ፮ U+136E
                    // 7      ፯ U+136F
                    // 8      ፰ U+1370
                    // 9      ፱ U+1371
                    auto units = group_value % 10;
                    if (units != 0)
                        builder.append_code_point(0x1369 + units - 1);
                }

                // 6. For each group with an odd index (as given in the second step), except groups which originally
                //    had a value of zero, append ፻ U+137B.
                if (i % 2 == 1 && group_value != 0)
                    builder.append_code_point(0x137B);

                // 7. For each group with an even index (as given in the second step), except the group with index
                //    0, append ፼ U+137C.
                else if (i % 2 == 0 && i != 0)
                    builder.append_code_point(0x137C);
            }

            // 8. Concatenate the groups into one string, and return it.
            return MUST(builder.to_string());
        },
        [&](ExtendedCJKCounterStyleAlgorithm const& extended_cjk_algorithm) -> Optional<String> {
            // https://drafts.csswg.org/css-counter-styles-3/#extended-range-optional
            // All of the styles are defined by almost identical algorithms (specified as a single algorithm here, with
            // the differences called out when relevant), but use different sets of characters.
            // The following tables define the characters used in these styles:

            switch (extended_cjk_algorithm.type) {
            // | Values                | Codepoints
            // |                       | simp-chinese-informal |  simp-chinese-formal |  trad-chinese-informal | trad-chinese-formal
            // | Digit 0               | 零 U+96F6             | 零 U+96F6            | 零 U+96F6              | 零 U+96F6
            // | Digit 1               | 一 U+4E00             | 壹 U+58F9            | 一 U+4E00              | 壹 U+58F9
            // | Digit 2               | 二 U+4E8C             | 贰 U+8D30            | 二 U+4E8C              | 貳 U+8CB3
            // | Digit 3               | 三 U+4E09             | 叁 U+53C1            | 三 U+4E09              | 參 U+53C3
            // | Digit 4               | 四 U+56DB             | 肆 U+8086            | 四 U+56DB              | 肆 U+8086
            // | Digit 5               | 五 U+4E94             | 伍 U+4F0D            | 五 U+4E94              | 伍 U+4F0D
            // | Digit 6               | 六 U+516D             | 陆 U+9646            | 六 U+516D              | 陸 U+9678
            // | Digit 7               | 七 U+4E03             | 柒 U+67D2            | 七 U+4E03              | 柒 U+67D2
            // | Digit 8               | 八 U+516B             | 捌 U+634C            | 八 U+516B              | 捌 U+634C
            // | Digit 9               | 九 U+4E5D             | 玖 U+7396            | 九 U+4E5D              | 玖 U+7396
            // | Second Digit Marker   | 十 U+5341             | 拾 U+62FE            | 十 U+5341              | 拾 U+62FE
            // | Third Digit Marker    | 百 U+767E             | 佰 U+4F70            | 百 U+767E              | 佰 U+4F70
            // | Fourth Digit Marker   | 千 U+5343             | 仟 U+4EDF            | 千 U+5343              | 仟 U+4EDF
            // | Second Group Marker   | 万 U+4E07             | 万 U+4E07            | 萬 U+842C              | 萬 U+842C
            // | Third Group Marker    | 亿 U+4EBF             | 亿 U+4EBF            | 億 U+5104              | 億 U+5104
            // | Fourth Group Marker   | 万亿 U+4E07 U+4EBF    | 万亿 U+4E07 U+4EBF    | 兆 U+5146             | 兆 U+5146
            case ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseInformal:
                return generate_an_initial_representation_for_extended_cjk_system(
                    value,
                    ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseInformal,
                    { "\U000096F6"_fly_string, "\U00004E00"_fly_string, "\U00004E8C"_fly_string, "\U00004E09"_fly_string, "\U000056DB"_fly_string, "\U00004E94"_fly_string, "\U0000516D"_fly_string, "\U00004E03"_fly_string, "\U0000516B"_fly_string, "\U00004E5D"_fly_string },
                    { "\U00005341"_fly_string, "\U0000767E"_fly_string, "\U00005343"_fly_string },
                    { "\U00004E07"_fly_string, "\U00004EBF"_fly_string, "\U00004E07\U00004EBF"_fly_string });
            case ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseFormal:
                return generate_an_initial_representation_for_extended_cjk_system(
                    value,
                    ExtendedCJKCounterStyleAlgorithm::Type::SimpChineseFormal,
                    { "\U000096F6"_fly_string, "\U000058F9"_fly_string, "\U00008D30"_fly_string, "\U000053C1"_fly_string, "\U00008086"_fly_string, "\U00004F0D"_fly_string, "\U00009646"_fly_string, "\U000067D2"_fly_string, "\U0000634C"_fly_string, "\U00007396"_fly_string },
                    { "\U000062FE"_fly_string, "\U00004F70"_fly_string, "\U00004EDF"_fly_string },
                    { "\U00004E07"_fly_string, "\U00004EBF"_fly_string, "\U00004E07\U00004EBF"_fly_string });
            case ExtendedCJKCounterStyleAlgorithm::Type::TradChineseInformal:
                return generate_an_initial_representation_for_extended_cjk_system(
                    value,
                    ExtendedCJKCounterStyleAlgorithm::Type::TradChineseInformal,
                    { "\U000096F6"_fly_string, "\U00004E00"_fly_string, "\U00004E8C"_fly_string, "\U00004E09"_fly_string, "\U000056DB"_fly_string, "\U00004E94"_fly_string, "\U0000516D"_fly_string, "\U00004E03"_fly_string, "\U0000516B"_fly_string, "\U00004E5D"_fly_string },
                    { "\U00005341"_fly_string, "\U0000767E"_fly_string, "\U00005343"_fly_string },
                    { "\U0000842C"_fly_string, "\U00005104"_fly_string, "\U00005146"_fly_string });
            case ExtendedCJKCounterStyleAlgorithm::Type::TradChineseFormal:
                return generate_an_initial_representation_for_extended_cjk_system(
                    value,
                    ExtendedCJKCounterStyleAlgorithm::Type::TradChineseFormal,
                    { "\U000096F6"_fly_string, "\U000058F9"_fly_string, "\U00008CB3"_fly_string, "\U000053C3"_fly_string, "\U00008086"_fly_string, "\U00004F0D"_fly_string, "\U00009678"_fly_string, "\U000067D2"_fly_string, "\U0000634C"_fly_string, "\U00007396"_fly_string },
                    { "\U000062FE"_fly_string, "\U00004F70"_fly_string, "\U00004EDF"_fly_string },
                    { "\U0000842C"_fly_string, "\U00005104"_fly_string, "\U00005146"_fly_string });

            // Values              | Codepoints
            //                     | japanese-informal | japanese-formal
            // Digit 0             | 〇 U+3007         | 零 U+96F6
            // Digit 1             | 一 U+4E00         | 壱 U+58F1
            // Digit 2             | 二 U+4E8C         | 弐 U+5F10
            // Digit 3             | 三 U+4E09         | 参 U+53C2
            // Digit 4             | 四 U+56DB         | 四 U+56DB
            // Digit 5             | 五 U+4E94         | 伍 U+4f0D
            // Digit 6             | 六 U+516D         | 六 U+516D
            // Digit 7             | 七 U+4E03         | 七 U+4E03
            // Digit 8             | 八 U+516B         | 八 U+516B
            // Digit 9             | 九 U+4E5D         | 九 U+4E5D
            // Second Digit Marker | 十 U+5341         | 拾 U+62FE
            // Third Digit Marker  | 百 U+767E         | 百 U+767E
            // Fourth Digit Marker | 千 U+5343         | 阡 U+9621
            // Second Group Marker | 万 U+4E07         | 萬 U+842C
            // Third Group Marker  | 億 U+5104         | 億 U+5104
            // Fourth Group Marker | 兆 U+5146         | 兆 U+5146
            case ExtendedCJKCounterStyleAlgorithm::Type::JapaneseInformal:
                return generate_an_initial_representation_for_extended_cjk_system(
                    value,
                    ExtendedCJKCounterStyleAlgorithm::Type::JapaneseInformal,
                    { "\U00003007"_fly_string, "\U00004E00"_fly_string, "\U00004E8C"_fly_string, "\U00004E09"_fly_string, "\U000056DB"_fly_string, "\U00004E94"_fly_string, "\U0000516D"_fly_string, "\U00004E03"_fly_string, "\U0000516B"_fly_string, "\U00004E5D"_fly_string },
                    { "\U00005341"_fly_string, "\U0000767E"_fly_string, "\U00005343"_fly_string },
                    { "\U00004E07"_fly_string, "\U00005104"_fly_string, "\U00005146"_fly_string });
            case ExtendedCJKCounterStyleAlgorithm::Type::JapaneseFormal:
                return generate_an_initial_representation_for_extended_cjk_system(
                    value,
                    ExtendedCJKCounterStyleAlgorithm::Type::JapaneseFormal,
                    { "\U000096F6"_fly_string, "\U000058F1"_fly_string, "\U00005F10"_fly_string, "\U000053C2"_fly_string, "\U000056DB"_fly_string, "\U00004F0D"_fly_string, "\U0000516D"_fly_string, "\U00004E03"_fly_string, "\U0000516B"_fly_string, "\U00004E5D"_fly_string },
                    { "\U000062FE"_fly_string, "\U0000767E"_fly_string, "\U00009621"_fly_string },
                    { "\U0000842C"_fly_string, "\U00005104"_fly_string, "\U00005146"_fly_string });
            }

            VERIFY_NOT_REACHED();
        });
}

// https://drafts.csswg.org/css-counter-styles-3/#counter-style-negative
bool CounterStyle::uses_a_negative_sign() const
{
    // Not all system values use a negative sign. In particular, a counter style uses a negative sign if its system
    // value is symbolic, alphabetic, numeric, additive, or extends if the extended counter style itself uses a negative
    // sign.
    // NB: We have resolved extends to the underlying algorithm before calling this
    return m_algorithm.visit(
        [](AdditiveCounterStyleAlgorithm const&) -> bool {
            return true;
        },
        [](FixedCounterStyleAlgorithm const&) -> bool {
            return false;
        },
        [](GenericCounterStyleAlgorithm const& generic_system) -> bool {
            switch (generic_system.type) {
            case CounterStyleSystem::Cyclic:
                return false;
            case CounterStyleSystem::Symbolic:
            case CounterStyleSystem::Alphabetic:
            case CounterStyleSystem::Numeric:
                return true;
            case CounterStyleSystem::Additive:
                // NB: This is handled by AdditiveCounterStyleAlgorithm.
                VERIFY_NOT_REACHED();
            }

            VERIFY_NOT_REACHED();
        },
        [](EthiopicNumericCounterStyleAlgorithm const&) {
            // https://drafts.csswg.org/css-counter-styles-3/#complex-predefined-counters
            // All of the counter styles defined in this section have a spoken form of numbers, and use a negative sign.
            return true;
        },
        [](ExtendedCJKCounterStyleAlgorithm const&) {
            // https://drafts.csswg.org/css-counter-styles-3/#complex-predefined-counters
            // All of the counter styles defined in this section have a spoken form of numbers, and use a negative sign.
            return true;
        });
}

// https://drafts.csswg.org/css-counter-styles-3/#generate-a-counter
static String generate_a_counter_representation_impl(Optional<CounterStyle const&> const& counter_style, HashMap<FlyString, CounterStyle> const& registered_counter_styles, i32 value, HashTable<FlyString>& fallback_history)
{
    // When asked to generate a counter representation using a particular counter style for a particular
    // counter value, follow these steps:

    // 1. If the counter style is unknown, exit this algorithm and instead generate a counter representation using the
    //    decimal style and the same counter value.
    if (!counter_style.has_value())
        return generate_a_counter_representation_impl(CounterStyle::decimal(), registered_counter_styles, value, fallback_history);

    auto const generate_a_counter_representation_using_fallback = [&]() {
        VERIFY(counter_style->name() != "decimal"_fly_string);

        auto const& fallback_name = counter_style->fallback().value();
        auto const& fallback = registered_counter_styles.get(fallback_name);

        // https://drafts.csswg.org/css-counter-styles-3/#counter-style-fallback
        // If the value of the fallback descriptor isn’t the name of any defined counter style, the used value of the
        // fallback descriptor is decimal instead. Similarly, while following fallbacks to find a counter style that
        // can render the given counter value, if a loop in the specified fallbacks is detected, the decimal style must
        // be used instead.
        if (!fallback.has_value() || fallback_history.contains(fallback_name))
            return generate_a_counter_representation_impl(CounterStyle::decimal(), registered_counter_styles, value, fallback_history);

        fallback_history.set(counter_style->name());

        return generate_a_counter_representation_impl(fallback, registered_counter_styles, value, fallback_history);
    };

    // 2. If the counter value is outside the range of the counter style, exit this algorithm and instead generate a
    //    counter representation using the counter style’s fallback style and the same counter value.
    if (!any_of(counter_style->range(), [&](auto const& entry) { return value >= entry.start && value <= entry.end; }))
        return generate_a_counter_representation_using_fallback();

    auto value_is_negative_and_uses_negative_sign = value < 0 && counter_style->uses_a_negative_sign();

    // 3. Using the counter value and the counter algorithm for the counter style, generate an initial representation
    //    for the counter value. If the counter value is negative and the counter style uses a negative sign, instead
    //    generate an initial representation using the absolute value of the counter value.
    auto maybe_representation = counter_style->generate_an_initial_representation_for_the_counter_value(value_is_negative_and_uses_negative_sign ? abs(static_cast<i64>(value)) : static_cast<i64>(value));

    // AD-HOC: Algorithms are sometimes unable to produce a representation and require us to use the fallback - we
    //         represent this by returning an empty Optional.
    if (!maybe_representation.has_value())
        return generate_a_counter_representation_using_fallback();

    auto representation = maybe_representation.value();

    // 4. Prepend symbols to the representation as specified in the pad descriptor.
    {
        // https://drafts.csswg.org/css-counter-styles-3/#counter-style-pad
        // Let difference be the provided <integer> minus the number of grapheme clusters in the initial representation
        // for the counter value.
        // FIXME: We should be counting grapheme clusters here.
        auto difference = counter_style->pad().minimum_length - static_cast<i32>(representation.length_in_code_units());

        // If the counter value is negative and the counter style uses a negative sign, further reduce difference by
        // the number of grapheme clusters in the counter style’s negative descriptor’s <symbol>(s).
        // FIXME: We should be counting grapheme clusters here.
        if (value_is_negative_and_uses_negative_sign)
            difference -= counter_style->negative_sign().prefix.to_string().length_in_code_units() + counter_style->negative_sign().suffix.to_string().length_in_code_units();

        // If difference is greater than zero, prepend difference copies of the specified <symbol> to the representation.
        if (difference > 0)
            representation = MUST(String::formatted("{}{}", MUST(String::repeated(counter_style->pad().symbol.to_string(), difference)), representation));
    }

    // 5. If the counter value is negative and the counter style uses a negative sign, wrap the representation in the
    //    counter style’s negative sign as specified in the negative descriptor.
    if (value_is_negative_and_uses_negative_sign)
        representation = MUST(String::formatted("{}{}{}", counter_style->negative_sign().prefix, representation, counter_style->negative_sign().suffix));

    // 6. Return the representation.
    return representation;
}

String generate_a_counter_representation(Optional<CounterStyle const&> const& counter_style, HashMap<FlyString, CounterStyle> const& registered_counter_styles, i32 value)
{
    HashTable<FlyString> fallback_history;
    return generate_a_counter_representation_impl(counter_style, registered_counter_styles, value, fallback_history);
}

}
