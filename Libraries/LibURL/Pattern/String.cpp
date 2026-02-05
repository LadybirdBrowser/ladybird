/*
 * Copyright (c) 2025, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibURL/Pattern/String.h>
#include <LibURL/Pattern/Tokenizer.h>

namespace URL::Pattern {

// https://urlpattern.spec.whatwg.org/#escape-a-pattern-string
String escape_a_pattern_string(String const& input)
{
    // 1. Assert: input is an ASCII string.
    VERIFY(input.is_ascii());

    // 2. Let result be the empty string.
    StringBuilder result;

    // 3. Let index be 0.
    // 4. While index is less than input’s length:
    for (auto c : input.bytes_as_string_view()) {
        // 1. Let c be input[index].
        // 2. Increment index by 1.

        // 3. If c is one of:
        //     * U+002B (+);
        //     * U+002A (*);
        //     * U+003F (?);
        //     * U+003A (:);
        //     * U+007B ({);
        //     * U+007D (});
        //     * U+0028 (();
        //     * U+0029 ()); or
        //     * U+005C (\),
        //    then append U+005C (\) to the end of result.
        if ("+*?:{}()\\"sv.contains(c))
            result.append('\\');

        // 4. Append c to the end of result.
        result.append(c);
    }

    // 5. Return result.
    return result.to_string_without_validation();
}

// https://urlpattern.spec.whatwg.org/#escape-a-regexp-string
String escape_a_regexp_string(String const& input)
{
    // 1. Assert: input is an ASCII string.
    VERIFY(input.is_ascii());

    // 2. Let result be the empty string.
    StringBuilder builder;

    // 3. Let index be 0.
    // 4. While index is less than input’s length:
    for (auto c : input.bytes_as_string_view()) {
        // 1. Let c be input[index].
        // 2. Increment index by 1.

        // 3. If c is one of:
        //     * U+002E (.);
        //     * U+002B (+);
        //     * U+002A (*);
        //     * U+003F (?);
        //     * U+005E (^);
        //     * U+0024 ($);
        //     * U+007B ({);
        //     * U+007D (});
        //     * U+0028 (();
        //     * U+0029 ());
        //     * U+005B ([);
        //     * U+005D (]);
        //     * U+007C (|);
        //     * U+002F (/); or
        //     * U+005C (\),
        //    then append "\" to the end of result.
        if (".+*?^${}()[]|/\\"sv.contains(c))
            builder.append('\\');

        // 4. Append c to the end of result.
        builder.append(c);
    }

    // 5. Return result.
    return builder.to_string_without_validation();
}

// https://urlpattern.spec.whatwg.org/#generate-a-segment-wildcard-regexp
String generate_a_segment_wildcard_regexp(Options const& options)
{
    // 1. Let result be "[^".
    StringBuilder result;
    result.append("[^"sv);

    // 2. Append the result of running escape a regexp string given options’s delimiter code point to the end of result.
    if (options.delimiter_code_point.has_value())
        result.append(escape_a_regexp_string(String::from_code_point(*options.delimiter_code_point)));

    // 3. Append "]+?" to the end of result.
    result.append("]+?"sv);

    // 4. Return result.
    return result.to_string_without_validation();
}

// https://urlpattern.spec.whatwg.org/#generate-a-pattern-string
String generate_a_pattern_string(ReadonlySpan<Part> part_list, Options const& options)
{
    // 1. Let result be the empty string.
    StringBuilder result;

    // 2. Let index list be the result of getting the indices for part list.
    // 3. For each index of index list:
    for (size_t index = 0; index < part_list.size(); ++index) {
        // 1. Let part be part list[index].
        auto const& part = part_list[index];

        // 2. Let previous part be part list[index - 1] if index is greater than 0, otherwise let it be null.
        Part const* previous_part = index > 0 ? &part_list[index - 1] : nullptr;

        // 3. Let next part be part list[index + 1] if index is less than index list’s size - 1, otherwise let it be null.
        Part const* next_part = index + 1 < part_list.size() ? &part_list[index + 1] : nullptr;

        // 4. If part’s type is "fixed-text" then:
        if (part.type == Part::Type::FixedText) {
            // 1. If part’s modifier is "none" then:
            if (part.modifier == Part::Modifier::None) {
                // 1. Append the result of running escape a pattern string given part’s value to the end of result.
                result.append(escape_a_pattern_string(part.value));

                // 2. Continue.
                continue;
            }

            // 2. Append "{" to the end of result.
            result.append('{');

            // 3. Append the result of running escape a pattern string given part’s value to the end of result.
            result.append(escape_a_pattern_string(part.value));

            // 4. Append "}" to the end of result.
            result.append('}');

            // 5. Append the result of running convert a modifier to a string given part’s modifier to the end of result.
            result.append(Part::convert_modifier_to_string(part.modifier));

            // 6. Continue.
            continue;
        }

        // 5. Let custom name be true if part’s name[0] is not an ASCII digit; otherwise false.
        bool custom_name = !is_ascii_digit(part.name.bytes()[0]);

        // 6. Let needs grouping be true if at least one of the following are true, otherwise let it be false:
        //     * part’s suffix is not the empty string.
        //     * part’s prefix is not the empty string and is not options’s prefix code point.
        bool needs_grouping = !part.suffix.is_empty()
            || (!part.prefix.is_empty() && (options.prefix_code_point.has_value() && part.prefix != String::from_code_point(*options.prefix_code_point)));

        // 7. If all of the following are true:
        //     * needs grouping is false; and
        //     * custom name is true; and
        //     * part’s type is "segment-wildcard"; and
        //     * part’s modifier is "none"; and
        //     * next part is not null; and
        //     * next part’s prefix is the empty string; and
        //     * next part’s suffix is the empty string
        //    then:
        if (!needs_grouping
            && custom_name
            && part.type == Part::Type::SegmentWildcard
            && part.modifier == Part::Modifier::None
            && next_part != nullptr
            && next_part->prefix.is_empty()
            && next_part->suffix.is_empty()) {
            // 1. If next part’s type is "fixed-text":
            if (next_part->type == Part::Type::FixedText) {
                // 1. Set needs grouping to true if the result of running is a valid name code point given next part’s
                //    value's first code point and the boolean false is true.
                // FIXME: Raise spec bug, the language here is weird.
                needs_grouping = Tokenizer::is_a_valid_name_code_point(*next_part->value.code_points().begin(), false);
            }
            // 2. Otherwise:
            else {
                // 1. Set needs grouping to true if next part’s name[0] is an ASCII digit.
                needs_grouping = is_ascii_digit(*next_part->name.code_points().begin());
            }
        }

        // 8. If all of the following are true:
        //     * needs grouping is false; and
        //     * part’s prefix is the empty string; and
        //     * previous part is not null; and
        //     * previous part’s type is "fixed-text"; and
        //     * previous part’s value's last code point is options’s prefix code point.
        //    then set needs grouping to true.
        if (!needs_grouping
            && part.prefix.is_empty()
            && previous_part != nullptr
            && previous_part->type == Part::Type::FixedText
            && ((previous_part->value.is_empty() && !options.prefix_code_point.has_value())
                || (options.prefix_code_point.has_value() && previous_part->value == String::from_code_point(*options.prefix_code_point)))) {
            needs_grouping = true;
        }

        // 9. Assert: part’s name is not the empty string or null.
        VERIFY(!part.name.is_empty());

        // 10. If needs grouping is true, then append "{" to the end of result.
        if (needs_grouping)
            result.append('{');

        // 11. Append the result of running escape a pattern string given part’s prefix to the end of result.
        result.append(escape_a_pattern_string(part.prefix));

        // 12. If custom name is true:
        if (custom_name) {
            // 1. Append ":" to the end of result.
            result.append(':');

            // 2. Append part’s name to the end of result.
            result.append(part.name);
        }

        // 13. If part’s type is "regexp" then:
        if (part.type == Part::Type::Regexp) {
            // 1. Append "(" to the end of result.
            result.append('(');

            // 2. Append part’s value to the end of result.
            result.append(part.value);

            // 3. Append ")" to the end of result.
            result.append(')');
        }
        // 14. Otherwise if part’s type is "segment-wildcard" and custom name is false:
        else if (part.type == Part::Type::SegmentWildcard && !custom_name) {
            // 1. Append "(" to the end of result.
            result.append('(');

            // 2. Append the result of running generate a segment wildcard regexp given options to the end of result.
            result.append(generate_a_segment_wildcard_regexp(options));

            // 3. Append ")" to the end of result.
            result.append(')');
        }
        // 15. Otherwise if part’s type is "full-wildcard":
        else if (part.type == Part::Type::FullWildcard) {
            // 1. If custom name is false and one of the following is true:
            //     * previous part is null; or
            //     * previous part’s type is "fixed-text"; or
            //     * previous part’s modifier is not "none"; or
            //     * needs grouping is true; or
            //     * part’s prefix is not the empty string
            //    then append "*" to the end of result.
            if (!custom_name
                && (previous_part == nullptr
                    || previous_part->type == Part::Type::FixedText
                    || previous_part->modifier != Part::Modifier::None
                    || needs_grouping
                    || !part.prefix.is_empty())) {
                result.append('*');
            }
            // 2. Otherwise:
            else {
                // 1. Append "(" to the end of result.
                result.append('(');

                // 2. Append full wildcard regexp value to the end of result.
                result.append(full_wildcard_regexp_value);

                // 3. Append ")" to the end of result.
                result.append(')');
            }
        }

        // 16. If all of the following are true:
        //      * part’s type is "segment-wildcard"; and
        //      * custom name is true; and
        //      * part’s suffix is not the empty string; and
        //      * The result of running is a valid name code point given part’s suffix's first code point and the boolean false is true
        //     then append U+005C (\) to the end of result.
        if (part.type == Part::Type::SegmentWildcard
            && custom_name
            && !part.suffix.is_empty()
            && Tokenizer::is_a_valid_name_code_point(*part.suffix.code_points().begin(), false)) {
            result.append('\\');
        }

        // 17. Append the result of running escape a pattern string given part’s suffix to the end of result.
        result.append(escape_a_pattern_string(part.suffix));

        // 18. If needs grouping is true, then append "}" to the end of result.
        if (needs_grouping)
            result.append('}');

        // 19. Append the result of running convert a modifier to a string given part’s modifier to the end of result.
        result.append(Part::convert_modifier_to_string(part.modifier));
    }

    // 4. Return result.
    return result.to_string_without_validation();
}

}
