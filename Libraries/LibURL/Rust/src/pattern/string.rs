/*
 * Copyright (c) 2025-2026, Shannon Booth <shannon@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::pattern::Options;
use crate::pattern::Part;
use crate::pattern::part::Type as PartType;
use crate::pattern::tokenizer::Tokenizer;

#[allow(non_upper_case_globals)]
// https://urlpattern.spec.whatwg.org/#full-wildcard-regexp-value
pub const full_wildcard_regexp_value: &str = ".*";

// https://urlpattern.spec.whatwg.org/#escape-a-pattern-string
pub fn escape_a_pattern_string(input: &str) -> String {
    // 1. Assert: input is an ASCII string.
    assert!(input.is_ascii());

    // 2. Let result be the empty string.
    let mut result = String::new();

    // 3. Let index be 0.
    // 4. While index is less than input’s length:
    for c in input.chars() {
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
        if "+*?:{}()\\".contains(c) {
            result.push('\\');
        }

        // 4. Append c to the end of result.
        result.push(c);
    }

    // 5. Return result.
    result
}

// https://urlpattern.spec.whatwg.org/#escape-a-regexp-string
pub fn escape_a_regexp_string(input: &str) -> String {
    // 1. Assert: input is an ASCII string.
    assert!(input.is_ascii());

    // 2. Let result be the empty string.
    let mut result = String::new();

    // 3. Let index be 0.
    // 4. While index is less than input’s length:
    for c in input.chars() {
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
        if ".+*?^${}()[]|/\\".contains(c) {
            result.push('\\');
        }

        // 4. Append c to the end of result.
        result.push(c);
    }

    // 5. Return result.
    result
}

// https://urlpattern.spec.whatwg.org/#generate-a-segment-wildcard-regexp
pub fn generate_a_segment_wildcard_regexp(options: &Options) -> String {
    // 1. Let result be "[^".
    let mut result = String::from("[^");

    // 2. Append the result of running escape a regexp string given options’s delimiter code point to the end of result.
    if let Some(delimiter_code_point) = options.delimiter_code_point {
        result.push_str(&escape_a_regexp_string(&delimiter_code_point.to_string()));
    }

    // 3. Append "]+?" to the end of result.
    result.push_str("]+?");

    // 4. Return result.
    result
}

// https://urlpattern.spec.whatwg.org/#generate-a-pattern-string
pub fn generate_a_pattern_string(part_list: &[Part], options: &Options) -> String {
    // 1. Let result be the empty string.
    let mut result = String::new();

    // 2. Let index list be the result of getting the indices for part list.
    // 3. For each index of index list:
    for index in 0..part_list.len() {
        // 1. Let part be part list[index].
        let part = &part_list[index];

        // 2. Let previous part be part list[index - 1] if index is greater than 0, otherwise let it be null.
        let previous_part = if index > 0 { Some(&part_list[index - 1]) } else { None };

        // 3. Let next part be part list[index + 1] if index is less than index list’s size - 1, otherwise let it be null.
        let next_part = if index + 1 < part_list.len() {
            Some(&part_list[index + 1])
        } else {
            None
        };

        // 4. If part’s type is "fixed-text" then:
        if part.r#type == PartType::FixedText {
            // 1. If part’s modifier is "none" then:
            if part.modifier == crate::pattern::part::Modifier::None {
                // 1. Append the result of running escape a pattern string given part’s value to the end of result.
                result.push_str(&escape_a_pattern_string(&part.value));

                // 2. Continue.
                continue;
            }

            // 2. Append "{" to the end of result.
            result.push('{');

            // 3. Append the result of running escape a pattern string given part’s value to the end of result.
            result.push_str(&escape_a_pattern_string(&part.value));

            // 4. Append "}" to the end of result.
            result.push('}');

            // 5. Append the result of running convert a modifier to a string given part’s modifier to the end of result.
            result.push_str(Part::convert_modifier_to_string(part.modifier));

            // 6. Continue.
            continue;
        }

        // 5. Let custom name be true if part’s name[0] is not an ASCII digit; otherwise false.
        let custom_name = !part.name.as_bytes()[0].is_ascii_digit();

        // 6. Let needs grouping be true if at least one of the following are true, otherwise let it be false:
        //     * part’s suffix is not the empty string.
        //     * part’s prefix is not the empty string and is not options’s prefix code point.
        let mut needs_grouping = !part.suffix.is_empty()
            || (!part.prefix.is_empty()
                && (options.prefix_code_point.is_some()
                    && part.prefix != options.prefix_code_point.unwrap().to_string()));

        // 7. If all of the following are true:
        //     * needs grouping is false; and
        //     * custom name is true; and
        //     * part’s type is "segment-wildcard"; and
        //     * part’s modifier is "none"; and
        //     * next part is not null; and
        //     * next part’s prefix is the empty string; and
        //     * next part’s suffix is the empty string
        //    then:
        if !needs_grouping
            && custom_name
            && part.r#type == PartType::SegmentWildcard
            && part.modifier == crate::pattern::part::Modifier::None
            && let Some(next_part) = next_part
            && next_part.prefix.is_empty()
            && next_part.suffix.is_empty()
        {
            // 1. If next part’s type is "fixed-text":
            if next_part.r#type == PartType::FixedText {
                // 1. Set needs grouping to true if the result of running is a valid name code point given next part’s
                //    value's first code point and the boolean false is true.
                needs_grouping = next_part
                    .value
                    .chars()
                    .next()
                    .is_some_and(|code_point| Tokenizer::is_a_valid_name_code_point(code_point as u32, false));
            }
            // 2. Otherwise:
            else {
                // 1. Set needs grouping to true if next part’s name[0] is an ASCII digit.
                needs_grouping = next_part.name.as_bytes()[0].is_ascii_digit();
            }
        }

        // 8. If all of the following are true:
        //     * needs grouping is false; and
        //     * part’s prefix is the empty string; and
        //     * previous part is not null; and
        //     * previous part’s type is "fixed-text"; and
        //     * previous part’s value's last code point is options’s prefix code point.
        //    then set needs grouping to true.
        if !needs_grouping
            && part.prefix.is_empty()
            && previous_part.is_some()
            && previous_part.unwrap().r#type == PartType::FixedText
            && ((previous_part.unwrap().value.is_empty() && options.prefix_code_point.is_none())
                || (options.prefix_code_point.is_some()
                    && previous_part.unwrap().value == options.prefix_code_point.unwrap().to_string()))
        {
            needs_grouping = true;
        }

        // 9. Assert: part’s name is not the empty string or null.
        assert!(!part.name.is_empty());

        // 10. If needs grouping is true, then append "{" to the end of result.
        if needs_grouping {
            result.push('{');
        }

        // 11. Append the result of running escape a pattern string given part’s prefix to the end of result.
        result.push_str(&escape_a_pattern_string(&part.prefix));

        // 12. If custom name is true:
        if custom_name {
            // 1. Append ":" to the end of result.
            result.push(':');

            // 2. Append part’s name to the end of result.
            result.push_str(&part.name);
        }

        // 13. If part’s type is "regexp" then:
        if part.r#type == PartType::Regexp {
            // 1. Append "(" to the end of result.
            result.push('(');

            // 2. Append part’s value to the end of result.
            result.push_str(&part.value);

            // 3. Append ")" to the end of result.
            result.push(')');
        }
        // 14. Otherwise if part’s type is "segment-wildcard" and custom name is false:
        else if part.r#type == PartType::SegmentWildcard && !custom_name {
            // 1. Append "(" to the end of result.
            result.push('(');

            // 2. Append the result of running generate a segment wildcard regexp given options to the end of result.
            result.push_str(&generate_a_segment_wildcard_regexp(options));

            // 3. Append ")" to the end of result.
            result.push(')');
        }
        // 15. Otherwise if part’s type is "full-wildcard":
        else if part.r#type == PartType::FullWildcard {
            // 1. If custom name is false and one of the following is true:
            //     * previous part is null; or
            //     * previous part’s type is "fixed-text"; or
            //     * previous part’s modifier is not "none"; or
            //     * needs grouping is true; or
            //     * part’s prefix is not the empty string
            //    then append "*" to the end of result.
            if !custom_name
                && (previous_part.is_none()
                    || previous_part.unwrap().r#type == PartType::FixedText
                    || previous_part.unwrap().modifier != crate::pattern::part::Modifier::None
                    || needs_grouping
                    || !part.prefix.is_empty())
            {
                result.push('*');
            }
            // 2. Otherwise:
            else {
                // 1. Append "(" to the end of result.
                result.push('(');

                // 2. Append full wildcard regexp value to the end of result.
                result.push_str(full_wildcard_regexp_value);

                // 3. Append ")" to the end of result.
                result.push(')');
            }
        }

        // 16. If all of the following are true:
        //      * part’s type is "segment-wildcard"; and
        //      * custom name is true; and
        //      * part’s suffix is not the empty string; and
        //      * The result of running is a valid name code point given part’s suffix's first code point and the boolean false is true
        //     then append U+005C (\) to the end of result.
        if part.r#type == PartType::SegmentWildcard
            && custom_name
            && !part.suffix.is_empty()
            && part
                .suffix
                .chars()
                .next()
                .is_some_and(|code_point| Tokenizer::is_a_valid_name_code_point(code_point as u32, false))
        {
            result.push('\\');
        }

        // 17. Append the result of running escape a pattern string given part’s suffix to the end of result.
        result.push_str(&escape_a_pattern_string(&part.suffix));

        // 18. If needs grouping is true, then append "}" to the end of result.
        if needs_grouping {
            result.push('}');
        }

        // 19. Append the result of running convert a modifier to a string given part’s modifier to the end of result.
        result.push_str(Part::convert_modifier_to_string(part.modifier));
    }

    // 4. Return result.
    result
}
